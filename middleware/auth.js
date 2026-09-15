const jwt = require('jsonwebtoken');

// En producción SIEMPRE debe venir de la variable de entorno JWT_SECRET.
// El valor de acá solo es para poder correr el server localmente sin configurar nada.
const JWT_SECRET = process.env.JWT_SECRET || 'dev-secret-cambiar-en-produccion';

function crearToken(usuario) {
  return jwt.sign({ usuario }, JWT_SECRET, { expiresIn: '7d' });
}

// Protege rutas: exige una cookie "token" válida.
// Si la petición es a la API responde 401 JSON; si es una página, redirige al login.
function requireAuth(req, res, next) {
  const token = req.cookies && req.cookies.token;

  if (!token) return rechazar(req, res);

  try {
    req.usuario = jwt.verify(token, JWT_SECRET).usuario;
    next();
  } catch (err) {
    return rechazar(req, res);
  }
}

function rechazar(req, res) {
  if (req.path.startsWith('/api/')) {
    return res.status(401).json({ ok: false, error: 'No autenticado' });
  }
  res.redirect('/login.html');
}

module.exports = { crearToken, requireAuth, JWT_SECRET };
