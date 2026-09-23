#include <Arduino.h>
#include <algorithm>

#ifndef ZIGBEE_MODE_ZCZR
#error "Select Zigbee ZCZR in Tools > Zigbee Mode"
#endif

#include "Zigbee.h"

// ============================================================
// Estacao A - Zigbee (Coordinator) - Iniciadora do benchmark
//
// Reaproveita a configuracao de endpoints e a descoberta do estudo
// ../../com-zigbee/ (endpoints switch/light, binding automatico).
// Depois que o link fica pronto, A envia um payload de bytes e mede
// quanto tempo leva ate o eco voltar da estacao B.
//
// Por que existe um cluster proprio aqui
// --------------------------------------
// A versao anterior deste teste usava o cluster On/Off, que carrega
// 1 bit de estado e nao um canal de dados. Com ele o Zigbee nao
// conseguia participar do payload de referencia de 16 bytes das
// outras tres tecnologias, e o throughput saia como "NA" - a
// comparacao ficava incompleta justamente na metrica central do
// estudo. O ping agora trafega num cluster ZCL especifico de
// fabricante, que aceita payload de bytes livre.
//
// Os endpoints switch/light continuam iguais: sao eles que fazem a
// associacao e o binding, que ja estao validados em hardware.
// ============================================================

// -------------------- Zigbee --------------------

constexpr uint8_t SWITCH_ENDPOINT = 5;
constexpr uint8_t LIGHT_ENDPOINT = 10;

uint16_t stationBAddress = 0xFFFF;

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

// -------------------- Protocolo --------------------

// 4 bytes sequencia + 4 bytes timestamp (micros). Sem byte de
// "equipe" nem de tipo: o binding Zigbee ja e um enlace exclusivo
// entre A e B, e o custom_cmd_id ja separa ping de eco.
constexpr size_t HEADER_SIZE = 8;

// Tamanhos de payload de aplicacao testados, em bytes (nao incluem o cabecalho acima).
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
constexpr size_t PAYLOAD_SIZES[] = {16, 32};
constexpr size_t NUM_PAYLOAD_SIZES = sizeof(PAYLOAD_SIZES) / sizeof(PAYLOAD_SIZES[0]);
constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + 32;

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
// conta do RTT. A cadencia e a mesma das outras tres tecnologias,
// para que as colunas rssi_* signifiquem a mesma coisa nas quatro.
constexpr uint16_t RSSI_SAMPLE_EVERY = 10;

// RSSI real e sempre negativo (dBm), entao 127 serve de "nao foi
// possivel ler" sem precisar de uma flag separada.
constexpr int RSSI_INVALID = 127;

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
// tempo o Zigbee leva desde o boot até o binding com a estacao B
// ficar pronto (ver LINKSETUP no setup()).
unsigned long linkSetupStartMs = 0;

// ------------------------------------------------------------
// Custo de memoria do protocolo
//
// Heap livre em tres momentos do boot, para separar o que cada
// camada cobra de RAM:
//   heapBeforeRadio - depois do Serial.begin(), antes de qualquer
//                     chamada da pilha Zigbee;
//   heapAfterRadio  - depois de Zigbee.begin(ZIGBEE_COORDINATOR)
//                     formar a rede;
//   heapAfterLink   - depois de a estacao B estar associada, vinculada
//                     (bound) e com endereco curto conhecido.
// A diferenca entre o primeiro e o segundo e o custo fixo do
// protocolo; entre o segundo e o terceiro, o custo de ter um
// dispositivo na rede (ver MEMORY no setup()).
// ------------------------------------------------------------

uint32_t heapBeforeRadio = 0;
uint32_t heapAfterRadio = 0;
uint32_t heapAfterLink = 0;

void printMemoryReport() {
  Serial.printf(
    "MEMORY,ZIGBEE,%lu,%lu,%lu,%ld,%ld,%lu\n",
    (unsigned long)heapBeforeRadio,
    (unsigned long)heapAfterRadio,
    (unsigned long)heapAfterLink,
    (long)heapBeforeRadio - (long)heapAfterRadio,
    (long)heapAfterRadio - (long)heapAfterLink,
    (unsigned long)ESP.getMinFreeHeap()
  );
}

// -------------------- Buffers e estado do ping-pong --------------------
//
// O callback roda a partir da stack Zigbee: so copia e sinaliza, e
// todo o processamento acontece no loop principal - mesmo padrao dos
// callbacks de ESP-NOW e BLE deste repositorio.

uint8_t txBuf[MAX_PACKET_SIZE];
uint8_t rxBuf[MAX_PACKET_SIZE];

volatile bool rxFlag = false;
volatile size_t rxLen = 0;

// ------------------------------------------------------------
// Empacotamento de inteiros (mesma arquitetura nos dois lados,
// entao memcpy direto e suficiente).
// ------------------------------------------------------------

void writeU32(uint8_t *buf, uint32_t value) {
  memcpy(buf, &value, sizeof(value));
}

uint32_t readU32(const uint8_t *buf) {
  uint32_t value;
  memcpy(&value, buf, sizeof(value));
  return value;
}

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

void onPingEcho(const esp_zb_zcl_custom_cluster_command_message_t *message) {
  if (message == nullptr || message->info.command.id != ECHO_CMD_ID) {
    return;
  }

  size_t len = extractPayload(message, rxBuf, sizeof(rxBuf));

  if (len == 0) {
    return;
  }

  rxLen = len;
  rxFlag = true;
}

// ------------------------------------------------------------
// Envia um ping com payload de bytes para a estacao B
// ------------------------------------------------------------

void sendPing(uint16_t destinationAddress, const uint8_t *data, size_t len) {
  static uint8_t frame[1 + MAX_PACKET_SIZE];

  frame[0] = (uint8_t)len;  // octet string: tamanho no primeiro byte
  memcpy(frame + 1, data, len);

  esp_zb_zcl_custom_cluster_cmd_t command = {};

  command.zcl_basic_cmd.src_endpoint = LIGHT_ENDPOINT;
  command.zcl_basic_cmd.dst_endpoint = LIGHT_ENDPOINT;
  command.zcl_basic_cmd.dst_addr_u.addr_short = destinationAddress;

  command.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  command.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  command.cluster_id = PING_CLUSTER_ID;
  command.custom_cmd_id = PING_CMD_ID;
  command.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;

  // Sem default response: o que cronometramos e o eco da estacao B, e
  // uma confirmacao extra da stack so somaria trafego ao enlace.
  command.dis_default_resp = 1;

  command.data.type = ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING;
  command.data.size = len + 1;
  command.data.value = frame;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_custom_cluster_cmd_req(&command);
  esp_zb_lock_release();
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
// Zigbee nao tem um "WiFi.RSSI()" pronto: o valor fica na tabela de
// vizinhos da camada de rede, que o coordenador mantem para cada
// dispositivo associado. Percorremos a tabela ate achar o endereco
// curto de B. A mesma entrada carrega o LQI (neighbor.lqi, 0-255),
// que e a metrica de qualidade mais usada em 802.15.4 - se voce
// quiser registra-lo tambem, ele esta a um campo de distancia daqui.
// ------------------------------------------------------------

int readRssiDbm() {
  esp_zb_nwk_info_iterator_t iterator = ESP_ZB_NWK_INFO_ITERATOR_INIT;
  esp_zb_nwk_neighbor_info_t neighbor;
  int rssi = RSSI_INVALID;

  esp_zb_lock_acquire(portMAX_DELAY);

  while (esp_zb_nwk_get_next_neighbor(&iterator, &neighbor) == ESP_OK) {
    if (neighbor.short_addr == stationBAddress) {
      rssi = neighbor.rssi;
      break;
    }
  }

  esp_zb_lock_release();

  return rssi;
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
// leitura deu certo (ex.: a estacao B saiu da tabela de vizinhos).
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
    sendPing(stationBAddress, txBuf, packetSize);

    bool gotReply = false;
    unsigned long waitStart = millis();
    unsigned long waitStartMicros = micros();

    while (!gotReply && (millis() - waitStart) < PING_TIMEOUT_MS) {
      yield();  // cede a CPU unica do C6 para a stack Zigbee

      if (rxFlag) {
        rxFlag = false;
        size_t len = rxLen;

        if (len == packetSize && readU32(rxBuf) == seq) {
          uint32_t rttMicros = micros() - sendMicros;
          recordRtt(stats, rttMicros / 1000.0);
          gotReply = true;
        }
        // Eco antigo/duplicado: ignora e continua esperando.
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

  Serial.print("RESULT,ZIGBEE,");
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
  Serial.println("=== Starting Zigbee benchmark ===");
  Serial.println("RESULT,tech,payload_bytes,sent,received,loss_pct,pdr_pct,rtt_min_ms,rtt_avg_ms,rtt_max_ms,rtt_stddev_ms,throughput_kbps,rtt_p50_ms,rtt_p90_ms,rtt_p95_ms,rtt_p99_ms,rssi_avg_dbm,rssi_min_dbm,rssi_amostras");

  for (size_t i = 0; i < NUM_PAYLOAD_SIZES; i++) {
    runBenchmarkForSize(PAYLOAD_SIZES[i]);
    delay(200);
  }

  Serial.println("=== Benchmark complete ===");
  Serial.println("Send 'r' + Enter in Serial Monitor to run again.");
}

// ------------------------------------------------------------
// Setup - descoberta identica a ../../com-zigbee/estacao_a.ino
// ------------------------------------------------------------

void setup() {
  linkSetupStartMs = millis();

  Serial.begin(115200);

  // Antes de qualquer chamada da pilha de radio (ver printMemoryReport).
  heapBeforeRadio = ESP.getFreeHeap();

  zbSwitch.setManufacturerAndModel("IFSC", "Perf-Station-A-TX");
  zbLight.setManufacturerAndModel("IFSC", "Perf-Station-A-RX");

  zbLight.onCustomClusterCommand(onPingEcho);

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

  heapAfterRadio = ESP.getFreeHeap();

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

  heapAfterLink = ESP.getFreeHeap();

  Serial.println();
  Serial.printf("Station B discovered at address 0x%04X\n", stationBAddress);
  Serial.printf("LINKSETUP,ZIGBEE,%lu\n", millis() - linkSetupStartMs);
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
