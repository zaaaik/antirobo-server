const mongoose = require('mongoose');

// Una suscripción push por dispositivo/navegador (lo que devuelve
// PushManager.subscribe() en el navegador). "endpoint" es único por
// dispositivo, así que lo usamos para no duplicar.
const suscripcionSchema = new mongoose.Schema({
  endpoint: { type: String, required: true, unique: true },
  keys: {
    p256dh: { type: String, required: true },
    auth: { type: String, required: true }
  },
  fecha: { type: Date, default: Date.now }
});

module.exports = mongoose.model('Suscripcion', suscripcionSchema);
