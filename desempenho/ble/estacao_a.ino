#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

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

// -------------------- Protocolo --------------------

// 4 bytes sequencia + 4 bytes timestamp (micros). Sem byte de
// "equipe" aqui: a conexao BLE ja e um enlace ponto a ponto
// exclusivo entre A e B depois do connect().
constexpr size_t HEADER_SIZE = 8;

// Payloads pequenos cabem no MTU padrao (23); os maiores dependem
// da negociacao de MTU feita em connectToStationB().
constexpr size_t PAYLOAD_SIZES[] = {8, 20, 100, 200};
constexpr size_t NUM_PAYLOAD_SIZES = sizeof(PAYLOAD_SIZES) / sizeof(PAYLOAD_SIZES[0]);
constexpr size_t MAX_PACKET_SIZE = 200;

// -------------------- Parametros do teste --------------------

constexpr uint16_t PING_COUNT = 100;

// BLE tem latencia de enlace maior que Wi-Fi/ESP-NOW (depende do
// connection interval negociado), por isso o timeout e mais folgado.
constexpr unsigned long PING_TIMEOUT_MS = 500;

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
};

// -------------------- Estado global --------------------

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
  stats.received++;
  stats.sumMs += rttMs;
  stats.sumSqMs += rttMs * rttMs;

  if (rttMs < stats.minMs) stats.minMs = rttMs;
  if (rttMs > stats.maxMs) stats.maxMs = rttMs;
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

  BLEClient *pClient = BLEDevice::createClient();
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

  Serial.print("Connected. Negotiated MTU: ");
  Serial.println(pClient->getMTU());

  return true;
}

// ------------------------------------------------------------
// Executa PING_COUNT round-trips para um tamanho de payload
// ------------------------------------------------------------

BenchStats runBenchmarkForSize(size_t payloadSize) {
  BenchStats stats;

  unsigned long batchStartMicros = micros();

  for (uint16_t seq = 0; seq < PING_COUNT; seq++) {
    stats.sent++;

    writeU32(txBuf, seq);
    writeU32(txBuf + 4, micros());

    for (size_t i = HEADER_SIZE; i < payloadSize; i++) {
      txBuf[i] = 0xAA;
    }

    uint32_t sendMicros = readU32(txBuf + 4);

    rxFlag = false;
    pRxCharacteristic->writeValue(txBuf, payloadSize, false);

    bool gotReply = false;
    unsigned long waitStart = millis();

    while (!gotReply && (millis() - waitStart) < PING_TIMEOUT_MS) {
      yield();  // cede a CPU unica do C6 para a pilha BLE

      if (rxFlag) {
        rxFlag = false;
        size_t len = rxLen;

        if (len == payloadSize && readU32(rxBuf) == seq) {
          uint32_t rttMicros = micros() - sendMicros;
          recordRtt(stats, rttMicros / 1000.0);
          gotReply = true;
        }
        // Notificacao antiga/duplicada: ignora e continua esperando.
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

  Serial.print("RESULT,BLE,");
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
  Serial.println("=== Starting BLE benchmark ===");
  Serial.println("RESULT,tech,payload_bytes,sent,received,loss_pct,rtt_min_ms,rtt_avg_ms,rtt_max_ms,rtt_stddev_ms,throughput_kbps");

  uint16_t negotiatedMtu = 23;  // valor padrao caso a negociacao falhe

  for (size_t i = 0; i < NUM_PAYLOAD_SIZES; i++) {
    if (PAYLOAD_SIZES[i] + 3 > PREFERRED_MTU) {
      Serial.print("Skipping payload ");
      Serial.print(PAYLOAD_SIZES[i]);
      Serial.println(" bytes: acima do MTU negociado.");
      continue;
    }

    runBenchmarkForSize(PAYLOAD_SIZES[i]);
    delay(200);
  }

  (void)negotiatedMtu;

  Serial.println("=== Benchmark complete ===");
  Serial.println("Send 'r' + Enter in Serial Monitor to run again.");
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  BLEDevice::setMTU(PREFERRED_MTU);
  BLEDevice::init("");

  BLEScan *pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->setActiveScan(true);

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
