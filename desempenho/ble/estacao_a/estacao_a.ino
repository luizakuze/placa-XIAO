#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <algorithm>

// ============================================================
// Estacao A - BLE (Central/Client) - Iniciadora do teste ping-pong
//
// A varre o ambiente (scan) ate achar a estacao B pelo nome
// anunciado, conecta, escreve o ping na characteristic RX de B
// e mede o RTT quando a notificacao de eco chega na TX de B.
// ============================================================

// -------------------- BLE --------------------

// Precisa ser IDENTICO ao DEVICE_NAME de estacao_b.ino.
constexpr char DEVICE_NAME[] = "XIAO-PERF-B-1";

constexpr char SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
// Nomes na perspectiva da estacao B (igual nos dois arquivos):
constexpr char CHAR_UUID_RX[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // A escreve aqui
constexpr char CHAR_UUID_TX[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // A recebe notify daqui

constexpr uint16_t PREFERRED_MTU = 247;

// -------------------- Connection interval --------------------

// Em BLE o RTT nao e definido pelo tempo no ar, e sim pelo
// connection interval: o radio so troca dados nos "connection
// events", espacados por esse intervalo. Um ping-pong gasta dois
// eventos (um para o write de A, outro para o notify de B), entao
// o RTT minimo fica em ~2x o intervalo INDEPENDENTE do payload.
//
// Sem configurar nada, valem os defaults da pilha: a estacao B
// anuncia a faixa preferida de 50-80 ms e A aceita, o que trava o
// enlace em 50 ms. Foi o que aconteceu nas coletas de 22 e 23/09:
// rtt_avg = 99.98 ms igual para 16, 32, 100 e 200 bytes (2 eventos),
// com os outliers em degraus exatos de 149.9 ms (3 eventos). Aquele
// lote mediu o relogio do enlace, nao o radio, e o throughput virou
// artefato puro (payload x 8 x 100 / 10 s).
//
// 6 x 1.25 ms = 7.5 ms e o minimo permitido pelo padrao BLE. Com
// ele o RTT esperado cai para a casa dos 15-20 ms e volta a variar
// com o payload, que e o que este estudo compara.
constexpr uint16_t CONN_INTERVAL_MIN_UNITS = 6;  // 6 x 1.25 ms = 7.5 ms
constexpr uint16_t CONN_INTERVAL_MAX_UNITS = 6;  // idem: faixa fechada
constexpr uint16_t CONN_LATENCY = 0;             // B nao pula eventos
constexpr uint16_t CONN_TIMEOUT_UNITS = 500;     // 500 x 10 ms = 5 s

// O update e negociado de forma assincrona com o controlador; sem
// esta pausa o primeiro lote comecaria ainda no intervalo antigo e
// so os lotes seguintes veriam o intervalo curto.
constexpr unsigned long CONN_PARAMS_SETTLE_MS = 200;

// -------------------- Protocolo --------------------

// 4 bytes sequencia + 4 bytes timestamp (micros). Sem byte de
// "equipe" aqui: a conexao BLE ja e um enlace ponto a ponto
// exclusivo entre A e B depois do connect().
constexpr size_t HEADER_SIZE = 8;

// Payloads pequenos cabem no MTU padrao (23); os maiores dependem
// da negociacao de MTU feita em connectToStationB().
//
// 16 bytes de payload de aplicacao e o ponto de referencia do RTT:
// esse tamanho existe nas quatro tecnologias e permite comparar o mesmo
// volume util de dados entregue ao protocolo. O overhead de cada pilha
// continua diferente por natureza e faz parte do comportamento medido.
//
// Lista UNICA para as quatro tecnologias: cada uma testa esta mesma
// sequencia ate onde o radio dela alcanca. Nao ha nenhum tamanho
// escolhido "so para esta tecnologia" - o que muda e apenas onde a
// lista precisa parar, e isso e uma caracteristica fisica do radio:
//
//   Zigbee   16, 32                     (quadro 802.15.4 de 127 bytes)
//   ESP-NOW  16, 32, 100, 200           (limite de 250 bytes)
//   BLE      16, 32, 100, 200           (MTU negociada)
//   Wi-Fi    16, 32, 100, 200, 512, 1200
//
// 16 bytes de payload de aplicacao e o tamanho de referencia do estudo: o unico presente nas
// quatro, e e por ele que as latencias sao comparadas entre si.
constexpr size_t PAYLOAD_SIZES[] = {16, 32, 100, 200};
//
// O nucleo comum das QUATRO tecnologias e {16, 32}. Wi-Fi, ESP-NOW e
// BLE tambem compartilham {100, 200}. Para comparacao direta entre todas,
// use 16 ou 32 bytes; os demais tamanhos servem para a curva individual
// de cada tecnologia.
constexpr size_t NUM_PAYLOAD_SIZES = sizeof(PAYLOAD_SIZES) / sizeof(PAYLOAD_SIZES[0]);
constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + 200;

// -------------------- Parametros do teste --------------------

constexpr uint16_t PING_COUNT = 100;

// BLE tem latencia de enlace maior que Wi-Fi/ESP-NOW (depende do
// connection interval negociado), por isso o timeout e mais folgado.
// Timeout de um ping, IGUAL nas quatro tecnologias de proposito.
//
// O timeout e o criterio que separa "chegou" de "perdeu". Se cada
// tecnologia usasse um valor diferente, um pacote de 300 ms contaria
// como perdido numa e como recebido noutra, e a coluna loss_pct/pdr_pct
// deixaria de ser comparavel exatamente nos cenarios de alcance e
// obstaculo, que sao os unicos em que ha perda de verdade.
//
// 1000 ms e folgado para todas (o RTT tipico no baseline vai de 2 a
// 60 ms conforme a tecnologia). O preco e que um lote inteiramente
// perdido leva 100 s em vez de 15 s - aceitavel, e so acontece quando
// o enlace ja caiu.
constexpr unsigned long PING_TIMEOUT_MS = 1000;

// -------------------- Sensibilidade (RSSI) --------------------

// O RSSI e lido a cada RSSI_SAMPLE_EVERY pings, sempre DEPOIS de o
// RTT daquele ping ja ter sido registrado - a leitura nao entra na
// conta do RTT, e o tempo gasto nela e descontado do lote antes de
// calcular o throughput. Em BLE isso importa mais que nas outras
// tecnologias: getRssi() dispara um comando HCI e bloqueia ate a
// resposta do controlador, entao ler a cada ping distorceria o lote.
constexpr uint16_t RSSI_SAMPLE_EVERY = 10;

// RSSI real e sempre negativo (dBm), entao 127 serve de "nao foi
// possivel ler" sem precisar de uma flag separada.
constexpr int RSSI_INVALID = 127;

// ------------------------------------------------------------
// Estrutura de estatisticas - definida antes de qualquer funcao
// (ver nota em desempenho/wifi/estacao_a.ino).
// ------------------------------------------------------------

struct BenchStats {
  uint16_t sent = 0;
  uint16_t received = 0;
  double sumMs = 0;
  double sumSqMs = 0;
  double minMs = 1e9;
  double maxMs = 0;
  // Amostra crua de cada RTT recebido (na ordem de chegada), usada
  // so para calcular percentis depois - min/avg/max/stddev acima
  // continuam calculados incrementalmente, sem depender deste array.
  double samples[PING_COUNT];
  // Sensibilidade do enlace neste lote (ver RSSI_SAMPLE_EVERY).
  uint16_t rssiCount = 0;
  long rssiSum = 0;
  int rssiMin = RSSI_INVALID;
};

// Estampa de tempo do inicio do setup(), usada para medir quanto
// tempo o BLE leva desde o boot até a conexao (GATT) com a
// estacao B ficar pronta (ver LINKSETUP no loop()).
unsigned long linkSetupStartMs = 0;

// ------------------------------------------------------------
// Custo de memoria do protocolo
//
// Heap livre em tres momentos do boot, para separar o que cada
// camada cobra de RAM:
//   heapBeforeRadio - depois do Serial.begin(), antes de qualquer
//                     chamada da pilha BLE;
//   heapAfterRadio  - depois de BLEDevice::init() e do scan estarem
//                     configurados;
//   heapAfterLink   - depois da conexao GATT com B e da resolucao
//                     de servico/characteristics.
// A diferenca entre o primeiro e o segundo e o custo fixo do
// protocolo; entre o segundo e o terceiro, o custo da conexao
// (ver MEMORY no loop()).
// ------------------------------------------------------------

uint32_t heapBeforeRadio = 0;
uint32_t heapAfterRadio = 0;
uint32_t heapAfterLink = 0;

void printMemoryReport() {
  Serial.printf(
    "MEMORY,BLE,%lu,%lu,%lu,%ld,%ld,%lu\n",
    (unsigned long)heapBeforeRadio,
    (unsigned long)heapAfterRadio,
    (unsigned long)heapAfterLink,
    (long)heapBeforeRadio - (long)heapAfterRadio,
    (long)heapAfterRadio - (long)heapAfterLink,
    (unsigned long)ESP.getMinFreeHeap()
  );
}

// -------------------- Estado global --------------------

// Cliente GATT da conexao com B. Global (e nao local de
// connectToStationB) porque a leitura de RSSI precisa da conexao
// viva depois que aquela funcao ja retornou.
BLEClient *pClient = nullptr;

BLERemoteCharacteristic *pRxCharacteristic = nullptr;  // A escreve (ping)
BLERemoteCharacteristic *pTxCharacteristic = nullptr;  // A recebe notify (echo)

uint8_t txBuf[MAX_PACKET_SIZE];
uint8_t rxBuf[MAX_PACKET_SIZE];

volatile bool rxFlag = false;
volatile size_t rxLen = 0;

bool foundTarget = false;
bool linkReady = false;
BLEAdvertisedDevice *targetDevice = nullptr;
unsigned long lastScanStart = 0;

// ------------------------------------------------------------
// Empacotamento de inteiros (memcpy simples: mesma arquitetura
// nos dois lados, sem travessia de rede heterogenea).
// ------------------------------------------------------------

void writeU32(uint8_t *buf, uint32_t value) {
  memcpy(buf, &value, sizeof(value));
}

uint32_t readU32(const uint8_t *buf) {
  uint32_t value;
  memcpy(&value, buf, sizeof(value));
  return value;
}

void recordRtt(BenchStats &stats, double rttMs) {
  if (stats.received < PING_COUNT) {
    stats.samples[stats.received] = rttMs;
  }

  stats.received++;
  stats.sumMs += rttMs;
  stats.sumSqMs += rttMs * rttMs;

  if (rttMs < stats.minMs) stats.minMs = rttMs;
  if (rttMs > stats.maxMs) stats.maxMs = rttMs;
}

// ------------------------------------------------------------
// Sensibilidade: RSSI (dBm) da conexao com a estacao B
//
// getRssi() consulta o controlador por HCI e devolve 0 quando a
// leitura falha ou o cliente nao esta conectado - como 0 dBm nao e
// um RSSI plausivel neste cenario, tratamos esse retorno como
// leitura invalida.
// ------------------------------------------------------------

int readRssiDbm() {
  if (pClient == nullptr || !pClient->isConnected()) {
    return RSSI_INVALID;
  }

  int rssi = pClient->getRssi();

  return rssi == 0 ? RSSI_INVALID : rssi;
}

void recordRssi(BenchStats &stats) {
  int rssi = readRssiDbm();

  if (rssi == RSSI_INVALID) {
    return;
  }

  stats.rssiSum += rssi;
  stats.rssiCount++;

  if (rssi < stats.rssiMin) stats.rssiMin = rssi;
}

// ------------------------------------------------------------
// Ultimos campos da linha RESULT: media e minimo do RSSI no lote,
// mais quantas leituras entraram nessa conta. "NA" quando nenhuma
// leitura deu certo (ex.: a estacao B caiu no meio do lote).
// ------------------------------------------------------------

void printRssiFields(const BenchStats &stats) {
  Serial.print(",");

  if (stats.rssiCount > 0) {
    Serial.print((double)stats.rssiSum / stats.rssiCount, 1);
    Serial.print(",");
    Serial.print(stats.rssiMin);
  } else {
    Serial.print("NA,NA");
  }

  Serial.print(",");
  Serial.println(stats.rssiCount);
}

// ------------------------------------------------------------
// Percentis do RTT (interpolacao linear, mesmo metodo default do
// numpy.percentile), calculados sobre uma copia ordenada das
// amostras.
// ------------------------------------------------------------

void computePercentiles(
  const BenchStats &stats,
  double &p50, double &p90, double &p95, double &p99
) {
  uint16_t n = stats.received;

  if (n == 0) {
    p50 = p90 = p95 = p99 = 0;
    return;
  }

  double sorted[PING_COUNT];
  for (uint16_t i = 0; i < n; i++) sorted[i] = stats.samples[i];
  std::sort(sorted, sorted + n);

  auto interpolate = [&](double p) -> double {
    double rank = p * (n - 1);
    uint16_t lo = (uint16_t)rank;
    uint16_t hi = (lo + 1 < n) ? (lo + 1) : lo;
    double frac = rank - lo;
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
  };

  p50 = interpolate(0.50);
  p90 = interpolate(0.90);
  p95 = interpolate(0.95);
  p99 = interpolate(0.99);
}

// ------------------------------------------------------------
// Notificacao recebida da estacao B (roda fora do loop principal,
// so copia e sinaliza).
// ------------------------------------------------------------

void notifyCallback(
  BLERemoteCharacteristic *characteristic,
  uint8_t *data,
  size_t length,
  bool isNotify
) {
  (void)characteristic;
  (void)isNotify;

  if (length > sizeof(rxBuf)) {
    length = sizeof(rxBuf);
  }

  memcpy((void *)rxBuf, data, length);
  rxLen = length;
  rxFlag = true;
}

// ------------------------------------------------------------
// Scan: procura a estacao B pelo nome anunciado
// ------------------------------------------------------------

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (advertisedDevice.getName() == DEVICE_NAME) {
      BLEDevice::getScan()->stop();
      targetDevice = new BLEAdvertisedDevice(advertisedDevice);
      foundTarget = true;
    }
  }
};

void startScanning() {
  Serial.println("Scanning for Station B...");

  BLEScan *pScan = BLEDevice::getScan();

  // stop() antes de start() porque esta funcao tambem e chamada pela
  // rede de seguranca do loop(), quando o scan anterior pode ainda
  // estar rodando - e start() sobre um scan ativo e rejeitado.
  pScan->stop();
  pScan->start(0, false);

  lastScanStart = millis();
}

// ------------------------------------------------------------
// Conecta na estacao B e resolve o servico/characteristics
// ------------------------------------------------------------

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient *client) override {
    (void)client;
  }

  void onDisconnect(BLEClient *client) override {
    (void)client;
    linkReady = false;
    Serial.println("Disconnected from Station B.");
  }
};

bool connectToStationB() {
  Serial.print("Connecting to ");
  Serial.println(targetDevice->getAddress().toString().c_str());

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new ClientCallbacks());

  if (!pClient->connect(targetDevice)) {
    Serial.println("Connection failed.");
    return false;
  }

  pClient->setMTU(PREFERRED_MTU);

  BLERemoteService *pRemoteService = pClient->getService(SERVICE_UUID);
  if (pRemoteService == nullptr) {
    Serial.println("Service not found on Station B.");
    pClient->disconnect();
    return false;
  }

  pRxCharacteristic = pRemoteService->getCharacteristic(CHAR_UUID_RX);
  pTxCharacteristic = pRemoteService->getCharacteristic(CHAR_UUID_TX);

  if (pRxCharacteristic == nullptr || pTxCharacteristic == nullptr) {
    Serial.println("Characteristics not found on Station B.");
    pClient->disconnect();
    return false;
  }

  if (pTxCharacteristic->canNotify()) {
    pTxCharacteristic->registerForNotify(notifyCallback);
  }

  // Sem isto o enlace fica no intervalo default e o RTT medido e o
  // do relogio da conexao, nao o do radio (ver CONN_INTERVAL_*).
  if (!pClient->updateConnParams(
        CONN_INTERVAL_MIN_UNITS,
        CONN_INTERVAL_MAX_UNITS,
        CONN_LATENCY,
        CONN_TIMEOUT_UNITS
      )) {
    Serial.println("AVISO: updateConnParams falhou - o enlace segue no intervalo default.");
  }

  delay(CONN_PARAMS_SETTLE_MS);

  // O controlador pode conceder um intervalo maior que o pedido, e
  // ai o RTT sobe junto. Quem imprime o valor que realmente passou
  // a valer e a estacao B (linha CONNPARAMS) - e esse numero, nao o
  // pedido aqui, que deve ir para as observacoes do CSV.
  Serial.print("Connected. Negotiated MTU: ");
  Serial.println(pClient->getMTU());

  return true;
}

// ------------------------------------------------------------
// Executa PING_COUNT round-trips para um tamanho de payload
// ------------------------------------------------------------

BenchStats runBenchmarkForSize(size_t payloadSize) {
  BenchStats stats;
  // payloadSize e apenas o payload de aplicacao. O cabecalho de
  // instrumentacao (sequencia/timestamp e, quando necessario, tipo)
  // nao entra nos 16/32/... bytes reportados no CSV.
  const size_t packetSize = HEADER_SIZE + payloadSize;

  unsigned long batchStartMicros = micros();

  // Tempo gasto lendo RSSI, descontado do lote antes de calcular o
  // throughput (a leitura e instrumentacao, nao trafego do teste).
  unsigned long rssiOverheadMicros = 0;

  // Tempo parado esperando pacotes que nunca chegaram. Sai da conta do
  // throughput: esperar um timeout nao e transmitir. Sem isso, com o
  // timeout unico de 1000 ms, um unico pacote perdido derrubaria a vazao
  // de um lote inteiro em varias vezes - e a coluna passaria a medir o
  // valor do timeout, nao o desempenho do radio.
  unsigned long timeoutWaitMicros = 0;

  for (uint16_t seq = 0; seq < PING_COUNT; seq++) {
    stats.sent++;

    writeU32(txBuf, seq);
    writeU32(txBuf + 4, micros());

    for (size_t i = HEADER_SIZE; i < packetSize; i++) {
      txBuf[i] = 0xAA;
    }

    uint32_t sendMicros = readU32(txBuf + 4);

    rxFlag = false;
    pRxCharacteristic->writeValue(txBuf, packetSize, false);

    bool gotReply = false;
    unsigned long waitStart = millis();
    unsigned long waitStartMicros = micros();

    while (!gotReply && (millis() - waitStart) < PING_TIMEOUT_MS) {
      yield();  // cede a CPU unica do C6 para a pilha BLE

      if (rxFlag) {
        rxFlag = false;
        size_t len = rxLen;

        if (len == packetSize && readU32(rxBuf) == seq) {
          uint32_t rttMicros = micros() - sendMicros;
          recordRtt(stats, rttMicros / 1000.0);
          gotReply = true;
        }
        // Notificacao antiga/duplicada: ignora e continua esperando.
      }
    }

    if (gotReply && (seq % RSSI_SAMPLE_EVERY) == 0) {
      unsigned long rssiStartMicros = micros();
      recordRssi(stats);
      rssiOverheadMicros += micros() - rssiStartMicros;
    }

    if (!gotReply) {
      timeoutWaitMicros += micros() - waitStartMicros;
      Serial.print("TIMEOUT seq=");
      Serial.println(seq);
    }
  }

  unsigned long elapsedMicros =
    micros() - batchStartMicros - rssiOverheadMicros - timeoutWaitMicros;

  double avgMs = stats.received > 0 ? stats.sumMs / stats.received : 0;
  double variance =
    stats.received > 0
      ? (stats.sumSqMs / stats.received) - (avgMs * avgMs)
      : 0;
  double stddevMs = variance > 0 ? sqrt(variance) : 0;
  double lossPct =
    stats.sent > 0
      ? 100.0 * (stats.sent - stats.received) / stats.sent
      : 0;

  // Packet Delivery Ratio: o complemento da perda, publicado como
  // coluna propria porque e a forma como a literatura de redes sem
  // fio costuma reportar confiabilidade de enlace.
  double pdrPct = 100.0 - lossPct;

  double elapsedSeconds = elapsedMicros / 1000000.0;
  // Taxa util do teste ping-pong (goodput do payload), nao vazao maxima do radio.
  // Como cada envio espera o eco antes do proximo, esta metrica incorpora o
  // comportamento stop-and-wait e deve ser apresentada com esse nome.
  double throughputKbps =
    elapsedSeconds > 0
      ? (payloadSize * 8.0 * stats.received) / elapsedSeconds / 1000.0
      : 0;

  double p50Ms, p90Ms, p95Ms, p99Ms;
  computePercentiles(stats, p50Ms, p90Ms, p95Ms, p99Ms);

  Serial.print("RESULT,BLE,");
  Serial.print(payloadSize);
  Serial.print(",");
  Serial.print(stats.sent);
  Serial.print(",");
  Serial.print(stats.received);
  Serial.print(",");
  Serial.print(lossPct, 2);
  Serial.print(",");
  Serial.print(pdrPct, 2);
  Serial.print(",");
  Serial.print(stats.received > 0 ? stats.minMs : 0, 3);
  Serial.print(",");
  Serial.print(avgMs, 3);
  Serial.print(",");
  Serial.print(stats.received > 0 ? stats.maxMs : 0, 3);
  Serial.print(",");
  Serial.print(stddevMs, 3);
  Serial.print(",");
  Serial.print(throughputKbps, 2);
  Serial.print(",");
  Serial.print(p50Ms, 3);
  Serial.print(",");
  Serial.print(p90Ms, 3);
  Serial.print(",");
  Serial.print(p95Ms, 3);
  Serial.print(",");
  Serial.print(p99Ms, 3);
  printRssiFields(stats);

  return stats;
}

void runFullBenchmark() {
  Serial.println();
  Serial.println("=== Starting BLE benchmark ===");
  Serial.println("RESULT,tech,payload_bytes,sent,received,loss_pct,pdr_pct,rtt_min_ms,rtt_avg_ms,rtt_max_ms,rtt_stddev_ms,throughput_kbps,rtt_p50_ms,rtt_p90_ms,rtt_p95_ms,rtt_p99_ms,rssi_avg_dbm,rssi_min_dbm,rssi_amostras");

  // MTU que o enlace realmente negociou, nao a preferida. Se a
  // negociacao falhar e a conexao ficar nos 23 bytes do default, um
  // payload de 100 ou 200 bytes seria truncado pela pilha e cada
  // ping contaria como timeout - o CSV registraria 100% de perda
  // como se fosse do radio. Melhor pular o tamanho e dizer por que.
  uint16_t negotiatedMtu = pClient->getMTU();

  if (negotiatedMtu < 23) {
    Serial.println("AVISO: MTU negociada invalida; assumindo o minimo de 23 bytes.");
    negotiatedMtu = 23;
  }

  for (size_t i = 0; i < NUM_PAYLOAD_SIZES; i++) {
    // Os 3 bytes sao o cabecalho ATT (opcode + handle), que ocupa
    // espaco dentro da MTU junto com o nosso pacote.
    if (HEADER_SIZE + PAYLOAD_SIZES[i] + 3 > negotiatedMtu) {
      Serial.print("Skipping payload ");
      Serial.print(PAYLOAD_SIZES[i]);
      Serial.print(" bytes: acima da MTU negociada (");
      Serial.print(negotiatedMtu);
      Serial.println(").");
      continue;
    }

    runBenchmarkForSize(PAYLOAD_SIZES[i]);
    delay(200);
  }

  Serial.println("=== Benchmark complete ===");
  Serial.println("Send 'r' + Enter in Serial Monitor to run again.");
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  linkSetupStartMs = millis();

  Serial.begin(115200);

  // Antes de qualquer chamada da pilha de radio (ver printMemoryReport).
  heapBeforeRadio = ESP.getFreeHeap();

  BLEDevice::setMTU(PREFERRED_MTU);
  BLEDevice::init("");

  BLEScan *pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->setActiveScan(true);

  heapAfterRadio = ESP.getFreeHeap();

  startScanning();
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {
  if (!foundTarget) {
    if (millis() - lastScanStart > 15000) {
      startScanning();  // rede de seguranca caso o scan pare sozinho
    }
    delay(100);
    return;
  }

  if (!linkReady) {
    linkReady = connectToStationB();

    if (!linkReady) {
      Serial.println("Retrying in 2s...");
      delay(2000);
      return;
    }

    heapAfterLink = ESP.getFreeHeap();

    Serial.printf("LINKSETUP,BLE,%lu\n", millis() - linkSetupStartMs);
    printMemoryReport();
    Serial.println("Link ready. Starting benchmark in 3s...");
    delay(3000);
  }

  runFullBenchmark();

  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'r' || c == 'R') break;
    }
    delay(50);
  }
}
