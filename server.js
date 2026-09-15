const express = require('express');
const cors = require('cors');
const path = require('path');

const app = express();
const PORT = process.env.PORT || 3000;

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ---------------- ESTADO EN MEMORIA ----------------
let estado = {
  sonido: 0,
  luz: 0,
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
  const { sonido, luz, distancia, alarma, confirmadas, objetivo, alertas, segundos } = req.body;

  estado = {
    sonido: sonido ?? estado.sonido,
    luz: luz ?? estado.luz,
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

// La página web pide los datos actuales aquí (GET)
app.get('/api/datos', (req, res) => {
  res.json({ ...estado, eventos });
});

// Ruta de salud, para confirmar que el servidor está vivo
app.get('/api/ping', (req, res) => {
  res.json({ ok: true, mensaje: 'Servidor activo' });
});

app.listen(PORT, () => {
  console.log(`Servidor corriendo en el puerto ${PORT}`);
});