/*
  Roteiro 03 - Botao + LED
  Um botao externo liga e desliga um LED externo.

  Montagem:
    - LED: anodo (perna longa) em D1 atraves de um resistor (~220 ohm), catodo em GND.
    - Botao: um lado em D2, outro em GND. D2 usa pull-up interno,
      entao o botao pressionado le LOW.

  Configuracao no Arduino IDE:
    Board: XIAO_ESP32C6
*/

const int PINO_LED = D1;
const int PINO_BOTAO = D2;

void setup() {
  pinMode(PINO_LED, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
}

void loop() {
  bool pressionado = (digitalRead(PINO_BOTAO) == LOW);
  digitalWrite(PINO_LED, pressionado ? HIGH : LOW);
}
