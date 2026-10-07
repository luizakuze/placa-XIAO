/*
  Roteiro 01 - Blink
  Pisca o LED embutido da XIAO ESP32-C6, sem nenhum componente externo.

  Montagem: nenhuma. Apenas conecte a placa via USB-C.

  Configuração no Arduino IDE:
    Board: XIAO_ESP32C6
*/

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
}
