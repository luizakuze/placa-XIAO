/*
  Roteiro 02 - Leitura analogica
  Le um sensor ou potenciometro no pino D0 e mostra o valor no Monitor Serial.

  Montagem:
    - Potenciometro: pino central em D0, os outros dois em 3V3 e GND.
    - Ou qualquer sensor analogico (LDR, sensor de temperatura, etc) no lugar do potenciometro.

  Configuracao no Arduino IDE:
    Board: XIAO_ESP32C6
    USB CDC On Boot: Enabled (necessario para ver a saida no Monitor Serial)
*/

const int PINO_ANALOGICO = D0;

void setup() {
  Serial.begin(115200);
}

void loop() {
  int leitura = analogRead(PINO_ANALOGICO);
  Serial.println(leitura);
  delay(200);
}
