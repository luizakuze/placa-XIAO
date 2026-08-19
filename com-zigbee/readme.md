# Comunicação Bidirecional com Zigbee

Este projeto implementa uma comunicação bidirecional via **Zigbee** entre duas placas **Seeed Studio XIAO ESP32C6**.

Cada estação pode controlar remotamente o LED da outra estação e possui um botão para reiniciar a comunicação.

## Funcionamento

```text
Estação A                         Estação B
┌─────────────┐                  ┌─────────────┐
│     D1      │ ───────────────► │     LED     │
│     LED     │ ◄─────────────── │     D1      │
│     D2      │                  │     D2      │
└─────────────┘                  └─────────────┘
           Comunicação via Zigbee
```

| Ação | Resultado |
|---|---|
| Pressionar **D1** da Estação A | LED da Estação B acende |
| Soltar **D1** da Estação A | LED da Estação B apaga |
| Pressionar **D1** da Estação B | LED da Estação A acende |
| Soltar **D1** da Estação B | LED da Estação A apaga |
| Manter **D1** das duas estações pressionados por **5 segundos** | Comunicação é finalizada |
| Comunicação finalizada | LEDs piscam continuamente |
| Pressionar **D2** em cada estação | Sessão é reiniciada |

> **D1:** botão de comunicação  
> **D2:** botão de reset

## Configurações na estação A

- Board: XIAO_ESP32C6

- Tools -> Zigbee Mode: Zigbee ZCZR (coordinator/router)

- Tools -> Partition Scheme: Zigbee ZCZR 4MB with spiffs

- Tools -> Erase All Flash Before Sketch Upload: Enabled

## Configurações na estação B

- Board: XIAO_ESP32C6

- Tools -> Zigbee Mode: Zigbee ED (end device)

- Tools -> Partition Scheme: Zigbee 4MB with spiffs

- Tools -> Erase All Flash Before Sketch Upload: Enabled

## Demonstração

