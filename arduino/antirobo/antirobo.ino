/* ============================================================
   SISTEMA ANTIROBO — ARDUINO UNO R4 WiFi
   ============================================================
   Envía los datos a un backend en Render. Se ve desde
   cualquier lugar con internet, no solo en la red local.

   CONEXIONES:
     HW-485 (sonido)     AO -> A0    DO -> D2
     HW-486 (luz)        AO -> A1
     HC-SR04 (distancia) Trig -> D9  Echo -> D10

   Sin LCD y sin LED: todo el feedback pasa por la página web
   (public/index.html), que se entera de cada cambio por
   socket.io en tiempo real.

   ------------------------------------------------------------
   POR QUÉ NO HAY NINGÚN UMBRAL FIJO
   ------------------------------------------------------------
   Estos módulos no vienen calibrados de fábrica: el MISMO estado
   físico (silencio, luz normal) se lee 600 en un módulo y 300 en
   otro. Un número absoluto tipo "dispara sobre 410" solo sirve
   para el módulo con el que se escribió, y hay que rehacerlo cada
   vez que cambian el hardware.

   Por eso el sketch no compara contra números: al arrancar mide el
   REPOSO y la FLUCTUACIÓN propios de este módulo y deriva de ahí
   sus umbrales (ver calcularUmbral). Se necesitan ~2 segundos de
   silencio y luz normal al encender, y nada más.

   Los umbrales resultantes viajan al servidor en cada POST como
   "sonidoUmbral" y "luzUmbral", así que la página web muestra el
   umbral real de hoy sin tener que saberlo de antemano.

   DISTANCIA_ALERTA sí es fija (50), porque son centímetros reales
   medidos por tiempo de eco, no cuentas de ADC: eso no cambia
   entre módulos. La página lo conoce como DISTANCIA_UMBRAL.

   ------------------------------------------------------------
   POR QUÉ EL LOOP NO SE CUELGA
   ------------------------------------------------------------
   En el UNO R4 el WiFi es un ESP32-S3 separado al que se le habla
   por UART: CADA llamada de red (write, available, read, status)
   es un viaje de ida y vuelta a ese chip. Ese es el recurso caro
   del sistema, y todo el diseño gira alrededor de gastar la menor
   cantidad posible de esos viajes:

   1) UN SOLO write() POR ENVÍO: la petición HTTP entera (headers
      + JSON) se arma en un buffer de char y se manda de una. Con
      print(F("...")) el core manda UN BYTE POR LLAMADA, o sea
      ~290 viajes al ESP32 por POST; así es 1.

   2) LA RESPUESTA SE VERIFICA DESPUÉS, NUNCA SE ESPERA: se lee al
      principio del envío siguiente, y solo lo que ya llegó
      (available() > 0). Nunca se bloquea esperando al servidor,
      pero igual se sabe con qué código HTTP contestó.

   3) WiFi.status() SE CACHEA: se consulta una vez por vuelta y se
      reusa, en vez de preguntarle al módem 3 o 4 veces.

   4) BACKOFF EN TODO LO QUE PUEDE BLOQUEAR: WiFi.begin() y
      connect() son bloqueantes por dentro y no hay forma de
      interrumpirlos desde el sketch. Lo único que se puede hacer
      es llamarlos cada vez menos seguido cuando vienen fallando,
      y eso es exactamente lo que hace el backoff exponencial.

   El log de [t] (activalo con DEBUG_TIMING) mide cada etapa por
   separado para poder ver, si alguna vez vuelve a tardar, dónde
   se fue el tiempo exactamente.
   ============================================================ */

#include <WiFiS3.h>

// 1 = imprime una línea de diagnóstico por vuelta. 0 = log mínimo.
#define DEBUG_TIMING 1

// ---------------- WIFI ----------------
const char* WIFI_SSID = "iPhone de Diego";
const char* WIFI_PASS = "Diego123";

// ---------------- SERVIDOR RENDER ----------------
const char* SERVIDOR_HOST = "antirobo-server.onrender.com";
const char* SERVIDOR_RUTA = "/api/datos";
const uint16_t SERVIDOR_PUERTO = 443;   // HTTPS

// ---------------- PINES ----------------
const uint8_t PIN_SONIDO     = A0;
const uint8_t PIN_SONIDO_DO  = 2;
const uint8_t PIN_LUZ        = A1;
const uint8_t PIN_TRIG       = 9;
const uint8_t PIN_ECHO       = 10;

const bool HC_SR04_ACTIVO = true;

// ---------------- DETECCIÓN ----------------
const uint16_t DISTANCIA_ALERTA = 50;   // cm reales: esto NO depende del modulo
const uint8_t  MUESTRAS_CALIB   = 50;
const uint8_t  CONFIRMACIONES   = 3;    // lecturas CONSECUTIVAS para disparar
const uint16_t DURACION_ALARMA  = 5000;

// NINGÚN umbral es un número fijo. Cada módulo tiene su propio nivel de
// reposo: uno marca 600 en silencio y otro marca 300 para exactamente el
// mismo estado físico, así que un valor absoluto solo sirve para el módulo
// con el que se escribió. Todos los umbrales se calculan al arrancar sobre
// lo que mide ESTE módulo:
//
//   umbral = reposo + max(K * fluctuacion, reposo * fraccion, piso)
//
// La fluctuación cubre los módulos que difieren en GANANCIA (el mismo ruido
// les mueve la aguja distinto); la fracción del reposo cubre los que
// difieren en OFFSET; el piso evita que en un ambiente demasiado quieto el
// umbral quede pegado al reposo y dispare con cualquier cosa.
const float    SONIDO_FRACCION   = 1.00f;  // al menos el doble del reposo
const float    SONIDO_K_FLUCT    = 4.0f;
const uint16_t SONIDO_MARGEN_MIN = 15;

const float    LUZ_FRACCION      = 0.25f;
const float    LUZ_K_FLUCT       = 4.0f;
const uint16_t LUZ_MARGEN_MIN    = 60;

// Ventana para medir amplitud de sonido. Un analogRead suelto agarra un
// punto al azar de la onda de audio, así que el MISMO ruido se lee alto o
// bajo según el instante; el pico a pico de una ventana es una medida
// estable y comparable entre módulos.
const uint16_t VENTANA_SONIDO_MS = 40;

// Tras una alarma hay que ver la zona despejada este número de lecturas
// seguidas antes de poder disparar otra. Sin esto, mientras la condición
// siguiera presente el sistema re-disparaba cada ~5s y mandaba una
// notificación push nueva cada vez.
const uint8_t DESPEJADAS_PARA_REARME = 10;

// El eco tarda ~58us por cm: 12000us cubren ~2m, de sobra para un umbral
// de 50cm. Cuanto más alto, más tiempo bloquea pulseIn() cuando no hay
// nada enfrente (que es la mayor parte del tiempo).
const uint32_t ECO_TIMEOUT_US = 12000;
const uint16_t DISTANCIA_SIN_ECO = 999;

// El umbral de luz nunca puede quedar por encima de esto: si el sistema
// calibra a oscuras, luzBase queda cerca del máximo del ADC y el umbral
// se iría arriba de cualquier lectura posible, dejando la alarma sorda
// para siempre.
const uint16_t LUZ_UMBRAL_MAXIMO = 950;
const uint32_t INTERVALO_DERIVA_MS = 60000;

// ---------------- TIEMPOS ----------------
const uint16_t PERIODO_LOOP_MS = 50;
const uint16_t INTERVALO_ENVIO = 1000;   // al servidor cada 1s (más rápido que los 3s pedidos)
const uint16_t TIMEOUT_RESPUESTA_MS = 5000;

const uint32_t REINTENTO_WIFI_MIN_MS = 5000;
const uint32_t REINTENTO_WIFI_MAX_MS = 60000;
const uint32_t BACKOFF_SERVIDOR_MIN_MS = 1000;
const uint32_t BACKOFF_SERVIDOR_MAX_MS = 30000;

const uint8_t RECONEXIONES_PARA_AVISO = 5;

// ---------------- ESTADO ----------------
// Reposo y fluctuación medidos al arrancar; los umbrales salen de ellos.
uint16_t luzBase = 0;
uint16_t luzFluct = 0;
uint16_t luzMaxima = 0;
uint16_t sonidoBase = 0;
uint16_t sonidoFluct = 0;
uint16_t sonidoUmbral = 0;
uint8_t  confirmadas = 0;
uint8_t  lecturasDespejadas = 0;
bool     rearmePendiente = false;
bool     alarmaActiva = false;
uint32_t tiempoAlarma = 0;
uint32_t tiempoEnvio = 0;
uint32_t tiempoDeriva = 0;
uint16_t alertasTotales = 0;

uint16_t ultSonido = 0;
uint16_t ultLuz = 0;
uint16_t ultDistancia = 0;

// Uptime propio: millis() se da vuelta a los ~49.7 días, este contador
// no (136 años), así que "segundos" en el JSON no se reinicia.
uint32_t segundosUptime = 0;
uint32_t ultimoTickUptime = 0;

// WiFi.status() cuesta un viaje al ESP32-S3: se consulta una vez por
// vuelta y todos lo leen de acá.
int      estadoWifi = WL_IDLE_STATUS;
uint32_t ultimoIntentoWifi = 0;
uint32_t esperaWifi = REINTENTO_WIFI_MIN_MS;
bool     avisoSinModulo = false;

uint32_t ultimoFalloServidor = 0;
uint32_t esperaServidor = 0;          // 0 = sin backoff activo
uint8_t  reconexionesSeguidas = 0;
bool     avisoKeepAlive = false;

bool     respuestaPendiente = false;
uint32_t tiempoPeticion = 0;

// ================ SENSOR ULTRASÓNICO ================
uint16_t medirDistancia() {
  if (!HC_SR04_ACTIVO) return 0;

  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  uint32_t duracion = pulseIn(PIN_ECHO, HIGH, ECO_TIMEOUT_US);
  if (duracion == 0) return DISTANCIA_SIN_ECO;

  return duracion * 0.0343 / 2;
}

// ================ SONIDO ================
// Amplitud pico a pico de la ventana: la diferencia entre el punto más
// alto y el más bajo de la onda. Eso sí es "cuánto sonido hay"; un
// analogRead suelto solo dice en qué parte de la onda cayó el muestreo.
uint16_t medirSonidoPicoAPico() {
  uint16_t minimo = 1023;
  uint16_t maximo = 0;
  uint32_t fin = millis() + VENTANA_SONIDO_MS;

  while ((int32_t)(millis() - fin) < 0) {
    uint16_t m = analogRead(PIN_SONIDO);
    if (m < minimo) minimo = m;
    if (m > maximo) maximo = m;
  }

  return maximo > minimo ? maximo - minimo : 0;
}

// ================ UMBRALES RELATIVOS ================
// umbral = reposo + max(K*fluctuacion, reposo*fraccion, piso)
uint16_t calcularUmbral(uint16_t base, uint16_t fluct, float k, float fraccion, uint16_t piso) {
  uint32_t margen = (uint32_t)(k * fluct);
  uint32_t porFraccion = (uint32_t)(fraccion * base);
  if (porFraccion > margen) margen = porFraccion;
  if (piso > margen) margen = piso;
  return (uint16_t)(base + margen);
}

void recalcularUmbrales(bool avisar) {
  sonidoUmbral = calcularUmbral(sonidoBase, sonidoFluct,
                                SONIDO_K_FLUCT, SONIDO_FRACCION, SONIDO_MARGEN_MIN);

  uint32_t umbralLuz = calcularUmbral(luzBase, luzFluct,
                                      LUZ_K_FLUCT, LUZ_FRACCION, LUZ_MARGEN_MIN);
  if (umbralLuz > LUZ_UMBRAL_MAXIMO) {
    umbralLuz = LUZ_UMBRAL_MAXIMO;
    if (avisar) Serial.println(F("AVISO: calibro muy oscuro, umbral de luz limitado."));
  }
  luzMaxima = (uint16_t)umbralLuz;
}

void calibrar() {
  uint32_t sumaLuz = 0, sumaSonido = 0;
  uint16_t luzMin = 1023, luzMax = 0;
  uint16_t sonMin = 1023, sonMax = 0;

  for (uint8_t i = 0; i < MUESTRAS_CALIB; i++) {
    uint16_t l = analogRead(PIN_LUZ);
    uint16_t s = medirSonidoPicoAPico();   // ya tarda VENTANA_SONIDO_MS

    sumaLuz += l;
    sumaSonido += s;
    if (l < luzMin) luzMin = l;
    if (l > luzMax) luzMax = l;
    if (s < sonMin) sonMin = s;
    if (s > sonMax) sonMax = s;
  }

  luzBase = sumaLuz / MUESTRAS_CALIB;
  luzFluct = luzMax - luzMin;
  sonidoBase = sumaSonido / MUESTRAS_CALIB;
  sonidoFluct = sonMax - sonMin;
  recalcularUmbrales(true);

  Serial.print(F("Sonido reposo: "));
  Serial.print(sonidoBase);
  Serial.print(F(" (+-"));
  Serial.print(sonidoFluct);
  Serial.print(F(") -> dispara sobre "));
  Serial.println(sonidoUmbral);

  Serial.print(F("Luz reposo: "));
  Serial.print(luzBase);
  Serial.print(F(" (+-"));
  Serial.print(luzFluct);
  Serial.print(F(") -> dispara sobre "));
  Serial.println(luzMaxima);
}

// La luz y el ruido de fondo cambian a lo largo del día. Solo se adapta
// cuando la lectura está en zona "normal": si se adaptara durante una
// intrusión, el umbral seguiría al evento y lo anularía.
void adaptarDeriva(bool sinLuz, bool hayRuido) {
  if (alarmaActiva) return;
  if (millis() - tiempoDeriva < INTERVALO_DERIVA_MS) return;
  tiempoDeriva = millis();

  if (!sinLuz)   luzBase    = (uint16_t)(((uint32_t)luzBase * 15 + ultLuz) / 16);
  if (!hayRuido) sonidoBase = (uint16_t)(((uint32_t)sonidoBase * 15 + ultSonido) / 16);
  recalcularUmbrales(false);
}

// ================ WIFI ================
void conectarWiFi() {
  Serial.print(F("Conectando a "));
  Serial.print(WIFI_SSID);

  estadoWifi = WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint8_t intentos = 0;
  while (estadoWifi != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(F("."));
    estadoWifi = WiFi.status();
    intentos++;
  }

  if (estadoWifi == WL_CONNECTED) {
    Serial.println(F(" conectado."));
    Serial.print(F("IP local: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F(" fallo. Continua sin red."));
  }
}

// WiFi.begin() bloquea por dentro varios segundos. Con un hotspot que se
// duerme, llamarlo cada 5s deja el loop congelado casi en permanencia;
// por eso la espera se va duplicando mientras siga fallando.
void reintentarWiFi() {
  if (estadoWifi == WL_CONNECTED) {
    esperaWifi = REINTENTO_WIFI_MIN_MS;
    return;
  }

  if (estadoWifi == WL_NO_MODULE) {
    if (!avisoSinModulo) {
      Serial.println(F("ERROR: no responde el modulo WiFi (revisar firmware del ESP32-S3)."));
      avisoSinModulo = true;
    }
    return;
  }

  if (millis() - ultimoIntentoWifi < esperaWifi) return;
  ultimoIntentoWifi = millis();

  Serial.print(F("WiFi caido, reintentando (espera "));
  Serial.print(esperaWifi / 1000);
  Serial.println(F("s)..."));

  estadoWifi = WiFi.begin(WIFI_SSID, WIFI_PASS);

  if (estadoWifi == WL_CONNECTED) {
    esperaWifi = REINTENTO_WIFI_MIN_MS;
    Serial.println(F("WiFi reconectado."));
  } else {
    esperaWifi *= 2;
    if (esperaWifi > REINTENTO_WIFI_MAX_MS) esperaWifi = REINTENTO_WIFI_MAX_MS;
  }
}

// ================ ENVIAR DATOS A RENDER ================
// Conexión persistente: se reutiliza el mismo cliente TLS entre envíos
// (Connection: keep-alive) para no pagar un handshake completo cada vez.
WiFiSSLClient clienteDatos;

// Lee la respuesta del POST ANTERIOR sin esperar: solo consume lo que ya
// llegó. Así se sabe con qué código contestó el servidor sin que el loop
// se quede bloqueado en ningún momento.
void verificarRespuestaPendiente() {
  if (!respuestaPendiente) return;

  int disponibles = clienteDatos.available();
  if (disponibles <= 0) {
    if (millis() - tiempoPeticion >= TIMEOUT_RESPUESTA_MS) {
      Serial.println(F("El servidor no contesto al ultimo POST."));
      respuestaPendiente = false;
    }
    return;
  }

  char cabecera[40];
  int aLeer = disponibles < (int)sizeof(cabecera) - 1 ? disponibles : (int)sizeof(cabecera) - 1;
  int leidos = clienteDatos.read((uint8_t*)cabecera, aLeer);
  if (leidos > 0) cabecera[leidos] = '\0';

  if (leidos >= 12 && cabecera[0] == 'H') {
    int codigo = (cabecera[9] - '0') * 100 + (cabecera[10] - '0') * 10 + (cabecera[11] - '0');
    if (codigo < 200 || codigo >= 300) {
      Serial.print(F("El servidor respondio HTTP "));
      Serial.println(codigo);
    }
  }

  // El resto de la respuesta se descarta en bloque: leer de a un byte
  // serían cientos de viajes al modem por cada POST.
  uint8_t basura[64];
  while ((disponibles = clienteDatos.available()) > 0) {
    int n = disponibles < (int)sizeof(basura) ? disponibles : (int)sizeof(basura);
    if (clienteDatos.read(basura, n) <= 0) break;
  }

  respuestaPendiente = false;
}

bool enviarDatos() {
  tiempoEnvio = millis();

  if (estadoWifi != WL_CONNECTED) return false;

  verificarRespuestaPendiente();

  bool reconecto = !clienteDatos.connected();
  if (reconecto) {
    if (esperaServidor > 0 && millis() - ultimoFalloServidor < esperaServidor) return false;

    uint32_t tConexion = millis();
    if (!clienteDatos.connect(SERVIDOR_HOST, SERVIDOR_PUERTO)) {
      clienteDatos.stop();
      respuestaPendiente = false;
      ultimoFalloServidor = millis();
      esperaServidor = esperaServidor == 0 ? BACKOFF_SERVIDOR_MIN_MS : esperaServidor * 2;
      if (esperaServidor > BACKOFF_SERVIDOR_MAX_MS) esperaServidor = BACKOFF_SERVIDOR_MAX_MS;

      Serial.print(F("No se pudo conectar al servidor ("));
      Serial.print(millis() - tConexion);
      Serial.print(F("ms). Proximo intento en "));
      Serial.print(esperaServidor / 1000);
      Serial.println(F("s."));
      return false;
    }
    esperaServidor = 0;

    if (reconexionesSeguidas < 255) reconexionesSeguidas++;
    if (reconexionesSeguidas >= RECONEXIONES_PARA_AVISO && !avisoKeepAlive) {
      Serial.println(F("AVISO: el keep-alive no funciona, se reconecta en cada envio."));
      avisoKeepAlive = true;
    }
  } else {
    reconexionesSeguidas = 0;
    avisoKeepAlive = false;
  }

  char json[192];
  int largoJson = snprintf(json, sizeof(json),
    "{\"sonido\":%u,\"sonidoUmbral\":%u,\"luz\":%u,\"luzUmbral\":%u,\"distancia\":%u,"
    "\"alarma\":%s,\"confirmadas\":%u,\"objetivo\":%u,\"alertas\":%u,\"segundos\":%lu}",
    (unsigned)ultSonido, (unsigned)sonidoUmbral,
    (unsigned)ultLuz, (unsigned)luzMaxima, (unsigned)ultDistancia,
    alarmaActiva ? "true" : "false", (unsigned)confirmadas, (unsigned)CONFIRMACIONES,
    (unsigned)alertasTotales, (unsigned long)segundosUptime);

  if (largoJson <= 0 || largoJson >= (int)sizeof(json)) {
    Serial.println(F("ERROR: el JSON no entro en el buffer."));
    return false;
  }

  char peticion[448];
  int largoPeticion = snprintf(peticion, sizeof(peticion),
    "POST %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %d\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "%s",
    SERVIDOR_RUTA, SERVIDOR_HOST, largoJson, json);

  if (largoPeticion <= 0 || largoPeticion >= (int)sizeof(peticion)) {
    Serial.println(F("ERROR: la peticion no entro en el buffer."));
    return false;
  }

  // Un solo write con todo: headers y body viajan juntos en el mismo
  // record TLS, en una única llamada al modem.
  size_t escritos = clienteDatos.write((const uint8_t*)peticion, largoPeticion);
  if (escritos != (size_t)largoPeticion) {
    Serial.println(F("Escritura incompleta, se cierra la conexion."));
    clienteDatos.stop();
    respuestaPendiente = false;
    return false;
  }

  respuestaPendiente = true;
  tiempoPeticion = millis();
  return true;
}

// ================ ALARMA ================
void dispararAlarma() {
  alarmaActiva = true;
  tiempoAlarma = millis();
  if (alertasTotales < 65535) alertasTotales++;
  Serial.println(F(">>> ALARMA ACTIVADA <<<"));
  if (!enviarDatos()) Serial.println(F("(no se pudo avisar al servidor)"));
}

void detenerAlarma() {
  alarmaActiva = false;
  confirmadas = 0;
  lecturasDespejadas = 0;
  rearmePendiente = true;   // no vuelve a disparar hasta ver la zona despejada
  Serial.println(F("Alarma desactivada (esperando zona despejada para rearmar)"));
  if (!enviarDatos()) Serial.println(F("(no se pudo avisar al servidor)"));
}

// ================ SETUP ================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_SONIDO_DO, INPUT);
  if (HC_SR04_ACTIVO) {
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
  }

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println(F("ERROR: no responde el modulo WiFi (revisar firmware del ESP32-S3)."));
    avisoSinModulo = true;
  }

  conectarWiFi();

  Serial.println(F("Calibrando luz ambiente..."));
  calibrar();

  ultimoTickUptime = millis();
  tiempoDeriva = millis();
  Serial.println(F("Sistema armado."));
}

// ================ LOOP ================
void loop() {
  uint32_t tLoopInicio = millis();

  // Uptime por acumulación de deltas: sobrevive al vuelco de millis() y
  // se pone al día solo si una vuelta tardó más de un segundo.
  while (tLoopInicio - ultimoTickUptime >= 1000) {
    ultimoTickUptime += 1000;
    segundosUptime++;
  }

  estadoWifi = WiFi.status();
  reintentarWiFi();
  uint32_t tWifi = millis();

  ultSonido = medirSonidoPicoAPico();
  ultLuz    = analogRead(PIN_LUZ);
  uint32_t tSensores = millis();

  ultDistancia = medirDistancia();
  uint32_t tDistancia = millis();

  bool hayRuido    = ultSonido > sonidoUmbral;
  bool sinLuz      = ultLuz > luzMaxima;
  bool hayCercania = HC_SR04_ACTIVO ? (ultDistancia < DISTANCIA_ALERTA) : true;
  bool intrusion   = hayRuido && sinLuz && hayCercania;

  adaptarDeriva(sinLuz, hayRuido);

  // CONFIRMACIONES son lecturas CONSECUTIVAS: cualquier lectura limpia
  // corta la racha. Sin este reset, tres picos espurios separados por
  // horas terminaban disparando la alarma igual.
  if (intrusion) {
    lecturasDespejadas = 0;
    if (!rearmePendiente && confirmadas < CONFIRMACIONES) confirmadas++;
  } else {
    confirmadas = 0;
    if (rearmePendiente) {
      lecturasDespejadas++;
      if (lecturasDespejadas >= DESPEJADAS_PARA_REARME) {
        rearmePendiente = false;
        lecturasDespejadas = 0;
        Serial.println(F("Zona despejada: sistema rearmado."));
      }
    }
  }

  if (!alarmaActiva && confirmadas >= CONFIRMACIONES) {
    dispararAlarma();
  }

  if (alarmaActiva && (millis() - tiempoAlarma >= DURACION_ALARMA)) {
    detenerAlarma();
  }

  if (millis() - tiempoEnvio >= INTERVALO_ENVIO) {
    enviarDatos();
  }
  uint32_t tEnvio = millis();

#if DEBUG_TIMING
  // Una sola línea armada en memoria: 25 Serial.print() sueltos por
  // vuelta costaban más que todo el resto del diagnóstico junto.
  char linea[220];
  snprintf(linea, sizeof(linea),
    "[t] wifi=%lu sens=%lu dist=%lu env=%lu | S:%u/%u%s L:%u/%u%s D:%ucm%s | C:%u/%u%s | %s%s",
    (unsigned long)(tWifi - tLoopInicio),
    (unsigned long)(tSensores - tWifi),
    (unsigned long)(tDistancia - tSensores),
    (unsigned long)(tEnvio - tDistancia),
    (unsigned)ultSonido, (unsigned)sonidoUmbral, hayRuido ? "[!]" : "",
    (unsigned)ultLuz, (unsigned)luzMaxima, sinLuz ? "[!]" : "",
    (unsigned)ultDistancia, hayCercania ? "[!]" : "",
    (unsigned)confirmadas, (unsigned)CONFIRMACIONES, rearmePendiente ? " REARME" : "",
    estadoWifi == WL_CONNECTED ? "WiFi OK" : "WiFi CAIDO",
    alarmaActiva ? " | ALARMA" : "");
  Serial.println(linea);
#endif

  // Período fijo: se descuenta lo que ya tardó la vuelta, así el muestreo
  // no se desacompasa cuando la red tarda.
  uint32_t transcurrido = millis() - tLoopInicio;
  if (transcurrido < PERIODO_LOOP_MS) delay(PERIODO_LOOP_MS - transcurrido);
}
