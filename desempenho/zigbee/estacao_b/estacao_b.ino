#include <Arduino.h>

#ifndef ZIGBEE_MODE_ED
#error "Select Zigbee ED in Tools > Zigbee Mode"
#endif

#include "Zigbee.h"

// ============================================================
// Estacao B - Zigbee (End Device) - Respondedora (echo)
//
// Devolve, byte a byte, o payload que a estacao A enviou.
//
// Por que existe um cluster proprio aqui
// --------------------------------------
// A versao anterior deste teste usava o cluster On/Off, que carrega
// 1 bit de estado (ligado/desligado) e nao um canal de dados. Com
// ele, o Zigbee nao conseguia participar do payload de referencia
// de 16 bytes usado pelas outras tres tecnologias, e a coluna de
// throughput saia como "NA" - a comparacao ficava incompleta
// exatamente na metrica que o estudo precisa medir.
//
// O ping agora trafega num cluster ZCL especifico de fabricante
// (PING_CLUSTER_ID), que aceita um payload de bytes livre. Os
// endpoints switch/light continuam existindo sem nenhuma mudanca:
// sao eles que fazem a associacao e o binding com a estacao A, que
// ja estao validados. O cluster de ping so pega carona no endpoint
// light.
//
// Toda a medicao de tempo fica em estacao_a.ino - aqui so existe o
// eco.
// ============================================================

constexpr uint8_t SWITCH_ENDPOINT = 5;
constexpr uint8_t LIGHT_ENDPOINT = 10;

// -------------------- Cluster de ping --------------------

// A faixa 0xFC00-0xFFFF e reservada pelo ZCL a clusters especificos
// de fabricante, entao nao colide com nenhum cluster padrao.
constexpr uint16_t PING_CLUSTER_ID = 0xFC00;
constexpr uint16_t PING_ATTR_ID = 0x0000;

constexpr uint8_t PING_CMD_ID = 0x01;  // A -> B
constexpr uint8_t ECHO_CMD_ID = 0x02;  // B -> A

// A lista de atributos guarda o ponteiro para o valor inicial, entao
// ele precisa sobreviver ao construtor - nao pode ser uma local.
uint8_t pingAttrValue = 0;

// Quadro 802.15.4 tem 127 bytes; depois dos cabecalhos NWK/APS/ZCL e
// da seguranca sobram algumas dezenas. 64 e folga suficiente para os
// tamanhos testados (ver PAYLOAD_SIZES em estacao_a.ino).
constexpr size_t MAX_PACKET_SIZE = 8 + 32;

// ------------------------------------------------------------
// Endpoint light + cluster de ping
//
// Derivamos de ZigbeeLight, em vez de montar um endpoint do zero,
// porque _cluster_list e protected em ZigbeeEP: so uma classe
// derivada consegue acrescentar um cluster a lista que o ZigbeeLight
// ja montou. Assim a descoberta e o binding continuam sendo o mesmo
// codigo que ja funciona.
// ------------------------------------------------------------

class ZigbeePingLight : public ZigbeeLight {
public:
  ZigbeePingLight(uint8_t endpoint) : ZigbeeLight(endpoint) {
    esp_zb_attribute_list_t *attrs = esp_zb_zcl_attr_list_create(PING_CLUSTER_ID);

    esp_zb_custom_cluster_add_custom_attr(
      attrs, PING_ATTR_ID, ESP_ZB_ZCL_ATTR_TYPE_U8,
      ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &pingAttrValue
    );

    esp_zb_cluster_list_add_custom_cluster(
      _cluster_list, attrs, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
    );
  }
};

ZigbeeSwitch zbSwitch(SWITCH_ENDPOINT);
ZigbeePingLight zbLight(LIGHT_ENDPOINT);

// -------------------- Estado compartilhado com o callback --------------------
//
// O callback roda a partir da stack Zigbee. Para nao chamar
// esp_zb_lock_acquire() de dentro dele (risco de reentrancia com o
// lock que a propria stack pode estar segurando), so copiamos e
// sinalizamos aqui; o envio acontece no loop(), no contexto normal
// do sketch - mesmo padrao usado em desempenho/espnow e
// desempenho/ble deste repositorio.

volatile bool echoPending = false;
volatile size_t echoLen = 0;
volatile uint16_t echoDstAddr = 0x0000;
uint8_t echoBuf[MAX_PACKET_SIZE];

// ------------------------------------------------------------
// Extrai o payload da mensagem recebida
//
// O dado vai no ar como octet string, cujo primeiro byte e o
// tamanho. Dependendo da versao da stack, o callback pode entregar
// o buffer ja sem esse prefixo - entao aceitamos as duas formas em
// vez de depender de um detalhe de serializacao.
// ------------------------------------------------------------

size_t extractPayload(
  const esp_zb_zcl_custom_cluster_command_message_t *message,
  uint8_t *dst, size_t dstSize
) {
  const uint8_t *src = (const uint8_t *)message->data.value;
  size_t size = message->data.size;

  if (src == nullptr || size == 0) {
    return 0;
  }

  if (size > 1 && src[0] == size - 1) {
    src += 1;  // prefixo de tamanho presente
    size -= 1;
  }

  if (size > dstSize) {
    size = dstSize;
  }

  memcpy(dst, src, size);
  return size;
}

void onPingCommand(const esp_zb_zcl_custom_cluster_command_message_t *message) {
  if (message == nullptr || message->info.command.id != PING_CMD_ID) {
    return;
  }

  size_t len = extractPayload(message, echoBuf, sizeof(echoBuf));

  if (len == 0) {
    return;
  }

  echoLen = len;
  echoDstAddr = message->info.src_address.u.short_addr;
  echoPending = true;
}

// ------------------------------------------------------------
// Devolve o payload recebido, sem alterar um byte
// ------------------------------------------------------------

void sendEcho(uint16_t dstAddr, const uint8_t *data, size_t len) {
  static uint8_t frame[1 + MAX_PACKET_SIZE];

  frame[0] = (uint8_t)len;  // octet string: tamanho no primeiro byte
  memcpy(frame + 1, data, len);

  esp_zb_zcl_custom_cluster_cmd_t command = {};

  command.zcl_basic_cmd.src_endpoint = LIGHT_ENDPOINT;
  command.zcl_basic_cmd.dst_endpoint = LIGHT_ENDPOINT;
  command.zcl_basic_cmd.dst_addr_u.addr_short = dstAddr;

  command.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  command.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  command.cluster_id = PING_CLUSTER_ID;
  command.custom_cmd_id = ECHO_CMD_ID;
  command.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;

  // Sem default response: o que a estacao A cronometra e este eco, e
  // uma confirmacao extra da stack so somaria trafego ao enlace.
  command.dis_default_resp = 1;

  command.data.type = ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING;
  command.data.size = len + 1;
  command.data.value = frame;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_custom_cluster_cmd_req(&command);
  esp_zb_lock_release();
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  zbSwitch.setManufacturerAndModel("IFSC", "Perf-Station-B-TX");
  zbLight.setManufacturerAndModel("IFSC", "Perf-Station-B-RX");

  zbLight.onCustomClusterCommand(onPingCommand);

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
// Main loop: ecoa o ultimo payload recebido
// ------------------------------------------------------------

void loop() {
  if (echoPending) {
    echoPending = false;
    sendEcho(echoDstAddr, echoBuf, echoLen);
  }

  // 1 ms, igual a estacao B do BLE. Este delay define quanto tempo o
  // eco pode ficar esperando antes de sair, entao entra direto no RTT
  // medido - por isso e o menor valor que ainda deixa a CPU unica do
  // C6 para a pilha de radio.
  delay(1);
}
