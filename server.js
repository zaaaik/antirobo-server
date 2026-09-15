require('dotenv').config();

const express = require('express');
const cors = require('cors');
const path = require('path');
const cookieParser = require('cookie-parser');
const bcrypt = require('bcryptjs');
const mongoose = require('mongoose');

const User = require('./models/User');
const { crearToken, requireAuth } = require('./middleware/auth');

const app = express();
const PORT = process.env.PORT || 3000;

// Render está detrás de un proxy HTTPS; esto hace que req.secure y
// la cookie "secure" funcionen bien ahí.
app.set('trust proxy', 1);

app.use(cors());
app.use(express.json());
app.use(cookieParser());

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

// ---------------- RUTAS ----------------

// El Arduino envía datos aquí (POST)
app.post('/api/datos', (req, res) => {
  const { sonido, luz, luzUmbral, distancia, alarma, confirmadas, objetivo, alertas, segundos } = req.body;

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

  // Si es una alerta nueva, la guardamos en el historial
  if (alarma) {
    eventos.unshift({
      fecha: new Date().toISOString(),
      sonido: estado.sonido,
      luz: estado.luz,
      distancia: estado.distancia
    });
    if (eventos.length > MAX_EVENTOS) eventos.pop();
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

app.listen(PORT, () => {
  console.log(`Servidor corriendo en el puerto ${PORT}`);
});