# Testbed de desempenho: Wi-Fi vs ESP-NOW vs BLE vs Zigbee

Este diretório compara quatro tecnologias de comunicação disponíveis na **XIAO ESP32-C6** usando duas placas iguais:

- **Estação A:** envia os pacotes e registra as métricas.
- **Estação B:** recebe cada pacote e devolve o mesmo conteúdo.

Para executar os testes, consulte [`como-rodar.md`](./como-rodar.md). Este README descreve a metodologia, as métricas e o formato dos resultados.

## Tecnologias comparadas

A XIAO ESP32-C6 utiliza um único rádio de 2,4 GHz compartilhado entre Wi-Fi, Bluetooth LE e 802.15.4. Por isso, cada tecnologia é testada separadamente.

| Tecnologia | Pasta | Camada usada | Topologia no teste |
|---|---|---|---|
| **Wi-Fi** | [`wifi/`](./wifi/) | 802.11, SoftAP + UDP | A cria o AP e B conecta como estação |
| **ESP-NOW** | [`espnow/`](./espnow/) | Rádio Wi-Fi sem associação/IP | Comunicação direta por broadcast |
| **BLE** | [`ble/`](./ble/) | Bluetooth 5 LE, GATT | B anuncia e A conecta como central |
| **Zigbee** | [`zigbee/`](./zigbee/) | 802.15.4 / Zigbee 3.0 | A é coordenador e B é end device |

O **Thread** também é suportado pelo ESP32-C6, mas não faz parte deste testbed. No Arduino-ESP32, o uso de OpenThread exige uma configuração mais complexa, incluindo Border Router, e não segue o mesmo modelo direto entre duas placas.

## Metodologia

### Ping-pong e RTT

Cada lote segue o mesmo fluxo:

1. A registra `t0 = micros()` e envia um pacote.
2. B recebe o pacote e devolve o mesmo conteúdo.
3. A recebe a resposta e calcula `RTT = micros() - t0`.
4. O processo é repetido `PING_COUNT` vezes, 100 por padrão.

O RTT é medido apenas com o relógio da estação A. Não é necessário sincronizar as duas placas.

Os testes usam transporte **best-effort** na camada medida: UDP no Wi-Fi, broadcast no ESP-NOW, GATT Write No Response no BLE e comando ZCL direto no Zigbee. Não há retransmissão implementada pelo benchmark para mascarar perdas.

### Parâmetros mantidos iguais

| Parâmetro | Valor | Observação |
|---|---:|---|
| `PING_COUNT` | 100 | Mesmo número de tentativas por lote |
| `PING_TIMEOUT_MS` | 1000 ms | Mesmo critério para considerar um pacote perdido |
| `RSSI_SAMPLE_EVERY` | 10 | Uma leitura de RSSI a cada 10 pings |
| Payload de referência | 16 bytes | Tamanho suportado pelas quatro tecnologias |
| Início do RTT | Antes do envio | `micros()` é lido imediatamente antes do pacote ser enviado |
| Estatísticas | Mesmo código | `recordRtt()` e `computePercentiles()` são equivalentes nos quatro testes |

O timeout de 1000 ms é mantido igual em todas as tecnologias para que PDR e perda sejam comparáveis.

## Payloads

### Definição de `payload_bytes`

`payload_bytes` representa apenas o **payload de aplicação**. Campos usados pela instrumentação, como número de sequência, timestamp e identificadores internos, não entram nesse valor.

Assim, `payload_bytes = 16` significa 16 bytes de dados de aplicação em todas as tecnologias.

| Tecnologia | Cabeçalho interno | Motivo |
|---|---:|---|
| ESP-NOW | 10 B | Inclui identificação da equipe em broadcast |
| Wi-Fi | 9 B | A associação ao AP identifica o par; permanece o campo de tipo |
| BLE | 8 B | A conexão GATT e a characteristic identificam o canal |
| Zigbee | 8 B | O binding e o `custom_cmd_id` identificam a comunicação |

### Tamanhos testados

| Tecnologia | Payloads testados | Limite utilizado |
|---|---|---|
| Zigbee | 16, 32 | Limite do quadro 802.15.4 após os cabeçalhos das camadas |
| ESP-NOW | 16, 32, 100, 200 | Limite do protocolo |
| BLE | 16, 32, 100, 200 | Dependente da MTU negociada, normalmente até 247 |
| Wi-Fi | 16, 32, 100, 200, 512, 1200 | Mantido abaixo do MTU usado no teste para evitar fragmentação |

Para comparar tecnologias entre si, use apenas os payloads presentes em todas elas. O [`analisar_resultados.py`](./ferramentas/analisar_resultados.py) calcula essa interseção automaticamente.

## Métricas coletadas

Cada `estacao_a.ino` imprime uma linha `RESULT,...` para cada lote.

| Coluna | Significado |
|---|---|
| `tech` | Tecnologia testada: `WIFI`, `ESPNOW`, `BLE` ou `ZIGBEE` |
| `payload_bytes` | Payload de aplicação em bytes |
| `sent` | Número de pings enviados |
| `received` | Número de respostas recebidas dentro do timeout |
| `loss_pct` | Percentual de perda |
| `pdr_pct` | Packet Delivery Ratio |
| `rtt_min_ms` | Menor RTT |
| `rtt_avg_ms` | RTT médio |
| `rtt_max_ms` | Maior RTT |
| `rtt_stddev_ms` | Desvio padrão do RTT |
| `rtt_p50_ms` | Percentil 50 do RTT |
| `rtt_p90_ms` | Percentil 90 do RTT |
| `rtt_p95_ms` | Percentil 95 do RTT |
| `rtt_p99_ms` | Percentil 99 do RTT |
| `throughput_kbps` | Goodput do fluxo ping-pong |
| `rssi_avg_dbm` | RSSI médio do lote |
| `rssi_min_dbm` | Pior RSSI registrado no lote |
| `rssi_amostras` | Número de leituras usadas no cálculo do RSSI |

### RSSI

O RSSI é coletado a cada 10 pings e permite relacionar qualidade de sinal com PDR e perda.

A leitura é feita depois do RTT do ping correspondente. O tempo gasto para obter o RSSI é descontado do cálculo de goodput.

| Tecnologia | Fonte do RSSI |
|---|---|
| Wi-Fi | `esp_wifi_ap_get_sta_list()` |
| ESP-NOW | `rx_ctrl->rssi` no callback de recepção |
| BLE | `BLEClient::getRssi()` |
| Zigbee | `esp_zb_nwk_get_next_neighbor()` |

### Memória

A estação A registra o heap livre em três momentos:

1. Antes de inicializar o rádio.
2. Depois de inicializar a pilha de comunicação.
3. Depois de estabelecer o enlace com B.

Também é registrado `ESP.getMinFreeHeap()`.

Essas medições são feitas uma vez por boot. Para medir variabilidade de memória, são necessários boots independentes.

### Tempo de estabelecimento do enlace

Após o link ficar pronto, a estação A imprime:

```text
LINKSETUP,<tech>,<ms>
```

O valor corresponde ao tempo desde o início do `setup()` até o enlace estar pronto para uso.

| Tecnologia | Critério de link pronto |
|---|---|
| Wi-Fi | B envia o `HELLO` UDP e A aprende IP e porta de B |
| ESP-NOW | A recebe uma resposta válida de B |
| BLE | `connect()` GATT e resolução de serviço/characteristics concluídos |
| Zigbee | End device associado e vinculado ao coordenador |

A linha de memória é:

```text
MEMORY,<tech>,<heap_antes_radio>,<heap_pos_radio>,<heap_pos_link>,<custo_radio_bytes>,<custo_link_bytes>,<min_free_heap>
```

`LINKSETUP` e `MEMORY` aparecem uma vez por boot. A ferramenta de captura associa esses valores às linhas `RESULT` da mesma sessão.

## Limitações do teste

Os resultados devem ser interpretados com estas restrições:

- `throughput_kbps` representa **goodput do fluxo ping-pong**, não vazão máxima do rádio. O teste é stop-and-wait.
- RSSI x PDR representa qualidade de enlace observada. Não é uma medição de sensibilidade de receptor em laboratório.
- `ESP.getFreeHeap()` mede o custo dinâmico de heap. Memória estática e global deve ser analisada separadamente a partir do uso de memória informado na compilação.
- A configuração de segurança não é equivalente entre Wi-Fi, ESP-NOW, BLE e Zigbee.
- Consumo de energia não é medido por este firmware.

Para medir vazão máxima seria necessário um teste separado, com transmissão contínua em uma direção e contagem de bytes em uma janela fixa.

## Protocolo de coleta

Mantenha posição, orientação, alimentação, canal e ambiente o mais constantes possível. Use `--observacoes` para registrar diferenças relevantes entre execuções.

### A. Baseline

- Distância: 1 m.
- Obstáculo: nenhum.
- 5 repetições por tecnologia.
- Cada repetição executa todos os payloads suportados pela tecnologia.

### B. Distância

- Distâncias mínimas: 1 m e 5 m.
- Adicionar 10 m se o ambiente permitir.
- Sem obstáculo.
- 5 repetições por distância e tecnologia.
- Para comparação direta, usar principalmente o payload de 16 bytes.

### C. Barreiras

- Manter a distância fixa, por exemplo 5 m.
- Testar sem obstáculo, com uma parede e outras barreiras de interesse.
- 5 repetições por condição e tecnologia.
- Usar 16 bytes como referência para PDR e RSSI.

### D. Memória e link setup

`LINKSETUP` e `MEMORY` são medidos uma vez por boot. Pressionar `r` cria uma nova repetição do benchmark, mas não uma nova medição de memória ou estabelecimento do enlace.

Para essas duas métricas, use pelo menos **5 boots independentes por tecnologia**. Cada nova execução do script de captura corresponde a uma nova sessão.

### Quantidade de pacotes

Cada payload usa 100 pings. Com 5 repetições, cada condição terá 500 tentativas por payload.

### Interferência opcional

Para avaliar interferência entre grupos, execute o baseline com 1, 2 e 3 duplas ativas ao mesmo tempo.

Ao usar mais de uma dupla, configure identificadores diferentes:

- `TEAM_ID` no ESP-NOW.
- `WIFI_SSID` no Wi-Fi.
- `DEVICE_NAME` no BLE.

## Montagem física

Não é necessário adicionar botão, LED ou circuito externo.

Material recomendado:

1. Duas placas **XIAO ESP32-C6**.
2. Cabo USB-C com dados para a estação conectada ao computador.
3. Alimentação estável para a segunda placa.
4. Hub USB se o computador tiver poucas portas.
5. Trena ou marcações no chão para manter as distâncias.
6. Identificação física das placas como A e B.

Mantenha a mesma orientação das placas em todos os testes. A posição da antena pode ser consultada em [`../com-zigbee/docs/pinagem-frente.png`](../com-zigbee/docs/pinagem-frente.png).

Para testes de alcance, a estação B pode ser alimentada por bateria LiPo 3,7 V no conector `BAT+`/`BAT-`. Consulte [`../com-zigbee/docs/pinagem-verso.png`](../com-zigbee/docs/pinagem-verso.png).

Nos testes realizados durante o desenvolvimento, alimentação por carregador de parede ou power bank apresentou instabilidade em alguns casos. Se B parar de responder durante transmissão, teste com alimentação USB mais estável ou hub USB.

## Configuração do Arduino IDE

Todos os testes usam a placa `XIAO_ESP32C6` com o pacote **esp32 by Espressif Systems**, testado na versão **3.3.11**.

Wi-Fi, ESP-NOW e BLE não exigem biblioteca adicional.

| Tecnologia | Estação A | Estação B |
|---|---|---|
| Wi-Fi / ESP-NOW / BLE | `XIAO_ESP32C6`, configuração padrão | `XIAO_ESP32C6`, configuração padrão |
| Zigbee | Zigbee Mode: **ZCZR**; Partition Scheme: **Zigbee ZCZR 4MB with spiffs**; Erase All Flash Before Sketch Upload: **Enabled** | Zigbee Mode: **ED**; Partition Scheme: **Zigbee 4MB with spiffs**; Erase All Flash Before Sketch Upload: **Enabled** |

Os oito sketches compilam com essa configuração. Alcance, MTU negociada e estabilidade do binding Zigbee devem ser validados nos testes reais.

## Coleta dos resultados

Os resultados podem ser gravados automaticamente ou preenchidos manualmente.

### Captura automática

O script [`ferramentas/capturar_serial.py`](./ferramentas/capturar_serial.py):

- conecta à serial da estação A;
- interpreta `RESULT`, `LINKSETUP` e `MEMORY`;
- adiciona data, hora, sessão, repetição e informações do cenário;
- grava os dados em CSV;
- mantém o terminal interativo para comandos como `r` + Enter.

Instalação e execução:

```bash
pip install pyserial
python3 ferramentas/capturar_serial.py --cenario baseline --distancia 1 --obstaculo nenhum
```

Sem `--port`, o script tenta identificar automaticamente qual porta corresponde à estação A. Para definir manualmente:

```bash
python3 ferramentas/capturar_serial.py --port /dev/ttyACM0 --cenario baseline --distancia 1 --obstaculo nenhum
```

Se `--distancia`, `--obstaculo` ou `--observacoes` não forem informados, o script solicita os valores durante a execução.

### Captura manual

Use [`resultados/template.csv`](./resultados/template.csv) como modelo.

Como `LINKSETUP` e `MEMORY` aparecem apenas uma vez por boot, repita esses valores nas linhas `RESULT` da mesma sessão.

### Colunas de contexto

| Coluna | Significado |
|---|---|
| `data` / `hora` | Momento da captura |
| `sessao` | Identificador do boot e da execução do script de captura |
| `repeticao` | Repetição do benchmark dentro da sessão |
| `cenario` | Tipo de cenário testado |
| `distancia_m` | Distância entre as placas |
| `obstaculo` | Condição de barreira |
| `observacoes` | Informações adicionais da montagem ou execução |

`sessao` e `repeticao` são campos diferentes. Uma sessão corresponde a um boot; uma mesma sessão pode conter várias repetições do benchmark.

## Análise dos resultados

O script [`ferramentas/analisar_resultados.py`](./ferramentas/analisar_resultados.py) lê os CSVs em `resultados/`, ignora `template.csv` e subpastas, imprime o resumo estatístico e gera gráficos em `resultados/graficos/`.

```bash
pip install matplotlib
python3 ferramentas/analisar_resultados.py
python3 ferramentas/analisar_resultados.py --sem-graficos
```

O resumo apresenta:

1. Resultados por tecnologia e cenário para todos os payloads.
2. Comparação entre tecnologias usando apenas o conjunto de payloads comum a todas.
3. Custo de memória por tecnologia.

Não compare diretamente médias calculadas sobre listas diferentes de payloads. Para comparação entre tecnologias, use o núcleo comum calculado pelo script.

### Gráficos gerados

| Arquivo | Conteúdo |
|---|---|
| `rtt_16bytes.png` | RTT médio para payload de 16 bytes |
| `comparativo_baseline.png` | RTT médio no baseline usando payloads comuns |
| `jitter_16bytes_baseline.png` | Desvio padrão do RTT no baseline, 16 bytes |
| `rtt_vs_distancia.png` | RTT médio por distância, com p95 como barra de erro |
| `perda_vs_distancia.png` | Perda por distância |
| `pdr_por_condicao_16bytes.png` | PDR por distância e obstáculo, 16 bytes |
| `pdr_vs_rssi.png` | PDR em função do RSSI |
| `throughput_vs_payload.png` | Goodput por payload no baseline |
| `link_setup_por_tecnologia.png` | Tempo de estabelecimento do enlace |
| `memoria_por_tecnologia.png` | Custo de memória do rádio e do enlace |

Nos gráficos de distância e PDR, o payload de referência é 16 bytes. Os gráficos de distância usam cenários sem obstáculo, enquanto as barreiras são analisadas separadamente. O gráfico de goodput usa apenas o cenário `baseline`.

## Estrutura de pastas

```text
desempenho/
├── readme.md
├── como-rodar.md
├── wifi/
│   ├── estacao_a/
│   └── estacao_b/
├── espnow/
│   ├── estacao_a/
│   └── estacao_b/
├── ble/
│   ├── estacao_a/
│   └── estacao_b/
├── zigbee/
│   ├── estacao_a/
│   └── estacao_b/
├── ferramentas/
│   ├── capturar_serial.py
│   └── analisar_resultados.py
└── resultados/
    ├── template.csv
    ├── descartados/
    └── graficos/
```

Os sketches ficam em subpastas com o mesmo nome do arquivo, conforme exigido pelo Arduino IDE.

## Solução de problemas

| Sintoma | Verificação |
|---|---|
| Porta serial não aparece | Verifique se o cabo USB-C possui linha de dados |
| `[Errno 16] Device or resource busy` | Feche o Monitor Serial. Use `fuser -v /dev/ttyACM0` para identificar o processo que está usando a porta |
| Conectou, mas não aparece saída | Pressione **RESET** ou envie `r` + Enter |
| Muitos `TIMEOUT` a 1 m | Verifique se a estação B está ligada e com alimentação estável |
| Wi-Fi parado em `Waiting for Station B (HELLO)...` | Confirme `WIFI_SSID` e `WIFI_PASSWORD` iguais nas duas estações. A senha deve ter pelo menos 8 caracteres |
| BLE: `Service not found` | Confirme `DEVICE_NAME` igual nas duas estações. Se houver outra dupla próxima, use outro nome |
| BLE: `Skipping payload...` | A MTU negociada ficou em 23. Reinicie as placas. O teste com 16 bytes continua válido |
| Zigbee parado em `Waiting for Station B...` | Use `Erase All Flash Before Sketch Upload: Enabled` nas duas placas e reinicie ambas |
| Pacotes de outra dupla aparecem | Altere `TEAM_ID`, `WIFI_SSID` ou `DEVICE_NAME`, conforme a tecnologia |
| Resultados variam entre repetições | Variações são esperadas em RF. Use as 5 repetições previstas no protocolo e analise a distribuição das medições |

## Referências

- [Seeed Studio - XIAO ESP32-C6](https://wiki.seeedstudio.com/pt-br/xiao_esp32c6_getting_started/)
- [Espressif - Arduino core para ESP32](https://github.com/espressif/arduino-esp32)
- [Espressif - ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)
- [Estudo anterior: comunicação Zigbee entre duas XIAO](../com-zigbee/)