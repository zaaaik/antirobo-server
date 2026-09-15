/* ============================================================
   SISTEMA ANTIROBO — ARDUINO UNO R4 WiFi
   ============================================================
   Envía los datos a un backend en Render. Se ve desde
   cualquier lugar con internet, no solo en la red local.

   CONEXIONES:
     HW-485 (sonido)     AO -> A0    DO -> D2
     HW-486 (luz)        AO -> A1
     HC-SR04 (distancia) Trig -> D9  Echo -> D10
     LCD 1602A (I2C)     SDA -> A4   SCL -> A5
     LED                 ánodo -> R220Ω -> D12

   NOTA: UMBRAL_SONIDO y DISTANCIA_ALERTA son constantes fijas que
   la página web (public/index.html) también conoce como
   SONIDO_UMBRAL y DISTANCIA_UMBRAL. Si cambiás alguno de los dos
   acá, actualizá también esas constantes en la página para que
   "Qué se activó" siga marcando correctamente.

   El umbral de luz, en cambio, se calibra en cada arranque
   (luzBase + MARGEN_LUZ) y se envía al servidor como "luzUmbral"
   en cada POST, para que la página pueda saber en tiempo real
   cuándo la luz es la causa de una alerta.
   ============================================================ */

#include <WiFiS3.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

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
const uint8_t PIN_LED_ALARMA = 12;

const bool HC_SR04_ACTIVO = true;

// ---------------- AJUSTES ----------------
const uint16_t UMBRAL_SONIDO      = 410;
const uint16_t MARGEN_LUZ         = 150;
const uint16_t DISTANCIA_ALERTA   = 50;
const uint8_t  MUESTRAS_CALIB     = 50;
const uint8_t  CONFIRMACIONES     = 3;
const uint16_t DURACION_ALARMA    = 5000;
const uint16_t INTERVALO_LECTURA  = 50;
const uint16_t INTERVALO_PANTALLA = 400;
const uint16_t INTERVALO_ENVIO    = 2000;

// ---------------- ESTADO ----------------
uint16_t luzBase = 0;
uint16_t luzMaxima = 0;
uint8_t  confirmadas = 0;
bool     alarmaActiva = false;
uint32_t tiempoAlarma = 0;
uint32_t tiempoPantalla = 0;
uint32_t tiempoEnvio = 0;
uint16_t alertasTotales = 0;

uint16_t ultSonido = 0;
uint16_t ultLuz = 0;
uint16_t ultDistancia = 0;

// ================ SENSOR ULTRASÓNICO ================
uint16_t medirDistancia() {
  if (!HC_SR04_ACTIVO) return 0;

  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  uint32_t duracion = pulseIn(PIN_ECHO, HIGH, 25000);
  if (duracion == 0) return 999; // sin eco (timeout): se toma como "muy lejos"

  return duracion * 0.0343 / 2;
}

// ================ WIFI ================
void conectarWiFi() {
  Serial.print(F("Conectando a "));
  Serial.print(WIFI_SSID);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Conectando WiFi");

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint8_t intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(F("."));
    intentos++;
  }

  lcd.clear();
  lcd.setCursor(0, 0);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F(" conectado."));
    Serial.print(F("IP local: "));
    Serial.println(WiFi.localIP());

    lcd.print("WiFi OK");
    delay(1500);
  } else {
    Serial.println(F(" fallo. Continua sin red."));
    lcd.print("WiFi fallo");
    delay(2000);
  }
}

// ================ ENVIAR DATOS A RENDER ================
void enviarDatos() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Sin WiFi, no se envia."));
    return;
  }

  WiFiSSLClient cliente;

  if (!cliente.connect(SERVIDOR_HOST, SERVIDOR_PUERTO)) {
    Serial.println(F("No se pudo conectar al servidor."));
    return;
  }

  String json = "{";
  json += "\"sonido\":" + String(ultSonido) + ",";
  json += "\"luz\":" + String(ultLuz) + ",";
  json += "\"luzUmbral\":" + String(luzMaxima) + ",";
  json += "\"distancia\":" + String(ultDistancia) + ",";
  json += "\"alarma\":" + String(alarmaActiva ? "true" : "false") + ",";
  json += "\"confirmadas\":" + String(confirmadas) + ",";
  json += "\"objetivo\":" + String(CONFIRMACIONES) + ",";
  json += "\"alertas\":" + String(alertasTotales) + ",";
  json += "\"segundos\":" + String(millis() / 1000);
  json += "}";

  cliente.print(F("POST "));
  cliente.print(SERVIDOR_RUTA);
  cliente.println(F(" HTTP/1.1"));
  cliente.print(F("Host: "));
  cliente.println(SERVIDOR_HOST);
  cliente.println(F("Content-Type: application/json"));
  cliente.print(F("Content-Length: "));
  cliente.println(json.length());
  cliente.println(F("Connection: close"));
  cliente.println();
  cliente.print(json);

  uint32_t espera = millis();
  while (cliente.connected() && millis() - espera < 3000) {
    if (cliente.available()) {
      cliente.read();
    }
  }
  cliente.stop();

  Serial.println(F("Datos enviados."));
}

// ================ PANTALLA LCD ================
void actualizarPantalla(uint16_t distancia) {
  if (millis() - tiempoPantalla < INTERVALO_PANTALLA) return;
  tiempoPantalla = millis();

  lcd.clear();

  if (alarmaActiva) {
    lcd.setCursor(0, 0);
    lcd.print("** INTRUSION **");
    lcd.setCursor(0, 1);
    if (HC_SR04_ACTIVO) {
      lcd.print("Dist: ");
      lcd.print(distancia);
      lcd.print(" cm");
    } else {
      lcd.print("Ruido+Oscuridad");
    }
  } else {
    lcd.setCursor(0, 0);
    lcd.print("Vigilando ");
    lcd.print(alertasTotales);
    lcd.print(" alr");
    lcd.setCursor(0, 1);
    if (HC_SR04_ACTIVO) {
      lcd.print(distancia);
      lcd.print("cm  ");
    }
    lcd.print(confirmadas);
    lcd.print("/");
    lcd.print(CONFIRMACIONES);
  }
}

// ================ CALIBRACIÓN ================
void calibrar() {
  uint32_t suma = 0;
  for (uint8_t i = 0; i < MUESTRAS_CALIB; i++) {
    suma += analogRead(PIN_LUZ);
    delay(20);
  }
  luzBase   = suma / MUESTRAS_CALIB;
  luzMaxima = luzBase + MARGEN_LUZ;

  Serial.print(F("Luz base: "));
  Serial.print(luzBase);
  Serial.print(F(" | Dispara sobre: "));
  Serial.println(luzMaxima);
}

// ================ ALARMA ================
void dispararAlarma() {
  alarmaActiva = true;
  tiempoAlarma = millis();
  alertasTotales++;
  digitalWrite(PIN_LED_ALARMA, HIGH);
  Serial.println(F(">>> ALARMA ACTIVADA <<<"));
  enviarDatos();
}

void detenerAlarma() {
  alarmaActiva = false;
  confirmadas  = 0;
  digitalWrite(PIN_LED_ALARMA, LOW);
  Serial.println(F("Alarma desactivada"));
  enviarDatos();
}

// ================ SETUP ================
void setup() {
  Serial.begin(9600);

  pinMode(PIN_SONIDO_DO, INPUT);
  if (HC_SR04_ACTIVO) {
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
  }
  pinMode(PIN_LED_ALARMA, OUTPUT);
  digitalWrite(PIN_LED_ALARMA, LOW);

  lcd.init();
  lcd.backlight();

  conectarWiFi();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Calibrando...");
  Serial.println(F("Calibrando luz ambiente..."));
  calibrar();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sistema armado");
  delay(1500);

  Serial.println(F("Sistema armado."));
}

// ================ LOOP ================
void loop() {
  ultSonido    = analogRead(PIN_SONIDO);
  ultLuz       = analogRead(PIN_LUZ);
  ultDistancia = medirDistancia();

  bool hayRuido    = ultSonido > UMBRAL_SONIDO;
  bool sinLuz      = ultLuz > luzMaxima;
  bool hayCercania = HC_SR04_ACTIVO ? (ultDistancia < DISTANCIA_ALERTA) : true;

  bool intrusion = hayRuido && sinLuz && hayCercania;

  if (intrusion && confirmadas < 255) {
    confirmadas++;
  }

  if (!alarmaActiva && confirmadas >= CONFIRMACIONES) {
    dispararAlarma();
  }

  if (alarmaActiva && (millis() - tiempoAlarma >= DURACION_ALARMA)) {
    detenerAlarma();
  }

  actualizarPantalla(ultDistancia);

  if (millis() - tiempoEnvio >= INTERVALO_ENVIO) {
    tiempoEnvio = millis();
    enviarDatos();
  }

  Serial.print(F("Sonido: "));
  Serial.print(ultSonido);
  Serial.print(hayRuido ? F(" [!] ") : F("     "));
  Serial.print(F("| Luz: "));
  Serial.print(ultLuz);
  Serial.print(sinLuz ? F(" [!] ") : F("     "));
  if (HC_SR04_ACTIVO) {
    Serial.print(F("| Dist: "));
    Serial.print(ultDistancia);
    Serial.print(hayCercania ? F("cm [!] ") : F("cm     "));
  }
  Serial.print(F("| Conf: "));
  Serial.print(confirmadas);
  Serial.print(F("/"));
  Serial.print(CONFIRMACIONES);
  Serial.println(alarmaActiva ? F(" | ALARMA") : F(""));

  delay(INTERVALO_LECTURA);
}
