require('dotenv').config();

const express = require('express');
const cors = require('cors');
const path = require('path');
const http = require('http');
const cookie = require('cookie');
const cookieParser = require('cookie-parser');
const bcrypt = require('bcryptjs');
const mongoose = require('mongoose');
const jwt = require('jsonwebtoken');
const { Server } = require('socket.io');
const webpush = require('web-push');

const User = require('./models/User');
const Persona = require('./models/Persona');
const Suscripcion = require('./models/Suscripcion');
const { crearToken, requireAuth, JWT_SECRET } = require('./middleware/auth');

const app = express();
const servidorHttp = http.createServer(app);
const io = new Server(servidorHttp);
const PORT = process.env.PORT || 3000;

// Render está detrás de un proxy HTTPS; esto hace que req.secure y
// la cookie "secure" funcionen bien ahí.
app.set('trust proxy', 1);

app.use(cors());
app.use(express.json({ limit: '2mb' })); // las fotos de la cámara van en base64 acá
app.use(cookieParser());

// ---------------- NOTIFICACIONES PUSH ----------------
if (process.env.VAPID_PUBLIC_KEY && process.env.VAPID_PRIVATE_KEY) {
  webpush.setVapidDetails(
    'mailto:no-reply@antirobo.local',
    process.env.VAPID_PUBLIC_KEY,
    process.env.VAPID_PRIVATE_KEY
  );
} else {
  console.warn('VAPID_PUBLIC_KEY / VAPID_PRIVATE_KEY no configurados: las notificaciones push no van a funcionar.');
}

// Le manda la notificación a todos los dispositivos suscriptos. Si algún
// endpoint ya no es válido (el usuario desinstaló, borró permisos, etc.),
// lo borramos de la base para no seguir intentando en vano.
async function notificarATodos(titulo, cuerpo) {
  if (!process.env.VAPID_PUBLIC_KEY) {
    return { enviados: 0, errores: ['El servidor no tiene configuradas las claves VAPID.'] };
  }

  const suscripciones = await Suscripcion.find();
  const payload = JSON.stringify({ title: titulo, body: cuerpo, url: '/' });
  let enviados = 0;
  const errores = [];

  await Promise.all(suscripciones.map(async (s) => {
    try {
      await webpush.sendNotification(
        { endpoint: s.endpoint, keys: s.keys },
        payload
      );
      enviados++;
    } catch (err) {
      if (err.statusCode === 404 || err.statusCode === 410) {
        await Suscripcion.deleteOne({ _id: s._id });
        errores.push('Una suscripción vencida se eliminó (código ' + err.statusCode + ').');
      } else {
        console.error('Error enviando notificación push:', err.statusCode, err.body || err.message);
        errores.push('Código ' + err.statusCode + ': ' + (err.body || err.message));
      }
    }
  }));

  return { totalSuscripciones: suscripciones.length, enviados, errores };
}

// ---------------- BASE DE DATOS ----------------
if (process.env.MONGODB_URI) {
  mongoose.connect(process.env.MONGODB_URI)
    .then(() => {
      console.log('Conectado a MongoDB.');
      asegurarAdmin();
    })
    .catch(err => console.error('Error conectando a MongoDB:', err.message));
} else {
  console.warn('MONGODB_URI no está configurado: el login no va a funcionar hasta que lo configures (ver .env.example).');
}

// Crea el usuario admin la primera vez que arranca, si todavía no existe ninguno.
async function asegurarAdmin() {
  const existente = await User.findOne();
  if (existente) return;

  const usuario = process.env.ADMIN_USER;
  const password = process.env.ADMIN_PASSWORD;
  if (!usuario || !password) {
    console.warn('ADMIN_USER / ADMIN_PASSWORD no configurados: no se pudo crear el usuario admin.');
    return;
  }

  const passwordHash = await bcrypt.hash(password, 10);
  await User.create({ usuario, passwordHash });
  console.log(`Usuario admin "${usuario}" creado.`);
}

// ---------------- LOGIN ----------------
app.get('/login.html', (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'login.html'));
});

app.post('/api/login', async (req, res) => {
  const { usuario, password } = req.body || {};
  if (!usuario || !password) {
    return res.status(400).json({ ok: false, error: 'Faltan usuario o contraseña.' });
  }

  const user = await User.findOne({ usuario });
  const valido = user && await bcrypt.compare(password, user.passwordHash);
  if (!valido) {
    return res.status(401).json({ ok: false, error: 'Usuario o contraseña incorrectos.' });
  }

  const token = crearToken(user.usuario);
  res.cookie('token', token, {
    httpOnly: true,
    sameSite: 'lax',
    secure: process.env.NODE_ENV === 'production',
    maxAge: 7 * 24 * 60 * 60 * 1000
  });
  res.json({ ok: true });
});

app.post('/api/logout', (req, res) => {
  res.clearCookie('token');
  res.json({ ok: true });
});

// ---------------- PÁGINA PRINCIPAL (protegida) ----------------
app.get(['/', '/index.html'], requireAuth, (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

// Página que abre el teléfono que hace de cámara (también requiere login)
app.get('/camara-emisor.html', requireAuth, (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'camara-emisor.html'));
});

// Los archivos de vendor/ (librerías + modelos de IA) nunca cambian una vez
// publicados, así que le decimos al navegador que los guarde en caché mucho
// tiempo — evita volver a descargar ~7MB cada vez que se abre la pestaña Cámara.
app.use('/vendor', express.static(path.join(__dirname, 'public', 'vendor'), {
  maxAge: '30d',
  immutable: true
}));

app.use(express.static(path.join(__dirname, 'public'), { index: false }));

// ---------------- ESTADO EN MEMORIA ----------------
let estado = {
  sonido: 0,
  luz: 0,
  luzUmbral: null,   // umbral de "sin luz" calibrado por el Arduino al arrancar
  distancia: 0,
  alarma: false,
  confirmadas: 0,
  objetivo: 20,
  alertas: 0,
  segundos: 0,
  ultimaActualizacion: null
};

const MAX_EVENTOS = 20;
let eventos = [];

// ---------------- CÁMARA ----------------
// Nota: el video en vivo va por WebRTC (más abajo), esto es solo para la
// foto puntual que se guarda cuando se dispara una alarma.
let ultimaFoto = null;   // { foto: "data:image/jpeg;base64,...", fecha: ISOString }

// ---------------- RUTAS ----------------

// El Arduino envía datos aquí (POST)
app.post('/api/datos', (req, res) => {
  const { sonido, luz, luzUmbral, distancia, alarma, confirmadas, objetivo, alertas, segundos } = req.body;

  const alarmaEstabaActiva = estado.alarma; // para detectar cuándo ARRANCA una alarma nueva

  estado = {
    sonido: sonido ?? estado.sonido,
    luz: luz ?? estado.luz,
    luzUmbral: luzUmbral ?? estado.luzUmbral,
    distancia: distancia ?? estado.distancia,
    alarma: alarma ?? estado.alarma,
    confirmadas: confirmadas ?? estado.confirmadas,
    objetivo: objetivo ?? estado.objetivo,
    alertas: alertas ?? estado.alertas,
    segundos: segundos ?? estado.segundos,
    ultimaActualizacion: new Date().toISOString()
  };

  // Si es una alerta NUEVA (el Arduino sigue mandando alarma:true varias
  // veces mientras dura la misma alerta), la guardamos en el historial y
  // avisamos por notificación push — una sola vez por evento, no en cada POST.
  if (alarma && !alarmaEstabaActiva) {
    eventos.unshift({
      fecha: new Date().toISOString(),
      sonido: estado.sonido,
      luz: estado.luz,
      distancia: estado.distancia,
      foto: null // se completa cuando llegue una foto de la cámara mientras esta alarma esté activa
    });
    if (eventos.length > MAX_EVENTOS) eventos.pop();

    notificarATodos('⚠️ Alerta de intrusión', 'Sonido: ' + estado.sonido + ' · Luz: ' + estado.luz + ' · Distancia: ' + estado.distancia + ' cm')
      .catch(err => console.error('Error al notificar:', err.message));
  }

  console.log('Datos recibidos:', estado);
  res.json({ ok: true });
});

// La página web pide los datos actuales aquí (GET) — requiere login
app.get('/api/datos', requireAuth, (req, res) => {
  res.json({ ...estado, eventos });
});

// Ruta de salud, para confirmar que el servidor está vivo
app.get('/api/ping', (req, res) => {
  res.json({ ok: true, mensaje: 'Servidor activo' });
});

// ---------------- NOTIFICACIONES PUSH ----------------

// La clave pública la necesita el navegador para suscribirse.
app.get('/api/notificaciones/clave-publica', requireAuth, (req, res) => {
  res.json({ clave: process.env.VAPID_PUBLIC_KEY || null });
});

app.post('/api/notificaciones/suscribir', requireAuth, async (req, res) => {
  const { endpoint, keys } = req.body || {};
  if (!endpoint || !keys || !keys.p256dh || !keys.auth) {
    return res.status(400).json({ ok: false, error: 'Suscripción inválida.' });
  }

  await Suscripcion.findOneAndUpdate(
    { endpoint },
    { endpoint, keys },
    { upsert: true }
  );
  res.json({ ok: true });
});

app.post('/api/notificaciones/desuscribir', requireAuth, async (req, res) => {
  const { endpoint } = req.body || {};
  if (endpoint) await Suscripcion.deleteOne({ endpoint });
  res.json({ ok: true });
});

// Botón "Probar" desde el dashboard, para confirmar que las notificaciones llegan.
app.post('/api/notificaciones/probar', requireAuth, async (req, res) => {
  const resultado = await notificarATodos('🔔 Notificación de prueba', 'Si ves esto, las notificaciones están funcionando.');
  res.json({ ok: true, ...resultado });
});

// ---------------- CÁMARA: foto de respaldo para el historial de alarmas ----------------
// (el video en vivo va aparte, por WebRTC, más abajo)
app.post('/api/camara/foto', requireAuth, (req, res) => {
  const { foto } = req.body || {};
  if (!foto || typeof foto !== 'string' || !foto.startsWith('data:image/')) {
    return res.status(400).json({ ok: false, error: 'Falta la foto o el formato no es válido.' });
  }

  ultimaFoto = { foto, fecha: new Date().toISOString() };

  // Si hay una alarma en curso y ese evento todavía no tiene foto, se la asignamos.
  if (estado.alarma && eventos.length > 0 && !eventos[0].foto) {
    eventos[0].foto = foto;
  }

  res.json({ ok: true });
});

// ---------------- RECONOCIMIENTO FACIAL: personas conocidas ----------------
// El reconocimiento en sí corre en el navegador de quien mira el dashboard
// (con face-api.js, analizando el video que ya está recibiendo). Acá solo
// se guardan/consultan las "huellas faciales" (128 números, no fotos) de
// las personas que se registraron como conocidas.

// Lista de personas conocidas (con su descriptor, para comparar en el navegador).
app.get('/api/personas', requireAuth, async (req, res) => {
  const personas = await Persona.find().select('nombre descriptor');
  res.json(personas);
});

// Registra una persona nueva a partir de un descriptor ya calculado en el navegador.
app.post('/api/personas', requireAuth, async (req, res) => {
  const { nombre, descriptor } = req.body || {};
  if (!nombre || typeof nombre !== 'string' || !nombre.trim()) {
    return res.status(400).json({ ok: false, error: 'Falta el nombre.' });
  }
  if (!Array.isArray(descriptor) || descriptor.length !== 128) {
    return res.status(400).json({ ok: false, error: 'El descriptor facial no es válido.' });
  }

  const persona = await Persona.create({ nombre: nombre.trim(), descriptor });
  res.json({ ok: true, persona: { _id: persona._id, nombre: persona.nombre } });
});

// Borra una persona conocida.
app.delete('/api/personas/:id', requireAuth, async (req, res) => {
  await Persona.findByIdAndDelete(req.params.id);
  res.json({ ok: true });
});

// ================ WEBRTC: SEÑALIZACIÓN (video en vivo) ================
// El servidor acá no ve ni un segundo de video: solo ayuda a que el
// teléfono-cámara y el dashboard "se encuentren" y negocien la conexión
// directa entre ellos (peer-to-peer). Una vez conectados, el video viaja
// directo entre los dos dispositivos.

// Solo se acepta la conexión de Socket.IO si trae una cookie de sesión válida
// (la misma que usa el login normal).
io.use((socket, next) => {
  try {
    const cookies = cookie.parse(socket.handshake.headers.cookie || '');
    const token = cookies.token;
    if (!token) return next(new Error('No autenticado'));
    jwt.verify(token, JWT_SECRET);
    next();
  } catch (err) {
    next(new Error('No autenticado'));
  }
});

let socketCamara = null; // el socket del teléfono que hace de cámara (solo puede haber uno a la vez)

io.on('connection', (socket) => {
  // El teléfono-cámara se anuncia acá.
  socket.on('soy-camara', () => {
    socketCamara = socket.id;
  });

  // El dashboard pide ver la cámara: se lo avisamos al teléfono-cámara
  // para que le arme una conexión (oferta) a este viewer en particular.
  socket.on('quiero-ver', () => {
    if (socketCamara) {
      io.to(socketCamara).emit('nuevo-viewer', { viewerId: socket.id });
    } else {
      socket.emit('camara-desconectada');
    }
  });

  // Reenvíos de la negociación WebRTC (oferta / respuesta / candidatos ICE):
  // el servidor no entiende ni toca el contenido, solo lo pasa al destinatario.
  // La oferta siempre sale de la cámara hacia un viewer puntual:
  socket.on('oferta', ({ viewerId, offer }) => {
    io.to(viewerId).emit('oferta', { offer });
  });
  // La respuesta siempre sale de un viewer hacia la cámara (el viewer no
  // sabe el id de la cámara, así que el servidor lo resuelve):
  socket.on('respuesta', ({ answer }) => {
    if (socketCamara) io.to(socketCamara).emit('respuesta', { answer, viewerId: socket.id });
  });
  // Los candidatos ICE viajan en los dos sentidos, según quién los mande:
  socket.on('ice-candidato', ({ viewerId, candidate }) => {
    if (socket.id === socketCamara) {
      io.to(viewerId).emit('ice-candidato', { candidate });
    } else if (socketCamara) {
      io.to(socketCamara).emit('ice-candidato', { candidate, viewerId: socket.id });
    }
  });

  socket.on('disconnect', () => {
    if (socket.id === socketCamara) {
      socketCamara = null;
      socket.broadcast.emit('camara-desconectada');
    } else {
      // Si era un viewer, avisamos a la cámara para que cierre esa conexión puntual.
      if (socketCamara) io.to(socketCamara).emit('viewer-desconectado', { viewerId: socket.id });
    }
  });
});

servidorHttp.listen(PORT, () => {
  console.log(`Servidor corriendo en el puerto ${PORT}`);
});