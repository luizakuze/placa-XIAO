# Como rodar o testbed - passo a passo de bancada

Roteiro prático para o dia do teste. O [`readme.md`](./readme.md) explica *o
que* cada métrica significa e *por quê*; aqui é só a ordem das coisas, do
cabo na mesa até os gráficos prontos.

Tempo: cada repetição do benchmark leva menos de um minuto. O que consome
tempo é gravar as placas e arrastá-las pelos cenários.

## 1. O que precisa estar na mesa

- **2 placas XIAO ESP32-C6.**
- **1 cabo USB-C com linha de dados** (o mínimo) e alimentação para a segunda
  placa. Muitos cabos são só de carga - se a porta não aparecer no
  computador, troque o cabo antes de investigar qualquer outra coisa.
- **Um hub USB**, se o computador tiver poucas portas. Vale mais do que
  parece: alimentar a estação B por carregador de parede ou power bank se
  mostrou instável - a placa responde os primeiros pacotes e para, por não
  sustentar o pico de corrente da transmissão. Com as duas no hub, o teste
  roda sem perda nenhuma no baseline.
- **Trena** ou marcações no chão.
- **Fita** marcando "A" e "B". As placas são idênticas e o papel de cada uma
  muda conforme a pasta - sem a fita, a confusão é questão de tempo.

### A placa precisa de mais alguma coisa ligada nela?

**Não.** Nada de LED, botão, resistor, jumper ou solda - o benchmark usa
apenas o rádio e a porta serial. Se sobrou a fiação do estudo interativo em
[`../com-zigbee/`](../com-zigbee/) (que usa D1/D2), ela não atrapalha.

## 2. Preciso do Arduino IDE?

**Só para gravar os sketches.** A coleta roda no terminal, com
[`ferramentas/capturar_serial.py`](./ferramentas/capturar_serial.py), que
**substitui o Monitor Serial**: mostra a mesma saída e repassa o que você
digitar (por exemplo `r` + Enter).

> **Importante:** o Monitor Serial do Arduino IDE e o script não podem usar a
> porta ao mesmo tempo. Feche o Monitor Serial antes de rodar o script - e
> feche o script antes de dar Upload, senão o IDE falha com "port is busy".

## 3. Três coisas que valem para as quatro tecnologias

### Você não precisa descobrir a porta na mão

O script identifica sozinho qual porta é a estação A, abrindo cada uma e
lendo como a placa se apresenta:

```text
Procurando a estacao A entre: /dev/ttyACM0, /dev/ttyACM1
  /dev/ttyACM0: estacao B
  /dev/ttyACM1: estacao A
Estacao A encontrada em /dev/ttyACM1
```

Isso importa porque **o nome da porta não diz qual placa está do outro lado**,
e a numeração muda conforme a ordem em que você pluga. Para o **Upload no
Arduino IDE** você ainda precisa escolher a porta na mão: rode o script,
anote a porta que ele indicou, dê `Ctrl+C` e use essa em **Tools → Port**.

### Rode o script antes de energizar - ou aperte RESET

As linhas `LINKSETUP` e `MEMORY` são impressas **uma única vez**, no
`setup()`. Se você plugar a placa e só depois rodar o script, essa saída já
passou e você vê uma tela vazia.

Três formas de acertar:

- **Aperte o RESET da placa A** (o botão `R`, à esquerda do conector USB-C;
  o `B` do outro lado é BOOT) depois que o script já estiver conectado;
- **digite `r` + Enter** no script - repete o benchmark sem resetar;
- deixe o script rodando e só então energize as placas.

Desplugar e replugar o cabo faz o mesmo que o RESET, e é mais fácil.

### Como saber qual placa está no cabo

Pela primeira linha do boot:

```text
Station A ready...   |   Scanning for Station B...   |   Starting Station A as Zigbee Coordinator...
     -> é a iniciadora: é aqui que o cabo de dados fica
```

```text
Station B ready...   |   Connecting to Station A   |   Starting Station B as Zigbee End Device...
     -> é a respondedora: o cabo está na placa errada
```

A estação B **nunca** imprime linhas `RESULT` - o trabalho dela é ficar
calada ecoando os pacotes.

### O aviso da pasta no Arduino IDE

Os sketches já estão em subpastas com o mesmo nome do arquivo
(`wifi/estacao_a/estacao_a.ino`), que é o que o IDE exige - então o aviso
pedindo para mover o arquivo não deve mais aparecer.

## 4. Passo a passo por tecnologia

Ordem recomendada: **ESP-NOW → Wi-Fi → BLE → Zigbee**, da mais simples para a
mais chata de configurar. O Zigbee é o único que exige opções especiais em
Tools, então deixá-lo por último evita esquecer de voltar essas opções ao
gravar as outras.

Os exemplos usam o baseline (1 m, sem obstáculo). Para os outros cenários,
mude `--cenario`/`--distancia`/`--obstaculo` conforme a
[seção 6](#6-sequência-dos-cenários).

---

### 4.1 ESP-NOW - comece por aqui

Sem configuração especial e sem associação: o link fica pronto quase
instantaneamente. É o melhor lugar para descobrir se a instrumentação está
funcionando.

**Gravar**

1. **Cabo de dados na placa A.**
2. **File → Open** → `espnow/estacao_a/estacao_a.ino`.
3. **Tools → Board → esp32 → XIAO_ESP32C6.** Nenhuma outra opção muda.
4. **Tools → Port →** a porta que aparecer → **Upload**.
5. **Passe o cabo de dados para a placa B.**
6. **File → Open** → `espnow/estacao_b/estacao_b.ino` → **Tools → Port →**
   selecione de novo → **Upload**.
7. **Cabo de dados de volta na A**, energia na **B**, as duas no hub.

**Coletar**

```bash
cd ~/placa-XIAO/desempenho
python3 ferramentas/capturar_serial.py --cenario baseline --distancia 1 --obstaculo nenhum
```

**Saída esperada na estação A**

```text
Station A ready. MAC: 40:4C:CA:XX:XX:XX
Waiting for Station B...
Station B confirmed.
LINKSETUP,ESPNOW,<ms>
MEMORY,ESPNOW,...
Starting benchmark in 3s (make sure Station B is powered on)...

=== Starting ESP-NOW benchmark ===
RESULT,ESPNOW,16,100,100,0.00,100.00,...
...
=== Benchmark complete ===
```

**Confira: devem sair 4 linhas `RESULT`** (payloads 16, 32, 100 e 200).

> O `LINKSETUP` do ESP-NOW só termina quando o eco de B volta - registrar o
> peer localmente não prova que a outra placa está ligada. Se ficar preso em
> `Waiting for Station B...`, a estação B não está de pé.

---

### 4.2 Wi-Fi

A estação A cria a própria rede (SoftAP) e espera a B se associar.

**Gravar** - mesma sequência da 4.1, com `wifi/estacao_a/` e
`wifi/estacao_b/`. Board padrão, nada especial em Tools.

**Coletar**

```bash
python3 ferramentas/capturar_serial.py --cenario baseline --distancia 1 --obstaculo nenhum
```

**Saída esperada na estação A**

```text
Station A ready. SoftAP IP: 192.168.4.1
Waiting for Station B (HELLO)...
Station B found at 192.168.4.2:5000
LINKSETUP,WIFI,<ms>
MEMORY,WIFI,...
Link ready. Starting benchmark in 3s...

=== Starting Wi-Fi benchmark ===
RESULT,WIFI,16,100,100,0.00,100.00,...
...
=== Benchmark complete ===
```

**Confira: devem sair 6 linhas `RESULT`** (16, 32, 100, 200, 512 e 1200).

> Se travar em `Waiting for Station B (HELLO)...`, o problema é credencial:
> `WIFI_SSID` e `WIFI_PASSWORD` precisam ser idênticos nos dois arquivos, e a
> senha precisa ter 8+ caracteres.

---

### 4.3 BLE

A estação B anuncia, a A varre o ambiente e conecta.

**Gravar** - mesma sequência, com `ble/estacao_a/` e `ble/estacao_b/`. Board
padrão.

**Coletar**

```bash
python3 ferramentas/capturar_serial.py --cenario baseline --distancia 1 --obstaculo nenhum
```

**Saída esperada na estação A**

```text
Scanning for Station B...
Connecting to 40:4c:ca:xx:xx:xx
Connected. Negotiated MTU: 247
LINKSETUP,BLE,<ms>
MEMORY,BLE,...
Link ready. Starting benchmark in 3s...

=== Starting BLE benchmark ===
RESULT,BLE,16,100,100,0.00,100.00,...
...
=== Benchmark complete ===
```

**Confira: devem sair 4 linhas `RESULT`** (16, 32, 100 e 200) e a MTU
negociada deve ser **247**.

> Se aparecer `Skipping payload ... bytes: acima do MTU negociado`, a
> negociação caiu para o padrão (23). O ponto de 16 bytes continua válido,
> mas os payloads maiores daquela sessão não valem. Religue as duas placas.
>
> O RTT do BLE é governado pelo *connection interval* negociado, não pelo
> tamanho do dado - espere valores na casa das dezenas de milissegundos e
> praticamente iguais entre os payloads. Não é defeito; é o comportamento do
> protocolo, e vale registrar assim no relatório.

---

### 4.4 Zigbee - o único com Tools diferente

As opções de **Tools** mudam, e são **diferentes para A e para B**. Conferir
isso antes de cada Upload evita o travamento mais comum do teste.

**Gravar a estação A (coordenador)**

1. **Cabo de dados na placa A.**
2. **File → Open** → `zigbee/estacao_a/estacao_a.ino`.
3. **Tools → Board → esp32 → XIAO_ESP32C6.**
4. **Tools → Zigbee Mode → Zigbee ZCZR (coordinator/router).**
5. **Tools → Partition Scheme → Zigbee ZCZR 4MB with spiffs.**
6. **Tools → Erase All Flash Before Sketch Upload → Enabled.**
7. **Tools → Port →** a porta que aparecer → **Upload**.

**Gravar a estação B (end device)**

8. **Passe o cabo de dados para a placa B.**
9. **File → Open** → `zigbee/estacao_b/estacao_b.ino`.
10. **Tools → Zigbee Mode → Zigbee ED (end device).**  ← muda
11. **Tools → Partition Scheme → Zigbee 4MB with spiffs.**  ← muda
12. **Tools → Erase All Flash Before Sketch Upload → Enabled.**  (continua)
13. **Tools → Port →** selecione de novo → **Upload**.
14. **Cabo de dados de volta na A**, energia na **B**, as duas no hub.

**Coletar**

```bash
python3 ferramentas/capturar_serial.py --cenario baseline --distancia 1 --obstaculo nenhum
```

**Saída esperada na estação A**

```text
Starting Station A as Zigbee Coordinator...
Waiting for Station B...
......
Station B discovered at address 0x1A2B
LINKSETUP,ZIGBEE,<ms>
MEMORY,ZIGBEE,...
Link ready. Starting benchmark in 3s...

=== Starting Zigbee benchmark ===
RESULT,ZIGBEE,16,100,100,0.00,100.00,...
RESULT,ZIGBEE,32,100,100,0.00,100.00,...
=== Benchmark complete ===
```

**Confira: devem sair 2 linhas `RESULT`** (16 e 32). O Zigbee para aí porque
o quadro do 802.15.4 tem 127 bytes e não comporta 100 nem 200 - é
característica da tecnologia, não limitação do teste.

> Os pontinhos de `Waiting for Station B...` podem demorar - o Zigbee é o que
> mais leva para associar, e é isso que o `LINKSETUP` captura. Se passar de
> uns 2 minutos, quase sempre é `Erase All Flash Before Sketch Upload` que
> não estava **Enabled** em uma das duas placas.

## 5. Confira a primeira coleta antes de seguir

Depois da primeira rodada (a de ESP-NOW), pare e verifique - é barato agora,
caro depois que as placas já se moveram:

- [ ] apareceu `LINKSETUP,<TECH>,<ms>`;
- [ ] apareceu `MEMORY,<TECH>,...` logo depois;
- [ ] nas linhas `RESULT`, os **três últimos campos** (RSSI médio, mínimo, nº
      de amostras) têm números - `NA,NA,0` significa que a leitura de sinal
      não funcionou;
- [ ] o número de linhas `RESULT` bate com o esperado da seção 4;
- [ ] abra `resultados/<data>.csv` e veja se as colunas de RSSI e de memória
      estão preenchidas.

## 6. Sequência dos cenários

Rode as quatro tecnologias em cada cenário antes de passar para o próximo.

| Cenário | `--cenario` | `--distancia` | `--obstaculo` |
|---|---|---|---|
| Baseline | `baseline` | `1` | `nenhum` |
| Alcance | `alcance-5m`, `alcance-10m`, ... | `5`, `10`, ... | `nenhum` |
| Barreira | `barreira-5m` | `5` (fixa) | `1 parede` |

Exemplos:

```bash
python3 ferramentas/capturar_serial.py --cenario alcance-5m --distancia 5 --obstaculo nenhum

python3 ferramentas/capturar_serial.py --cenario barreira-5m --distancia 5 --obstaculo "1 parede" \
  --observacoes "placas alinhadas, parede de alvenaria entre elas"
```

Regras práticas:

- **5 repetições por condição**: `r` + Enter quatro vezes depois da primeira.
  Cada `r` incrementa a coluna `repeticao` no CSV.
- **Memória e link setup precisam de boots, não de repetições.** O `r` não
  reinicia a placa, então `LINKSETUP` e `MEMORY` continuam os mesmos. Para ter
  variabilidade neles, faça **5 execuções separadas do script** por
  tecnologia - cada uma é uma `sessao` nova.
- Ao **mudar de cenário**, `Ctrl+C` e relance com os novos parâmetros.
- **Não precisa regravar nada** ao mudar de cenário - só ao mudar de
  tecnologia.
- Mantenha a **mesma orientação** das placas entre os testes, e registre o
  arranjo em `--observacoes`.
- No alcance, suba os degraus até a perda ficar alta ou o link cair, e anote
  onde cada tecnologia deixou de ser confiável (PDR abaixo de 80%).

Tudo é anexado ao mesmo `resultados/<data>.csv`, que abre direto no
LibreOffice/Excel/Sheets.

## 7. Analisar

```bash
python3 ferramentas/analisar_resultados.py
```

Sai o resumo em texto - incluindo a tabela do **núcleo comum**, que é a
comparável entre tecnologias - e 10 gráficos PNG em `resultados/graficos/`.

Use `--sem-graficos` para ver só o resumo, sem precisar de matplotlib.

Os gráficos de distância, PDR e sensibilidade só ganham forma depois dos
cenários de 5 m e com barreira - com um cenário só, eles viram alguns pontos
isolados.

## 8. Se der errado

| Sintoma | O que verificar |
|---|---|
| A porta não aparece em `ls /dev/ttyACM*` | Cabo USB-C sem linha de dados. Troque antes de qualquer outra coisa |
| "Porta ocupada" / `[Errno 16] Device or resource busy` | O Monitor Serial do Arduino IDE está aberto. Feche-o. Para ver quem segura a porta: `fuser -v /dev/ttyACM0` |
| Upload falha com `No such file or directory: '/dev/ttyACMx'` | O IDE está apontando para uma porta que não existe mais. **Tools → Port** e selecione de novo |
| Conectou mas **não aparece nada** | A placa bootou antes do script conectar. Aperte RESET ou digite `r` + Enter. Ver seção 3 |
| Aparece `Station B ready` / `Waiting for Station A...` | O cabo de dados está na respondedora. Passe-o para a outra placa |
| Enxurrada de `TIMEOUT` com as placas a 1 m | A estação B não está respondendo. Quase sempre é alimentação - use o hub USB |
| A estação B responde os primeiros pacotes e para | Mesma causa: não sustenta o pico de corrente da transmissão e reinicia |
| Wi-Fi: preso em `Waiting for Station B (HELLO)...` | `WIFI_SSID`/`WIFI_PASSWORD` idênticos nos dois arquivos; senha com 8+ caracteres |
| BLE: `Service not found` | `DEVICE_NAME` idêntico nos dois arquivos. Outra dupla testando perto? Troque o nome nos dois |
| BLE: `Skipping payload...` | A MTU negociada caiu para 23. Religue as duas placas |
| Zigbee: preso em `Waiting for Station B...` | `Erase All Flash Before Sketch Upload: Enabled` nas **duas** placas, e religue juntas |
| Pings de outra dupla aparecendo | Mude `TEAM_ID` (ESP-NOW), `WIFI_SSID` (Wi-Fi) ou `DEVICE_NAME` (BLE) nos dois arquivos da dupla |
| Resultados inconsistentes entre repetições | Normal em RF. Por isso 5 repetições e comparação por mediana |
