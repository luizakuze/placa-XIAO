# Testbed de Desempenho: Wi-Fi vs ESP-NOW vs BLE vs Zigbee

Este diretório monta um banco de testes (testbed) para comparar, na prática e
com a mesma dupla de placas **XIAO ESP32-C6**, o desempenho das tecnologias de
rádio que o chip oferece nativamente. A pergunta que o testbed responde é:
*"para este projeto de telecomunicações, qual tecnologia sem fio faz mais
sentido em cada cenário?"* - e a resposta vem de números medidos, não de
folha de dados.

O método é o mesmo em todas as tecnologias: uma estação **A (iniciadora)**
envia uma mensagem, uma estação **B (respondedora)** devolve o mesmo conteúdo
imediatamente (eco), e A mede quanto tempo levou a ida-e-volta. É o mesmo
princípio do comando `ping`, por isso chamamos de **ping-pong**.

## Sumário

- [Tecnologias comparadas](#tecnologias-comparadas)
- [Metodologia](#metodologia)
- [Métricas coletadas](#métricas-coletadas)
- [Cenários de teste](#cenários-de-teste)
- [Montagem física](#montagem-física)
- [Configuração do Arduino IDE](#configuração-do-arduino-ide)
- [Procedimento de execução](#procedimento-de-execução)
- [Formato dos resultados](#formato-dos-resultados)
- [Estrutura de pastas](#estrutura-de-pastas)
- [Solução de problemas](#solução-de-problemas)
- [Referências](#referências)

## Tecnologias comparadas

A XIAO ESP32-C6 tem **um único rádio de 2,4 GHz**, compartilhado no tempo
entre Wi-Fi, Bluetooth LE e 802.15.4 (Zigbee/Thread) - por isso os testes
rodam **uma tecnologia por vez**, cada uma com seu próprio par de sketches:

| Tecnologia | Pasta | Camada usada | Topologia no teste | Por que comparar |
|---|---|---|---|---|
| **Wi-Fi** | [`wifi/`](./wifi/) | 802.11 (SoftAP + UDP) | A cria a rede (AP), B conecta como estação | O que a maioria já reconhece como "Wi-Fi"; mostra o custo de associação/IP/roteador embutido no protocolo |
| **ESP-NOW** | [`espnow/`](./espnow/) | Radio Wi-Fi, sem associação | Broadcast direto, sem AP/IP | Mesmo rádio do Wi-Fi, mas "sem conexão" - bom contraponto para isolar o custo da pilha TCP/IP |
| **BLE** | [`ble/`](./ble/) | Bluetooth 5 LE (GATT) | B anuncia, A conecta como central | Baixo consumo, payload pequeno, presente em praticamente todo celular |
| **Zigbee** | [`zigbee/`](./zigbee/) | 802.15.4 / Zigbee 3.0 | B (end device) associado ao coordenador A | Malha, baixa taxa, foi o primeiro estudo deste repositório ([`../com-zigbee/`](../com-zigbee/)) |

**Thread** (também 802.15.4, usado pelo Matter) roda no mesmo rádio e é
suportado pelo chip, mas foi deixado de fora do testbed: a pilha OpenThread
no Arduino-ESP32 exige um Border Router e configuração bem mais avançada que
as demais, o que foge do escopo de um teste comparativo direto placa-a-placa.
Fica como extensão futura.

Números de datasheet (taxa de PHY teórica) não entram nesta tabela de
propósito: 802.15.4 tem um teto de PHY conhecido (250 kbit/s), BLE 5 opera a
1 ou 2 Mbit/s conforme o PHY negociado, e Wi-Fi 6 tem um teto muito mais alto
- mas nenhum desses números é o que uma aplicação realmente consegue entregar
em um microcontrolador. É exatamente isso que o testbed mede.

## Metodologia

### Ping-pong sem sincronizar relógio

Cada teste segue a mesma lógica em A:

1. A grava `t0 = micros()` e envia um pacote/comando.
2. B recebe e devolve o mesmo conteúdo (eco) o mais rápido possível.
3. A recebe a resposta e calcula `RTT = micros() - t0`.
4. Repete `PING_COUNT` vezes (100 por padrão) antes de imprimir o resultado.

Como o `t0` é gravado e conferido **sempre no relógio de A**, não é preciso
sincronizar os relógios das duas placas - o mesmo truque usado pelo `ping`
tradicional. B não faz nenhuma medição; só ecoa.

Em Wi-Fi, ESP-NOW e BLE, o pacote carrega um cabeçalho próprio (número de
sequência + timestamp) e é preenchido até o tamanho de payload sendo testado
naquele momento. Em Zigbee, o "payload" é o próprio estado ON/OFF do cluster
(1 bit) - o cluster On/Off não foi feito para carregar um payload de bytes
livre, então o teste Zigbee mede **latência**, não variação de tamanho de
payload (ver comentários em [`zigbee/estacao_a.ino`](./zigbee/estacao_a.ino)).

Em todas as tecnologias o transporte usado é **best-effort** (sem
confirmação/retransmissão automática na camada que estamos medindo: UDP,
broadcast ESP-NOW, GATT Write No Response e o comando ZCL direto) - então a
"perda de pacotes" reportada é uma característica real de cada tecnologia
naquele cenário, não um artefato do teste.

### O que o teste NÃO mede

- **Vazão saturada/pipelinada** (estilo `iperf`, várias mensagens em voo ao
  mesmo tempo). Como o ping-pong espera cada resposta antes de mandar a
  próxima, o número de "vazão" no resultado é uma vazão *limitada pelo RTT*
  - útil para comparar as quatro tecnologias entre si, mas não é o máximo
  teórico de cada uma.
- **Consumo de energia** de forma instrumentada. Se houver um multímetro ou
  medidor de corrente USB disponível, ele pode ser inserido em série com a
  alimentação da estação B durante qualquer um dos cenários abaixo - é um
  bom complemento opcional, mas não faz parte do firmware.

## Métricas coletadas

Cada iniciador (`estacao_a.ino`) imprime uma linha `RESULT,...` por lote de
teste, sempre com as mesmas colunas:

| Coluna | Significado |
|---|---|
| `tech` | Tecnologia testada (`WIFI`, `ESPNOW`, `BLE`, `ZIGBEE`) |
| `payload_bytes` | Tamanho do pacote de aplicação, em bytes (fixo em `1` para Zigbee) |
| `sent` | Quantos pings foram enviados |
| `received` | Quantos ecos voltaram dentro do timeout |
| `loss_pct` | Percentual de perda: `(sent - received) / sent` |
| `rtt_min_ms` / `rtt_avg_ms` / `rtt_max_ms` | Latência de ida-e-volta: mínima, média e máxima |
| `rtt_stddev_ms` | Desvio padrão do RTT (quanto maior, mais "instável"/com jitter é o enlace) |
| `throughput_kbps` | Vazão efetiva limitada por RTT (`NA` para Zigbee - ver metodologia) |

## Cenários de teste

Rode os quatro sketches-A/B em cada cenário abaixo e anote os resultados
(veja [Formato dos resultados](#formato-dos-resultados)). Sugestão de ordem:
do mais controlado para o mais realista.

1. **Baseline (linha de visada, curta distância).** As duas placas a ~1 m,
   sem obstáculos, sem outras redes por perto. Estabelece o "melhor caso" de
   cada tecnologia.
2. **Alcance.** Aumente a distância em degraus (ex.: 5 m, 10 m, 20 m, 30 m)
   até a taxa de perda ficar alta ou o link cair. Anote a distância em que
   cada tecnologia deixa de ser confiável (ex.: perda > 20%).
3. **Obstáculos.** Na distância do baseline, repita com 1 parede e depois
   2 paredes (ou um andar) entre as placas. Zigbee e BLE costumam sofrer
   menos que Wi-Fi/ESP-NOW em obstáculos densos por operarem com potência e
   taxa menores, mas isso é exatamente o que este cenário verifica.
4. **Tamanho de payload.** Já é automático dentro de cada sketch de
   Wi-Fi/ESP-NOW/BLE (eles alternam sozinhos entre os tamanhos definidos em
   `PAYLOAD_SIZES`). Vale comparar como o `throughput_kbps` cresce (ou não)
   com payloads maiores em cada tecnologia.
5. **Interferência / múltiplas equipes.** Se houver mais de uma dupla de
   placas testando ao mesmo tempo na mesma sala (comum em um evento de
   divulgação com várias bancadas), rode o mesmo cenário de baseline com
   1, 2 e 3 duplas ativas simultaneamente e compare a degradação. Lembre-se
   de mudar `TEAM_ID`/SSID/nome anunciado de cada dupla (ver
   [Solução de problemas](#solução-de-problemas)) para não misturar os
   pings de duplas diferentes.
6. **(Opcional) Consumo de energia.** Com um medidor de corrente USB entre a
   fonte e a estação B, compare a corrente média em repouso e durante o
   teste de cada tecnologia.

## Montagem física

Boa notícia: **nenhuma solda, botão ou LED extra é necessário** para estes
testes (diferente do estudo interativo em [`../com-zigbee/`](../com-zigbee/),
que usa D1/D2). O benchmark roda só com firmware + Monitor Serial.

Para cada cenário:

1. **Duas placas XIAO ESP32-C6** e dois cabos USB-C **com linhas de dados**
   (alguns cabos são só de alimentação - se a porta não aparecer no
   computador, troque o cabo antes de qualquer outra coisa).
2. **Estação A (iniciadora)** fica ligada por USB a um computador, para você
   acompanhar os resultados pelo Monitor Serial (115200 baud).
3. **Estação B (respondedora)** pode ficar:
   - ligada por USB em outra porta do mesmo computador (mais fácil para o
     baseline, e permite ver os logs dela também), ou
   - alimentada por um power bank USB, ou
   - alimentada por uma bateria LiPo 3,7 V no conector `BAT+`/`BAT-` da
     placa (ver [`../com-zigbee/docs/pinagem-verso.png`](../com-zigbee/docs/pinagem-verso.png))
     - útil justamente para os cenários de alcance/obstáculo, em que B
     precisa se afastar do computador.
4. Para o cenário de alcance, use uma trena ou marcações no chão para manter
   as distâncias consistentes entre uma tecnologia e outra.
5. Mantenha as placas na mesma orientação entre os testes (a antena da XIAO
   ESP32-C6 fica em uma ponta da placa - ver
   [`../com-zigbee/docs/pinagem-frente.png`](../com-zigbee/docs/pinagem-frente.png)).
   Isso não muda o resultado absoluto, mas mantém os testes comparáveis entre
   si.
6. Ao trocar de tecnologia, é só regravar os dois sketches daquela pasta nas
   mesmas duas placas - não precisa de hardware diferente.

Dica: cole um pedaço de fita com "A" e "B" em cada placa durante a sessão de
testes, já que o papel de cada uma muda conforme a pasta (iniciadora vs.
respondedora), diferente da convenção coordenador/end device do estudo
Zigbee interativo.

## Configuração do Arduino IDE

Todas as tecnologias usam a mesma placa (`XIAO_ESP32C6`, pacote **esp32 by
Espressif Systems**, testado aqui na versão **3.3.11**). Wi-Fi, ESP-NOW e BLE
já vêm inclusos no pacote da placa - não é preciso instalar nenhuma
biblioteca extra.

| | Estação A | Estação B |
|---|---|---|
| **Wi-Fi** | Board: XIAO_ESP32C6 (configuração padrão) | Board: XIAO_ESP32C6 (configuração padrão) |
| **ESP-NOW** | Board: XIAO_ESP32C6 (configuração padrão) | Board: XIAO_ESP32C6 (configuração padrão) |
| **BLE** | Board: XIAO_ESP32C6 (configuração padrão) | Board: XIAO_ESP32C6 (configuração padrão) |
| **Zigbee** | Board: XIAO_ESP32C6 · Tools → Zigbee Mode: **Zigbee ZCZR** · Partition Scheme: **Zigbee ZCZR 4MB with spiffs** · Erase All Flash Before Sketch Upload: **Enabled** | Board: XIAO_ESP32C6 · Tools → Zigbee Mode: **Zigbee ED** · Partition Scheme: **Zigbee 4MB with spiffs** · Erase All Flash Before Sketch Upload: **Enabled** |

As opções de Zigbee são as mesmas já documentadas em
[`../com-zigbee/readme.md`](../com-zigbee/readme.md).

> Todos os oito sketches deste diretório foram compilados com sucesso contra
> este exato pacote (`esp32:esp32:XIAO_ESP32C6`, core 3.3.11) antes de serem
> adicionados ao repositório. O que **não** foi validado aqui é o
> comportamento em rádio real (RF) - alcance, MTU negociado, robustez do
> binding Zigbee etc. - já que isso depende do hardware físico e do
> ambiente, exatamente o que este testbed existe para medir.

## Procedimento de execução

Para cada tecnologia (pasta `wifi/`, `espnow/`, `ble/` ou `zigbee/`):

1. Abra `estacao_a.ino` no Arduino IDE. Se aparecer um aviso pedindo para
   mover o arquivo para uma pasta de mesmo nome, aceite (é uma exigência do
   Arduino IDE para sketches avulsos).
2. Ajuste as configurações de Tools de acordo com a tabela acima e grave na
   placa que será a **estação A**.
3. Repita para `estacao_b.ino` na segunda placa (**estação B**), com as
   configurações de B.
4. Ligue **as duas placas juntas** (todo protocolo de descoberta - HELLO do
   Wi-Fi, broadcast do ESP-NOW, scan do BLE, binding do Zigbee - começa do
   zero a cada boot; ligar uma muito antes da outra só faz esperar mais, não
   quebra nada, mas religar as duas juntas evita confusão).
5. Abra o Monitor Serial da **estação A** a 115200 baud. Acompanhe as
   mensagens de descoberta ("Waiting for Station B...", "Scanning...",
   etc.) até aparecer "Link ready" ou "Connected".
6. O benchmark começa sozinho alguns segundos depois do link ficar pronto e
   imprime uma linha `RESULT,...` por tamanho de payload testado.
7. Ao final ("Benchmark complete"), posicione as placas no próximo cenário
   e envie `r` + Enter pelo Monitor Serial da estação A para rodar de novo -
   não precisa resetar nem regravar nada entre repetições no mesmo cenário.
8. Copie as linhas `RESULT,...` para a planilha de resultados (ver abaixo)
   antes de mudar de cenário.

## Formato dos resultados

Cada linha `RESULT,...` impressa por A já está pronta em CSV, mas não sabe
em que cenário físico ela foi gerada - isso só quem está com as placas na
mão sabe. Use [`resultados/template.csv`](./resultados/template.csv) como
ponto de partida: copie o arquivo (ex.: `resultados/2026-09-14.csv`) e, para
cada linha `RESULT,...`, preencha também `data`, `cenario` (ex.:
`baseline`, `alcance-10m`, `obstaculo-2paredes`), `distancia_m`, `obstaculo`
e `observacoes`.

Com os CSVs preenchidos, qualquer planilha consegue montar gráficos
comparando as quatro tecnologias por cenário - por exemplo, RTT médio vs.
distância, ou perda de pacotes vs. número de equipes simultâneas.

## Estrutura de pastas

```text
desempenho/
├── readme.md              este arquivo
├── wifi/
│   ├── estacao_a.ino       SoftAP + UDP, iniciadora
│   └── estacao_b.ino       Station + UDP, respondedora (echo)
├── espnow/
│   ├── estacao_a.ino       broadcast ESP-NOW, iniciadora
│   └── estacao_b.ino       broadcast ESP-NOW, respondedora (echo)
├── ble/
│   ├── estacao_a.ino       central/cliente GATT, iniciadora
│   └── estacao_b.ino       peripheral/servidor GATT, respondedora (echo)
├── zigbee/
│   ├── estacao_a.ino       coordinator, iniciadora do benchmark
│   └── estacao_b.ino       end device, respondedora (echo)
└── resultados/
    └── template.csv        modelo de planilha para consolidar os RESULT
```

## Solução de problemas

- **Porta serial não aparece.** Troque o cabo USB-C (muitos são só de
  carga). A XIAO ESP32-C6 usa o USB nativo do próprio chip, então não deve
  precisar de driver extra em Linux/macOS.
- **Wi-Fi: B nunca conecta.** Confirme que `WIFI_SSID`/`WIFI_PASSWORD` em
  `estacao_a.ino` e `estacao_b.ino` são idênticos. A senha precisa ter pelo
  menos 8 caracteres (exigência do WPA2).
- **BLE: "Service not found" / "Characteristics not found".** Confirme que
  `DEVICE_NAME` é idêntico nos dois arquivos. Se A conectar no dispositivo
  errado (outra dupla testando por perto), mude o nome nos dois arquivos
  (ex.: `XIAO-PERF-B-2`).
- **BLE: payloads grandes são pulados ("Skipping payload...").** A MTU
  negociada ficou menor que o payload testado. Confira no Monitor Serial de
  A a linha "Negotiated MTU" logo após conectar.
- **ESP-NOW ou Wi-Fi: pings de outra dupla aparecendo nos resultados /
  timeouts estranhos com várias bancadas ligadas.** Mude `TEAM_ID` (ESP-NOW)
  ou o número final de `WIFI_SSID` (Wi-Fi) nos dois arquivos da dupla -
  todas as duplas da sala precisam usar valores diferentes.
- **Zigbee: A fica preso em "Waiting for Station B...".** Confirme
  `Erase All Flash Before Sketch Upload: Enabled` nas duas placas antes de
  gravar (mesma pegadinha já documentada em
  [`../com-zigbee/readme.md`](../com-zigbee/readme.md)) e religue as duas
  placas juntas.
- **Resultados inconsistentes entre repetições do mesmo cenário.** Rode pelo
  menos 3 repetições por cenário e compare a média - RF sofre variação
  natural (reflexos, outras redes 2,4 GHz por perto, até pessoas se
  movendo no ambiente).

## Referências

- [Seeed Studio - XIAO ESP32-C6 (documentação oficial)](https://wiki.seeedstudio.com/pt-br/xiao_esp32c6_getting_started/)
- [Espressif - Arduino core para ESP32 (`arduino-esp32`)](https://github.com/espressif/arduino-esp32)
- [Espressif - Documentação do ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)
- [Estudo anterior deste repositório: comunicação Zigbee entre duas XIAO](../com-zigbee/)
