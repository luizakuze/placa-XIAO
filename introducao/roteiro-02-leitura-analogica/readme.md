# Roteiro 02 - Leitura analógica

Lê o valor de um potenciômetro (ou outro sensor analógico) no pino `D0` e mostra o resultado no Monitor Serial.

## Material

- 1 placa XIAO ESP32-C6
- Cabo USB-C com dados
- 1 potenciômetro (ex.: 10 kΩ) ou sensor analógico (LDR, sensor de temperatura etc.)
- Protoboard e jumpers

## Montagem

| Potenciômetro | XIAO |
|---|---|
| Pino central | `D0` |
| Um dos pinos laterais | `3V3` |
| Outro pino lateral | `GND` |

> Use `3V3`, não `5V`: os pinos da placa trabalham em 3,3 V.

## Como rodar

1. Abra [`roteiro-02-leitura-analogica.ino`](./roteiro-02-leitura-analogica.ino) no Arduino IDE.
2. Selecione a placa `XIAO_ESP32C6` e a porta correta.
3. Habilite `Tools > USB CDC On Boot > Enabled` (sem isso, nada aparece no Monitor Serial).
4. Clique em **Upload**.
5. Abra o Monitor Serial (`Tools > Serial Monitor`) e ajuste a velocidade para **115200**.

## O que se espera

Um número novo aparece no Monitor Serial a cada 200 ms. Ao girar o potenciômetro, o valor varia entre cerca de `0` e `4095` (o conversor analógico-digital da placa tem 12 bits).

Para ver um gráfico em vez de números, use `Tools > Serial Plotter`.

## Conceitos

- `analogRead()`: converte a tensão no pino em um número.
- `Serial.begin()` / `Serial.println()`: envia texto da placa para o computador.

## Experimente

- Troque o potenciômetro por um LDR (com um resistor de ~10 kΩ formando um divisor de tensão) e cubra o sensor com a mão.
- Converta a leitura para tensão: `leitura * 3.3 / 4095`.
