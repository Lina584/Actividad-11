/*
 * Contador de monedas por denominación - ESP32
 * Proyecto: Sistema logístico de monedas inteligentes (UMNG, Mecatrónica)
 * Autores: Lina María Moreno Ospina, Bryan Andrey Martinez Montaño
 *
 * Qué hace:
 *  - 3 sensores IR de barrera (uno por canal del separador mecánico).
 *  - Cada interrupción del haz = una moneda de esa denominación.
 *  - Cuenta, suma valor y peso nominal, imprime por Serial y publica por MQTT
 *    el MISMO JSON que usa dashboard_streamlit.py.
 *  - El conteo NUNCA se bloquea por el Wi-Fi: los flancos se capturan por
 *    interrupción y se procesan en loop().
 *
 * Librerías (Gestor de librerías del Arduino IDE):
 *   - PubSubClient (Nick O'Leary)
 *   - Sólo si USE_OLED=1: Adafruit SSD1306 + Adafruit GFX
 * Placa: "ESP32 Dev Module".
 */
#include "config.h"
#if USE_WIFI
  #include <WiFi.h>
  #include <PubSubClient.h>
  #include "secrets.h"
#endif
#if USE_OLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 oled(128, 64, &Wire, -1);
#endif

// ------------------------------------------------------------------
//  Captura de flancos (ISR)
// ------------------------------------------------------------------
struct Canal {
  uint8_t  pin;
  volatile uint32_t t_inicio_us;   // instante en que se bloqueó el haz
  volatile uint32_t t_fin_us;      // instante en que se liberó
  volatile bool     pulso_listo;   // hay un pulso completo para evaluar
  volatile bool     bloqueado;
  uint32_t ultimo_conteo_ms;
  uint32_t n;                      // conteo
  uint32_t rechazados;             // pulsos fuera de rango (diagnóstico)
};
static Canal canales[3] = {
  { PIN_SENSOR_100, 0, 0, false, false, 0, 0, 0 },
  { PIN_SENSOR_200, 0, 0, false, false, 0, 0, 0 },
  { PIN_SENSOR_500, 0, 0, false, false, 0, 0, 0 },
};

static void IRAM_ATTR manejarFlanco(Canal* c) {
  uint32_t ahora = micros();
  bool bloq = (digitalRead(c->pin) == NIVEL_BLOQUEADO);
  if (bloq && !c->bloqueado) {            // inicio del pulso
    c->bloqueado = true;
    c->t_inicio_us = ahora;
  } else if (!bloq && c->bloqueado) {     // fin del pulso
    c->bloqueado = false;
    c->t_fin_us = ahora;
    c->pulso_listo = true;
  }
}
static void IRAM_ATTR isr0() { manejarFlanco(&canales[0]); }
static void IRAM_ATTR isr1() { manejarFlanco(&canales[1]); }
static void IRAM_ATTR isr2() { manejarFlanco(&canales[2]); }

// ------------------------------------------------------------------
//  Red / MQTT
// ------------------------------------------------------------------
#if USE_WIFI
WiFiClient redWifi;
PubSubClient mqtt(redWifi);
static uint32_t ultimoIntento = 0;

static void mantenerConexion() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - ultimoIntento > MQTT_RECONEXION_MS) {
      ultimoIntento = millis();
      WiFi.begin(WIFI_SSID, WIFI_PASS);   // no bloquea: reintenta en segundo plano
    }
    return;
  }
  if (!mqtt.connected() && millis() - ultimoIntento > MQTT_RECONEXION_MS) {
    ultimoIntento = millis();
    char id[32];
    snprintf(id, sizeof(id), "esp32-monedas-%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
    mqtt.connect(id);
  }
}
#endif

// ------------------------------------------------------------------
//  Utilidades
// ------------------------------------------------------------------
static void pitar(int ms) {
  if (PIN_BUZZER > 0) { digitalWrite(PIN_BUZZER, HIGH); delay(ms); digitalWrite(PIN_BUZZER, LOW); }
}

static double marcaDeTiempo() {
  time_t t = time(nullptr);
  if (t > 1700000000) return (double)t;      // hora real si NTP ya sincronizó
  return millis() / 1000.0;                  // si no, segundos desde el arranque
}

static void publicar(int idx) {
  // JSON idéntico al de simulador_esp32.py:
  // {"evento","denominacion","totales":{"$500":{"n","peso","valor"},...},"timestamp"}
  char buf[420];
  int k = snprintf(buf, sizeof(buf),
      "{\"evento\":\"moneda_clasificada\",\"denominacion\":\"%s\",\"totales\":{", DENOM[idx].nombre);
  // Orden $500, $200, $100 como en el simulador
  for (int i = 2; i >= 0; i--) {
    k += snprintf(buf + k, sizeof(buf) - k,
        "\"%s\":{\"n\":%lu,\"peso\":%.2f,\"valor\":%lu}%s",
        DENOM[i].nombre, (unsigned long)canales[i].n,
        canales[i].n * DENOM[i].peso_g,
        (unsigned long)(canales[i].n * DENOM[i].valor), i > 0 ? "," : "");
  }
  snprintf(buf + k, sizeof(buf) - k, "},\"timestamp\":%.3f}", marcaDeTiempo());
  Serial.println(buf);
#if USE_WIFI
  if (mqtt.connected()) mqtt.publish(MQTT_TEMA, buf);
#endif
}

static void pantalla() {
#if USE_OLED
  oled.clearDisplay(); oled.setTextColor(WHITE); oled.setTextSize(1);
  oled.setCursor(0, 0); oled.println("Contador de monedas");
  uint32_t total = 0, valor = 0;
  for (int i = 2; i >= 0; i--) {
    oled.printf("%s : %lu\n", DENOM[i].nombre, (unsigned long)canales[i].n);
    total += canales[i].n; valor += canales[i].n * DENOM[i].valor;
  }
  oled.printf("Total: %lu  $%lu\n", (unsigned long)total, (unsigned long)valor);
  oled.display();
#endif
}

static void reiniciarConteos() {
  for (auto &c : canales) { c.n = 0; c.rechazados = 0; }
  Serial.println("[RESET] conteos en cero");
  pantalla();
}

// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_ESTADO, OUTPUT);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  if (PIN_BUZZER > 0) pinMode(PIN_BUZZER, OUTPUT);

  // GPIO34-39 no tienen pull-up interno; nuestros canales usan resistencia externa.
  for (auto &c : canales) pinMode(c.pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(canales[0].pin), isr0, CHANGE);
  attachInterrupt(digitalPinToInterrupt(canales[1].pin), isr1, CHANGE);
  attachInterrupt(digitalPinToInterrupt(canales[2].pin), isr2, CHANGE);

#if USE_OLED
  Wire.begin(21, 22);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED no encontrada");
#endif
#if USE_WIFI
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setSocketTimeout(2);
  configTime(-5 * 3600, 0, "pool.ntp.org");   // Colombia UTC-5
#endif
  Serial.println("Contador listo. Ponga una moneda en la rampa.");
  Serial.println("Comandos por Serial:  r = reiniciar,  d = diagnóstico");
  pantalla();
}

void loop() {
#if USE_WIFI
  mantenerConexion();
  mqtt.loop();
#endif

  // 1) Procesar pulsos capturados por las interrupciones
  for (int i = 0; i < 3; i++) {
    Canal &c = canales[i];
    if (!c.pulso_listo) continue;
    noInterrupts();
    uint32_t dur_ms = (c.t_fin_us - c.t_inicio_us) / 1000;
    c.pulso_listo = false;
    interrupts();

    bool duracionOk = dur_ms >= PULSO_MIN_MS && dur_ms <= PULSO_MAX_MS;
    bool rearmado   = (millis() - c.ultimo_conteo_ms) >= REARME_MS;
    if (duracionOk && rearmado) {
      c.n++;
      c.ultimo_conteo_ms = millis();
      Serial.printf("[%s] moneda detectada (pulso %lu ms)\n", DENOM[i].nombre, (unsigned long)dur_ms);
      publicar(i);
      pantalla();
      digitalWrite(PIN_LED_ESTADO, HIGH); pitar(30); digitalWrite(PIN_LED_ESTADO, LOW);
    } else {
      c.rechazados++;
      Serial.printf("[%s] pulso rechazado (%lu ms)\n", DENOM[i].nombre, (unsigned long)dur_ms);
    }
  }

  // 2) Botón de reinicio (con antirrebote simple)
  static uint32_t tBtn = 0;
  if (digitalRead(PIN_BTN_RESET) == LOW && millis() - tBtn > 400) {
    tBtn = millis(); reiniciarConteos();
  }

  // 3) Comandos por Serial
  if (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'r') reiniciarConteos();
    if (ch == 'd') {
      for (int i = 0; i < 3; i++)
        Serial.printf("%s: n=%lu rechazados=%lu pin=%d\n", DENOM[i].nombre,
                      (unsigned long)canales[i].n, (unsigned long)canales[i].rechazados,
                      digitalRead(canales[i].pin));
    }
  }
}
