# Roteiro 03 - Botão + LED

Um botão externo controla um LED externo: enquanto o botão está pressionado, o LED fica aceso.

## Material

- 1 placa XIAO ESP32-C6
- Cabo USB-C com dados
- 1 LED
- 1 resistor de ~220 Ω
- 1 botão (push button)
- Protoboard e jumpers

## Montagem

| Componente | Ligação |
|---|---|
| LED, perna longa (ânodo) | `D1`, passando pelo resistor de 220 Ω |
| LED, perna curta (cátodo) | `GND` |
| Botão, um lado | `D2` |
| Botão, outro lado | `GND` |

O pino `D2` usa o resistor de pull-up interno da placa, então não é preciso resistor externo no botão.

## Como rodar

1. Abra [`roteiro-03-botao-led.ino`](./roteiro-03-botao-led.ino) no Arduino IDE.
2. Selecione a placa `XIAO_ESP32C6` e a porta correta.
3. Clique em **Upload**.

## O que se espera

- Botão solto: LED apagado.
- Botão pressionado: LED aceso.

Se o LED não acender, confira se ele não está invertido (a perna longa vai para o lado do `D1`).

## Conceitos

- `INPUT_PULLUP`: mantém o pino em `HIGH` quando nada está ligado; o botão pressionado leva o pino para `LOW`.
- `digitalRead()`: lê se um pino está em `HIGH` ou `LOW`.
- Entrada e saída digital no mesmo programa.

## Experimente

- Inverta a lógica: LED aceso com o botão solto.
- Faça cada clique alternar o LED entre aceso e apagado (dica: guarde o estado anterior do botão em uma variável).
