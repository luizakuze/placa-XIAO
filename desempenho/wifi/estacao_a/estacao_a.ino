#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <algorithm>

// ============================================================
// Estacao A - Wi-Fi (SoftAP) - Iniciadora do teste ping-pong
//
// A cria a propria rede Wi-Fi (SoftAP) e atua como servidor UDP.
// Nao depende de roteador/internet: A e B formam um enlace direto.
// ============================================================

// -------------------- Wi-Fi --------------------

// Precisa ser IDENTICO em estacao_b.ino.
// Se houver mais de uma dupla de placas testando na mesma sala ao
// mesmo tempo, troque o numero final (ex.: "IFSC-Perf-2") nas DUAS
// estacoes dessa dupla para evitar que se conectem na rede errada.
constexpr char WIFI_SSID[] = "IFSC-Perf-1";

// WPA2 exige no minimo 8 caracteres.
constexpr char WIFI_PASSWORD[] = "telecom123";

// Canal 2.4 GHz fixo. Use para o cenario de interferencia
// (testar em canais diferentes, ex.: 1, 6, 11).
constexpr uint8_t WIFI_CHANNEL = 6;

constexpr uint16_t UDP_PORT = 5000;

WiFiUDP udp;

// -------------------- Protocolo --------------------

constexpr uint8_t TYPE_HELLO = 0x01;
constexpr uint8_t TYPE_DATA = 0x02;

// 1 byte tipo + 4 bytes sequencia + 4 bytes timestamp (micros)
constexpr size_t HEADER_SIZE = 9;

// Tamanhos de payload de aplicacao testados, em bytes (nao incluem o cabecalho acima).
// UDP puro aguenta bem mais, mas 1200 fica seguro abaixo do MTU
// tipico de 1472 bytes e evita fragmentacao.
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
constexpr size_t PAYLOAD_SIZES[] = {16, 32, 100, 200, 512, 1200};
//
// O nucleo comum das QUATRO tecnologias e {16, 32}. Wi-Fi, ESP-NOW e
// BLE tambem compartilham {100, 200}. Para comparacao direta entre todas,
// use 16 ou 32 bytes; os demais tamanhos servem para a curva individual
// de cada tecnologia.
constexpr size_t NUM_PAYLOAD_SIZES = sizeof(PAYLOAD_SIZES) / sizeof(PAYLOAD_SIZES[0]);
constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + 1200;

// -------------------- Parametros do teste --------------------

constexpr uint16_t PING_COUNT = 100;
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
// calcular o throughput. Ler a cada ping so encareceria o lote: com
// as placas paradas o sinal nao muda de pacote para pacote.
constexpr uint16_t RSSI_SAMPLE_EVERY = 10;

// RSSI real e sempre negativo (dBm), entao 127 serve de "nao foi
// possivel ler" sem precisar de uma flag separada.
constexpr int RSSI_INVALID = 127;

// -------------------- Buffers --------------------

uint8_t txBuf[MAX_PACKET_SIZE];
uint8_t rxBuf[MAX_PACKET_SIZE];

IPAddress peerIP;
uint16_t peerPort = 0;

// ------------------------------------------------------------
// Estatisticas de uma bateria de PING_COUNT pings
//
// Definida antes das funcoes abaixo: o gerador automatico de
// prototipos do Arduino insere as declaracoes logo antes da
// primeira funcao do arquivo, entao qualquer struct usada como
// parametro precisa existir antes dela.
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
// tempo cada tecnologia leva desde o boot até o link com a
// estacao B ficar pronto (ver LINKSETUP no setup()).
unsigned long linkSetupStartMs = 0;

// ------------------------------------------------------------
// Custo de memoria do protocolo
//
// Heap livre em tres momentos do boot, para separar o que cada
// camada cobra de RAM:
//   heapBeforeRadio - depois do Serial.begin(), antes de qualquer
//                     chamada da pilha de radio;
//   heapAfterRadio  - depois de o SoftAP e o socket UDP subirem;
//   heapAfterLink   - depois de a estacao B estar associada e
//                     conhecida (IP/porta aprendidos).
// A diferenca entre o primeiro e o segundo e o custo fixo do
// protocolo; entre o segundo e o terceiro, o custo do enlace com
// a outra placa (ver MEMORY no setup()).
// ------------------------------------------------------------

uint32_t heapBeforeRadio = 0;
uint32_t heapAfterRadio = 0;
uint32_t heapAfterLink = 0;

void printMemoryReport() {
  Serial.printf(
    "MEMORY,WIFI,%lu,%lu,%lu,%ld,%ld,%lu\n",
    (unsigned long)heapBeforeRadio,
    (unsigned long)heapAfterRadio,
    (unsigned long)heapAfterLink,
    (long)heapBeforeRadio - (long)heapAfterRadio,
    (long)heapAfterRadio - (long)heapAfterLink,
    (unsigned long)ESP.getMinFreeHeap()
  );
}

// ------------------------------------------------------------
// Empacotamento de inteiros (mesma arquitetura nos dois lados,
// entao memcpy direto e suficiente - nao ha travessia de rede
// heterogenea aqui, o link e sempre XIAO-para-XIAO).
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
// Sensibilidade: RSSI (dBm) do enlace com a estacao B
//
// Como A e o SoftAP, o sinal que interessa e o que A recebe de B -
// exatamente o que o driver mantem na lista de estacoes associadas.
// Com uma unica estacao associada (o cenario deste testbed), a
// primeira entrada da lista e sempre a estacao B.
// ------------------------------------------------------------

int readRssiDbm() {
  wifi_sta_list_t staList;

  if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK || staList.num < 1) {
    return RSSI_INVALID;
  }

  return staList.sta[0].rssi;
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
// amostras - media/stddev acima nao usam isto, entao continuam
// validos mesmo se PING_COUNT crescer alem do que cabe na pilha.
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
// Aguarda o pacote HELLO da estacao B para aprender IP/porta dela.
// ------------------------------------------------------------

void waitForStationB() {
  Serial.println("Waiting for Station B (HELLO)...");

  while (peerPort == 0) {
    int packetSize = udp.parsePacket();

    if (packetSize >= 1) {
      udp.read(rxBuf, sizeof(rxBuf));

      if (rxBuf[0] == TYPE_HELLO) {
        peerIP = udp.remoteIP();
        peerPort = udp.remotePort();

        Serial.print("Station B found at ");
        Serial.print(peerIP);
        Serial.print(":");
        Serial.println(peerPort);
      }
    }

    delay(10);
  }
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

    txBuf[0] = TYPE_DATA;
    writeU32(txBuf + 1, seq);
    writeU32(txBuf + 5, micros());

    for (size_t i = HEADER_SIZE; i < packetSize; i++) {
      txBuf[i] = 0xAA;
    }

    udp.beginPacket(peerIP, peerPort);
    udp.write(txBuf, packetSize);
    udp.endPacket();

    uint32_t sendMicros = readU32(txBuf + 5);

    bool gotReply = false;
    unsigned long waitStart = millis();
    unsigned long waitStartMicros = micros();

    while (!gotReply && (millis() - waitStart) < PING_TIMEOUT_MS) {
      yield();  // cede a CPU unica do C6 para as tarefas de Wi-Fi/sistema

      int incomingSize = udp.parsePacket();

      // Todo datagrama precisa ser lido, mesmo os que nao interessam:
      // parsePacket() so busca um pacote novo depois que o anterior foi
      // consumido com read() (ver "if (rx_buffer) return 0" em
      // NetworkUdp.cpp do core). Filtrar por tamanho ANTES de ler deixa
      // o pacote entalado e a estacao A surda para sempre - e e
      // exatamente o que acontecia com os HELLO de 1 byte que a estacao
      // B continua enviando ate receber o primeiro dado.
      if (incomingSize > 0) {
        int len = udp.read(rxBuf, sizeof(rxBuf));

        if (
          len == (int)packetSize &&
          rxBuf[0] == TYPE_DATA &&
          readU32(rxBuf + 1) == seq
        ) {
          uint32_t rttMicros = micros() - sendMicros;
          recordRtt(stats, rttMicros / 1000.0);
          gotReply = true;
        }
        // Pacote antigo/duplicado: ignora e continua esperando.
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

  Serial.print("RESULT,WIFI,");
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
  Serial.println("=== Starting Wi-Fi benchmark ===");
  Serial.println("RESULT,tech,payload_bytes,sent,received,loss_pct,pdr_pct,rtt_min_ms,rtt_avg_ms,rtt_max_ms,rtt_stddev_ms,throughput_kbps,rtt_p50_ms,rtt_p90_ms,rtt_p95_ms,rtt_p99_ms,rssi_avg_dbm,rssi_min_dbm,rssi_amostras");

  for (size_t i = 0; i < NUM_PAYLOAD_SIZES; i++) {
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

  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL)) {
    Serial.println("Failed to start SoftAP.");
    delay(1000);
    ESP.restart();
  }

  Serial.println();
  Serial.print("Station A ready. SoftAP IP: ");
  Serial.println(WiFi.softAPIP());

  udp.begin(UDP_PORT);

  heapAfterRadio = ESP.getFreeHeap();

  waitForStationB();

  heapAfterLink = ESP.getFreeHeap();

  Serial.printf("LINKSETUP,WIFI,%lu\n", millis() - linkSetupStartMs);
  printMemoryReport();
  Serial.println("Link ready. Starting benchmark in 3s...");
  delay(3000);
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {
  runFullBenchmark();

  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'r' || c == 'R') break;
    }
    delay(50);
  }
}
