#include <Arduino.h>

#ifndef ZIGBEE_MODE_ED
#error "Select Zigbee ED in Tools > Zigbee Mode"
#endif

#include "Zigbee.h"

// ============================================================
// Estacao B - Zigbee (End Device) - Respondedora (echo)
//
// Reaproveita a configuracao de endpoints de ../../com-zigbee/.
// Ao receber uma mudanca de estado de A, devolve o MESMO estado
// assim que possivel. Toda a medicao de tempo fica em
// estacao_a.ino - aqui so existe o eco.
// ============================================================

constexpr uint8_t SWITCH_ENDPOINT = 5;
constexpr uint8_t LIGHT_ENDPOINT = 10;

// Coordenador Zigbee sempre usa o endereco curto 0x0000.
constexpr uint16_t COORDINATOR_ADDRESS = 0x0000;

ZigbeeSwitch zbSwitch(SWITCH_ENDPOINT);
ZigbeeLight zbLight(LIGHT_ENDPOINT);

// -------------------- Estado compartilhado com o callback --------------------
//
// O callback onLightChange roda a partir da stack Zigbee. Para
// nao chamar esp_zb_lock_acquire() de dentro dele (risco de
// reentrancia/deadlock com o lock que a propria stack pode estar
// segurando), so sinalizamos aqui; o envio de verdade acontece no
// loop(), no contexto normal do sketch - mesmo padrao usado em
// desempenho/espnow e desempenho/ble deste repositorio.

volatile bool echoPending = false;
volatile bool echoState = false;

// ------------------------------------------------------------
// Envia um comando ON/OFF direto (identico ao usado em
// ../../com-zigbee/estacao_b.ino) de volta ao coordenador.
// ------------------------------------------------------------

void sendOnOffCommand(bool state) {
  esp_zb_zcl_on_off_cmd_t command = {};

  command.zcl_basic_cmd.src_endpoint = SWITCH_ENDPOINT;
  command.zcl_basic_cmd.dst_endpoint = LIGHT_ENDPOINT;
  command.zcl_basic_cmd.dst_addr_u.addr_short = COORDINATOR_ADDRESS;

  command.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;

  command.on_off_cmd_id =
    state
      ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
      : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_on_off_cmd_req(&command);
  esp_zb_lock_release();
}

void onRemoteSignal(bool state) {
  echoState = state;
  echoPending = true;
}

// ------------------------------------------------------------
// Setup - identico a ../../com-zigbee/estacao_b.ino
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  zbSwitch.setManufacturerAndModel("IFSC", "Perf-Station-B-TX");
  zbLight.setManufacturerAndModel("IFSC", "Perf-Station-B-RX");

  zbLight.onLightChange(onRemoteSignal);

  Zigbee.addEndpoint(&zbSwitch);
  Zigbee.addEndpoint(&zbLight);

  Serial.println();
  Serial.println("Starting Station B as Zigbee End Device...");

  if (!Zigbee.begin()) {
    Serial.println("Failed to start Zigbee End Device.");
    delay(1000);
    ESP.restart();
  }

  Serial.println("Looking for Station A network...");

  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("Connected to Station A. Echoing pings...");
}

// ------------------------------------------------------------
// Main loop: ecoa o ultimo estado recebido
// ------------------------------------------------------------

void loop() {
  if (echoPending) {
    echoPending = false;
    sendOnOffCommand(echoState);
  }

  delay(1);
}
