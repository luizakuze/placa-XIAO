#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

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

// Tamanhos de payload testados, em bytes (inclui o cabecalho acima).
// UDP puro aguenta bem mais, mas 1200 fica seguro abaixo do MTU
// tipico de 1472 bytes e evita fragmentacao.
constexpr size_t PAYLOAD_SIZES[] = {9, 32, 128, 512, 1200};
constexpr size_t NUM_PAYLOAD_SIZES = sizeof(PAYLOAD_SIZES) / sizeof(PAYLOAD_SIZES[0]);
constexpr size_t MAX_PACKET_SIZE = 1200;

// -------------------- Parametros do teste --------------------

constexpr uint16_t PING_COUNT = 100;
constexpr unsigned long PING_TIMEOUT_MS = 200;

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
};

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
  stats.received++;
  stats.sumMs += rttMs;
  stats.sumSqMs += rttMs * rttMs;

  if (rttMs < stats.minMs) stats.minMs = rttMs;
  if (rttMs > stats.maxMs) stats.maxMs = rttMs;
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

  unsigned long batchStartMicros = micros();

  for (uint16_t seq = 0; seq < PING_COUNT; seq++) {
    stats.sent++;

    txBuf[0] = TYPE_DATA;
    writeU32(txBuf + 1, seq);
    writeU32(txBuf + 5, micros());

    for (size_t i = HEADER_SIZE; i < payloadSize; i++) {
      txBuf[i] = 0xAA;
    }

    udp.beginPacket(peerIP, peerPort);
    udp.write(txBuf, payloadSize);
    udp.endPacket();

    uint32_t sendMicros = readU32(txBuf + 5);

    bool gotReply = false;
    unsigned long waitStart = millis();

    while (!gotReply && (millis() - waitStart) < PING_TIMEOUT_MS) {
      yield();  // cede a CPU unica do C6 para as tarefas de Wi-Fi/sistema

      int packetSize = udp.parsePacket();

      if (packetSize >= (int)HEADER_SIZE) {
        int len = udp.read(rxBuf, sizeof(rxBuf));

        if (
          len == (int)payloadSize &&
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

  Serial.print("RESULT,WIFI,");
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
  Serial.println("=== Starting Wi-Fi benchmark ===");
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

  waitForStationB();

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
