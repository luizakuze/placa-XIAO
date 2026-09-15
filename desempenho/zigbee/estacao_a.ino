#include <Arduino.h>

#ifndef ZIGBEE_MODE_ZCZR
#error "Select Zigbee ZCZR in Tools > Zigbee Mode"
#endif

#include "Zigbee.h"

// ============================================================
// Estacao A - Zigbee (Coordinator) - Iniciadora do benchmark
//
// Reaproveita a mesma configuracao de endpoints e descoberta do
// estudo ../../com-zigbee/ (endpoints switch/light, binding
// automatico). A diferenca e o que acontece depois do link
// pronto: em vez de refletir um botao, A alterna ON/OFF sozinha
// e mede quanto tempo leva at o eco voltar da estacao B.
//
// Zigbee nao carrega um payload de bytes livre no cluster
// On/Off (e um comando semantico ON/OFF, nao um canal de dados).
// Por isso este teste mede so latencia, com payload fixo de 1 bit
// - ver RESULT abaixo e o metodo no readme.md deste diretorio.
// ============================================================

// -------------------- Zigbee --------------------

constexpr uint8_t SWITCH_ENDPOINT = 5;
constexpr uint8_t LIGHT_ENDPOINT = 10;

ZigbeeSwitch zbSwitch(SWITCH_ENDPOINT);
ZigbeeLight zbLight(LIGHT_ENDPOINT);

uint16_t stationBAddress = 0xFFFF;

// -------------------- Parametros do teste --------------------

constexpr uint16_t PING_COUNT = 100;

// Zigbee soma latencia de APS/NWK/MAC e, no caso da estacao B,
// tambem o tempo de um ciclo de loop() (ver estacao_b.ino) -
// por isso o timeout e o mais folgado dos quatro testes.
constexpr unsigned long PING_TIMEOUT_MS = 1000;

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

// -------------------- Estado do ping-pong --------------------
//
// Mesmo padrao de flag volatile usado no callback Zigbee do
// estudo com-zigbee: o callback so sinaliza, o loop() consome.

volatile bool remoteEchoState = false;
volatile bool remoteEchoReceived = false;

// ------------------------------------------------------------
// Envia um comando ON/OFF direto (identico ao usado em
// ../../com-zigbee/estacao_a.ino) para o endereco informado.
// ------------------------------------------------------------

void sendOnOffCommand(uint16_t destinationAddress, bool state) {
  esp_zb_zcl_on_off_cmd_t command = {};

  command.zcl_basic_cmd.src_endpoint = SWITCH_ENDPOINT;
  command.zcl_basic_cmd.dst_endpoint = LIGHT_ENDPOINT;
  command.zcl_basic_cmd.dst_addr_u.addr_short = destinationAddress;

  command.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;

  command.on_off_cmd_id =
    state
      ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
      : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_on_off_cmd_req(&command);
  esp_zb_lock_release();
}

// ------------------------------------------------------------
// Eco recebido de volta da estacao B
// ------------------------------------------------------------

void onRemoteEcho(bool state) {
  remoteEchoState = state;
  remoteEchoReceived = true;
}

void recordRtt(BenchStats &stats, double rttMs) {
  stats.received++;
  stats.sumMs += rttMs;
  stats.sumSqMs += rttMs * rttMs;

  if (rttMs < stats.minMs) stats.minMs = rttMs;
  if (rttMs > stats.maxMs) stats.maxMs = rttMs;
}

// ------------------------------------------------------------
// Executa PING_COUNT round-trips alternando ON/OFF
// ------------------------------------------------------------

BenchStats runBenchmark() {
  BenchStats stats;
  bool state = false;

  for (uint16_t seq = 0; seq < PING_COUNT; seq++) {
    stats.sent++;
    state = !state;

    remoteEchoReceived = false;
    unsigned long sendMicros = micros();
    sendOnOffCommand(stationBAddress, state);

    bool gotReply = false;
    unsigned long waitStart = millis();

    while (!gotReply && (millis() - waitStart) < PING_TIMEOUT_MS) {
      yield();  // cede a CPU unica do C6 para a stack Zigbee

      if (remoteEchoReceived) {
        remoteEchoReceived = false;

        if (remoteEchoState == state) {
          uint32_t rttMicros = micros() - sendMicros;
          recordRtt(stats, rttMicros / 1000.0);
          gotReply = true;
        }
        // Eco com estado inesperado (ex.: reenvio atrasado): ignora e continua esperando.
      }
    }

    if (!gotReply) {
      Serial.print("TIMEOUT seq=");
      Serial.println(seq);
    }
  }

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

  // payload_bytes=1: um comando ON/OFF (1 bit de estado), nao um
  // canal de bytes livre. throughput fica "NA" de proposito - ver
  // methodology no readme.md deste diretorio.
  Serial.print("RESULT,ZIGBEE,1,");
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
  Serial.println(",NA");

  return stats;
}

// ------------------------------------------------------------
// Setup - descoberta identica a ../../com-zigbee/estacao_a.ino
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  zbSwitch.setManufacturerAndModel("IFSC", "Perf-Station-A-TX");
  zbLight.setManufacturerAndModel("IFSC", "Perf-Station-A-RX");

  zbLight.onLightChange(onRemoteEcho);

  Zigbee.addEndpoint(&zbSwitch);
  Zigbee.addEndpoint(&zbLight);

  Zigbee.setRebootOpenNetwork(180);

  Serial.println();
  Serial.println("Starting Station A as Zigbee Coordinator...");

  if (!Zigbee.begin(ZIGBEE_COORDINATOR)) {
    Serial.println("Failed to start Zigbee Coordinator.");
    delay(1000);
    ESP.restart();
  }

  Serial.println("Waiting for Station B...");

  while (!zbSwitch.bound()) {
    Serial.print(".");
    delay(500);
  }

  while (stationBAddress == 0xFFFF) {
    std::list<zb_device_params_t *> devices = zbSwitch.getBoundDevices();

    if (!devices.empty()) {
      stationBAddress = devices.front()->short_addr;
    } else {
      delay(100);
    }
  }

  Serial.println();
  Serial.printf("Station B discovered at address 0x%04X\n", stationBAddress);
  Serial.println("Link ready. Starting benchmark in 3s...");
  delay(3000);
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {
  Serial.println();
  Serial.println("=== Starting Zigbee benchmark ===");
  Serial.println("RESULT,tech,payload_bytes,sent,received,loss_pct,rtt_min_ms,rtt_avg_ms,rtt_max_ms,rtt_stddev_ms,throughput_kbps");

  runBenchmark();

  Serial.println("=== Benchmark complete ===");
  Serial.println("Send 'r' + Enter in Serial Monitor to run again.");

  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'r' || c == 'R') break;
    }
    delay(50);
  }
}
