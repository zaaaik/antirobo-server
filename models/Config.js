const mongoose = require('mongoose');

// Configuración general guardada en la base (un solo documento).
// Por ahora sólo el número al que se manda el SMS de alarma.
// Las credenciales de Twilio NO van acá: viven en variables de entorno.
const configSchema = new mongoose.Schema({
  _id: { type: String, default: 'general' },
  smsTelefono: { type: String, default: '' }
});

module.exports = mongoose.model('Config', configSchema);
