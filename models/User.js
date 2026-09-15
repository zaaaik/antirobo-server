const mongoose = require('mongoose');

// Un solo tipo de usuario: el admin que ve el dashboard.
// (No hay pantalla de registro público a propósito.)
const userSchema = new mongoose.Schema({
  usuario: { type: String, required: true, unique: true, trim: true },
  passwordHash: { type: String, required: true }
});

module.exports = mongoose.model('User', userSchema);
