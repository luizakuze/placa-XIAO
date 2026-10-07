# Roteiro 01 - Blink

Pisca o LED embutido da XIAO ESP32-C6. É o "olá, mundo" da eletrônica: serve para confirmar que o ambiente está instalado e que a placa grava programas.

## Material

- 1 placa XIAO ESP32-C6
- Cabo USB-C com dados

## Montagem

Nenhuma. Basta conectar a placa ao computador pelo cabo USB-C.

## Como rodar

1. Abra [`roteiro-01-blink.ino`](./roteiro-01-blink.ino) no Arduino IDE.
2. Selecione a placa em `Tools > Board > esp32 > XIAO_ESP32C6`.
3. Selecione a porta em `Tools > Port` (`/dev/ttyACM0` no Linux, `COMx` no Windows).
4. Clique em **Upload**.

## O que se espera

O LED laranja da placa (`USER LED`, `GPIO15`) pisca continuamente: meio segundo aceso, meio segundo apagado.

## Conceitos

- `pinMode()`: configura um pino como saída.
- `digitalWrite()`: liga (`HIGH`) ou desliga (`LOW`) o pino.
- `delay()`: pausa o programa por alguns milissegundos.

## Experimente

- Mude os valores de `delay()` para piscar mais rápido ou mais devagar.
- Faça o LED piscar em um padrão, como o SOS em código Morse.
