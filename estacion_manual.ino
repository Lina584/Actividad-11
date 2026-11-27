/*
  Estacion con banda MANUAL (interruptor): compuerta (servo) + sensor IR. Sin pantalla.

  La banda corre siempre que el interruptor este encendido; la compuerta cerrada detiene la moneda.
    1. El sensor IR (FC-51) detecta que llego un objeto a la compuerta.
    2. La ESP32 manda "LISTO" por USB. El computador toma la foto y responde:
         M = moneda     -> la compuerta se abre y la moneda pasa
         N = no moneda  -> la compuerta queda cerrada hasta que retires el objeto
    3. Sin respuesta en 15 s se trata como "no moneda".

  Pruebas por Monitor Serie (minusculas): a=abrir c=cerrar i=leer IR
  Libreria: ESP32Servo. Placa: ESP32 Dev Module.
*/
#include <ESP32Servo.h>

// ---------------- PINES ----------------
const int PIN_SERVO = 18;
const int PIN_IR = 32;                    // FC-51 (LOW = hay objeto)

// ---------------- AJUSTES ----------------
const int ANG_CERRADA = 0;
const int ANG_ABIERTA = 90;
const unsigned long T_ASENTAR   = 800;    // ms con el objeto en la compuerta antes de la foto
const unsigned long T_RESPUESTA = 15000;
const unsigned long T_PASO      = 1500;
const unsigned long T_LIBRE     = 1000;
const unsigned long T_DETECTA   = 100;

Servo compuerta;
enum Estado { ESPERA_OBJETO, ASENTAR, ESPERA_RESP, PASA, RETIRAR };
Estado estado = ESPERA_OBJETO;
unsigned long tEstado = 0, tIrCambio = 0;
bool irPrevio = false;
int nMonedas = 0, nRechazados = 0;

bool irCrudo() { return digitalRead(PIN_IR) == LOW; }
bool irEstable(bool valor, unsigned long ms) {
  bool ahora = irCrudo();
  if (ahora != irPrevio) { irPrevio = ahora; tIrCambio = millis(); }
  return (ahora == valor) && (millis() - tIrCambio >= ms);
}
void cambiar(Estado e) { estado = e; tEstado = millis(); }

void cuenta() {
  Serial.print("Monedas: "); Serial.print(nMonedas);
  Serial.print("  No monedas: "); Serial.println(nRechazados);
}

void rechazar(const char* motivo) {
  nRechazados++;
  Serial.print("NO MONEDA ("); Serial.print(motivo); Serial.println(")");
  compuerta.write(ANG_CERRADA);
  cuenta();
  cambiar(RETIRAR);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_IR, INPUT_PULLUP);
  compuerta.attach(PIN_SERVO);
  compuerta.write(ANG_CERRADA);
  irPrevio = irCrudo(); tIrCambio = millis();
  Serial.println("Estacion manual lista.");
}

void loop() {
  while (Serial.available()) {               // comandos de prueba
    char t = Serial.peek();
    if (t == 'M' || t == 'N') break;
    Serial.read();
    if (t == 'a') { compuerta.write(ANG_ABIERTA); Serial.println("compuerta ABIERTA"); }
    if (t == 'c') { compuerta.write(ANG_CERRADA); Serial.println("compuerta CERRADA"); }
    if (t == 'i') { Serial.println(irCrudo() ? "IR: OBJETO" : "IR: libre"); }
  }

  switch (estado) {
    case ESPERA_OBJETO:
      if (irEstable(true, T_DETECTA)) cambiar(ASENTAR);
      break;

    case ASENTAR:
      if (millis() - tEstado >= T_ASENTAR) {
        while (Serial.available()) Serial.read();
        Serial.println("LISTO");
        cambiar(ESPERA_RESP);
      }
      break;

    case ESPERA_RESP:
      if (Serial.available()) {
        char r = Serial.read();
        if (r == 'M') {
          nMonedas++;
          Serial.println("MONEDA");
          compuerta.write(ANG_ABIERTA);
          cuenta();
          cambiar(PASA);
        } else if (r == 'N') {
          rechazar("el computador dijo N");
        }
      } else if (millis() - tEstado >= T_RESPUESTA) {
        rechazar("sin respuesta");
      }
      break;

    case PASA:
      if ((millis() - tEstado >= T_PASO && irEstable(false, 300)) ||
          (millis() - tEstado >= 3 * T_PASO)) {
        compuerta.write(ANG_CERRADA);
        cambiar(ESPERA_OBJETO);
      }
      break;

    case RETIRAR:
      if (irEstable(false, T_LIBRE)) cambiar(ESPERA_OBJETO);
      break;
  }
}
