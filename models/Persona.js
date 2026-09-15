const mongoose = require('mongoose');

// Una "persona conocida" para el reconocimiento facial: guarda su nombre
// y la huella facial (descriptor de 128 números que genera face-api.js),
// no una foto. Se registra solo con consentimiento de la persona.
const personaSchema = new mongoose.Schema({
  nombre: { type: String, required: true, trim: true },
  descriptor: {
    type: [Number],
    required: true,
    validate: v => Array.isArray(v) && v.length === 128
  },
  fecha: { type: Date, default: Date.now }
});

module.exports = mongoose.model('Persona', personaSchema);
