#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

// ============================================================
// Estacao A - ESP-NOW - Iniciadora do teste ping-pong
//
// ESP-NOW e um protocolo da Espressif que usa o radio Wi-Fi em
// modo "connectionless": sem roteador, sem IP, sem associacao.
// As duas placas trocam quadros diretamente pelo endereco MAC.
// Aqui usamos o endereco de broadcast dos dois lados, entao nao
// e preciso descobrir o MAC da outra placa antecipadamente.
// ============================================================

// -------------------- ESP-NOW --------------------

// Precisa ser o MESMO canal em estacao_b.ino.
// Use para o cenario de interferencia (testar em 1, 6, 11...).
constexpr uint8_t WIFI_CHANNEL = 6;

// Identifica esta dupla de placas. Se houver mais de uma dupla
// testando ao mesmo tempo na mesma sala, mude este valor (ex.: 0x02)
// nas DUAS estacoes dessa dupla para não misturar os pings.
constexpr uint8_t TEAM_ID = 0x01;

constexpr uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// -------------------- Protocolo --------------------

constexpr uint8_t TYPE_DATA = 0x02;

// 1 byte team + 1 byte tipo + 4 bytes sequencia + 4 bytes timestamp
constexpr size_t HEADER_SIZE = 10;

// ESP-NOW classico limita o payload a 250 bytes; ficamos com folga.
constexpr size_t PAYLOAD_SIZES[] = {10, 32, 100, 200};
constexpr size_t NUM_PAYLOAD_SIZES = sizeof(PAYLOAD_SIZES) / sizeof(PAYLOAD_SIZES[0]);
constexpr size_t MAX_PACKET_SIZE = 200;

// -------------------- Parametros do teste --------------------

constexpr uint16_t PING_COUNT = 100;
constexpr unsigned long PING_TIMEOUT_MS = 150;

// ------------------------------------------------------------
// Estrutura de estatisticas - definida antes de qualquer funcao
// (ver nota em desempenho/wifi/estacao_a.ino sobre os prototipos
// automaticos do Arduino).
// ------------------------------------------------------------

struct BenchStats {
  uint16_t sent = 0;
  uint16_t received = 0;
  double sumMs = 0;
  double sumSqMs = 0;
  double minMs = 1e9;
  double maxMs = 0;
};

// -------------------- Buffers compartilhados com o callback --------------------

uint8_t txBuf[MAX_PACKET_SIZE];
uint8_t rxBuf[MAX_PACKET_SIZE];

// O callback do ESP-NOW roda na task de Wi-Fi, fora do loop().
// Ele so copia os dados e sinaliza; todo o processamento (e o
// eventual reenvio, na estacao B) acontece no loop principal.
volatile bool rxFlag = false;
volatile int rxLen = 0;

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
  stats.received++;
  stats.sumMs += rttMs;
  stats.sumSqMs += rttMs * rttMs;

  if (rttMs < stats.minMs) stats.minMs = rttMs;
  if (rttMs > stats.maxMs) stats.maxMs = rttMs;
}

// ------------------------------------------------------------
// Callback de recepcao ESP-NOW
// ------------------------------------------------------------

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;

  if (len < 1 || data[0] != TEAM_ID) {
    return;  // pacote de outra equipe/rede: ignora
  }

  if (len > (int)sizeof(rxBuf)) {
    len = sizeof(rxBuf);
  }

  memcpy((void *)rxBuf, data, len);
  rxLen = len;
  rxFlag = true;
}

// ------------------------------------------------------------
// Executa PING_COUNT round-trips para um tamanho de payload
// ------------------------------------------------------------

BenchStats runBenchmarkForSize(size_t payloadSize) {
  BenchStats stats;

  unsigned long batchStartMicros = micros();

  for (uint16_t seq = 0; seq < PING_COUNT; seq++) {
    stats.sent++;

    txBuf[0] = TEAM_ID;
    txBuf[1] = TYPE_DATA;
    writeU32(txBuf + 2, seq);
    writeU32(txBuf + 6, micros());

    for (size_t i = HEADER_SIZE; i < payloadSize; i++) {
      txBuf[i] = 0xAA;
    }

    uint32_t sendMicros = readU32(txBuf + 6);

    rxFlag = false;
    esp_now_send(BROADCAST_ADDR, txBuf, payloadSize);

    bool gotReply = false;
    unsigned long waitStart = millis();

    while (!gotReply && (millis() - waitStart) < PING_TIMEOUT_MS) {
      yield();  // cede a CPU unica do C6 para as tarefas de Wi-Fi/sistema

      if (rxFlag) {
        rxFlag = false;
        int len = rxLen;

        if (
          len == (int)payloadSize &&
          rxBuf[1] == TYPE_DATA &&
          readU32(rxBuf + 2) == seq
        ) {
          uint32_t rttMicros = micros() - sendMicros;
          recordRtt(stats, rttMicros / 1000.0);
          gotReply = true;
        }
        // Pacote antigo/duplicado: ignora e continua esperando.
      }
    }

    if (!gotReply) {
      Serial.print("TIMEOUT seq=");
      Serial.println(seq);
    }
  }

  unsigned long elapsedMicros = micros() - batchStartMicros;

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

  double elapsedSeconds = elapsedMicros / 1000000.0;
  double throughputKbps =
    elapsedSeconds > 0
      ? (payloadSize * 8.0 * stats.received) / elapsedSeconds / 1000.0
      : 0;

  Serial.print("RESULT,ESPNOW,");
  Serial.print(payloadSize);
  Serial.print(",");
  Serial.print(stats.sent);
  Serial.print(",");
  Serial.print(stats.received);
  Serial.print(",");
  Serial.print(lossPct, 2);
  Serial.print(",");
  Serial.print(stats.received > 0 ? stats.minMs : 0, 3);
  Serial.print(",");
  Serial.print(avgMs, 3);
  Serial.print(",");
  Serial.print(stats.received > 0 ? stats.maxMs : 0, 3);
  Serial.print(",");
  Serial.print(stddevMs, 3);
  Serial.print(",");
  Serial.println(throughputKbps, 2);

  return stats;
}

void runFullBenchmark() {
  Serial.println();
  Serial.println("=== Starting ESP-NOW benchmark ===");
  Serial.println("RESULT,tech,payload_bytes,sent,received,loss_pct,rtt_min_ms,rtt_avg_ms,rtt_max_ms,rtt_stddev_ms,throughput_kbps");

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
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.setChannel(WIFI_CHANNEL);

  while (!WiFi.STA.started()) {
    delay(100);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("Failed to initialize ESP-NOW.");
    delay(1000);
    ESP.restart();
  }

  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, BROADCAST_ADDR, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to register broadcast peer.");
    delay(1000);
    ESP.restart();
  }

  Serial.println();
  Serial.print("Station A ready. MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("Starting benchmark in 3s (make sure Station B is powered on)...");
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
