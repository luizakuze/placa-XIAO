#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ============================================================
// Estacao B - BLE (Peripheral/Server) - Respondedora (echo)
//
// B anuncia um servico GATT com duas characteristics (padrao
// "UART" da Nordic, muito usado como referencia):
//   RX (WRITE_NR): A escreve o ping aqui.
//   TX (NOTIFY):   B devolve o eco aqui.
// B nunca inicia a conexao - so aceita e ecoa.
// ============================================================

// -------------------- BLE --------------------

// Nome anunciado. A filtra o scan por este nome. Se houver mais
// de uma dupla testando na mesma sala, mude o numero final aqui
// e em estacao_a.ino (ex.: "XIAO-PERF-B-2").
constexpr char DEVICE_NAME[] = "XIAO-PERF-B-1";

constexpr char SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char CHAR_UUID_RX[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char CHAR_UUID_TX[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// MTU maximo pratico em links ESP32-ESP32 (o padrao BLE permite
// ate 517, mas o controlador do ESP32 usa 247 como teto usual).
constexpr uint16_t PREFERRED_MTU = 247;

constexpr size_t MAX_PACKET_SIZE = 8 + 200;

// -------------------- Connection interval --------------------

// Faixa de connection interval que B anuncia como preferida, em
// unidades de 1.25 ms. O default da lib e 0x20-0x40 (50-80 ms), e
// era ele que fazia o RTT do teste travar em ~100 ms para qualquer
// payload: o ping-pong gasta dois connection events, entao o RTT
// nunca desce abaixo de 2x o intervalo. Ver a nota longa em
// estacao_a.ino.
//
// A estacao A tambem pede 7.5 ms explicitamente depois de conectar;
// isto aqui serve para o enlace ja NASCER curto, em vez de nascer
// em 50 ms e so depois ser corrigido.
constexpr uint16_t PREFERRED_CONN_INTERVAL_MIN = 0x06;  // 6 x 1.25 ms = 7.5 ms
constexpr uint16_t PREFERRED_CONN_INTERVAL_MAX = 0x0C;  // 12 x 1.25 ms = 15 ms

// -------------------- Estado compartilhado com os callbacks --------------------
//
// As callbacks da pilha BLE rodam fora do loop() principal. Por
// isso so copiamos os dados e sinalizamos aqui; o notify() de
// verdade acontece dentro de loop(), como ja e feito nos outros
// testes deste diretorio (wifi/espnow) e no estudo com-zigbee.

BLECharacteristic *pTxCharacteristic = nullptr;

volatile bool deviceConnected = false;
bool wasConnected = false;

volatile bool echoPending = false;
volatile size_t echoLen = 0;
uint8_t echoBuf[MAX_PACKET_SIZE];

// ------------------------------------------------------------
// Callbacks de conexao
// ------------------------------------------------------------

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    (void)server;
    deviceConnected = true;
    Serial.println("Station A connected.");
  }

  void onDisconnect(BLEServer *server) override {
    (void)server;
    deviceConnected = false;
    Serial.println("Station A disconnected.");
  }

#if defined(CONFIG_NIMBLE_ENABLED)
  // O controlador pode conceder um intervalo maior que o pedido, e
  // nesse caso o RTT medido sobe junto sem que nada no sketch avise.
  // As duas linhas CONNPARAMS abaixo tornam isso visivel no log:
  // "inicial" e o intervalo com que a conexao nasceu, "negociado" e
  // o que passou a valer depois do updateConnParams() de A. E o
  // segundo numero que descreve o enlace medido - ele deve ir para a
  // coluna observacoes do CSV junto com as linhas BLE.

  void onConnect(BLEServer *server, ble_gap_conn_desc *desc) override {
    (void)server;
    Serial.printf(
      "CONNPARAMS,BLE,inicial,%u,%.2f\n",
      desc->conn_itvl, desc->conn_itvl * 1.25
    );
  }

  void onConnParamsUpdate(
    uint16_t conn_handle, uint16_t interval,
    uint16_t latency, uint16_t timeout, uint8_t status
  ) override {
    (void)conn_handle;
    (void)latency;
    (void)timeout;
    Serial.printf(
      "CONNPARAMS,BLE,negociado,%u,%.2f,status=%u\n",
      interval, interval * 1.25, status
    );
  }
#endif
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    uint8_t *data = characteristic->getData();
    size_t len = characteristic->getLength();

    if (len > 0 && len <= sizeof(echoBuf)) {
      memcpy((void *)echoBuf, data, len);
      echoLen = len;
      echoPending = true;
    }
  }
};

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  BLEDevice::setMTU(PREFERRED_MTU);
  BLEDevice::init(DEVICE_NAME);

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
    CHAR_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
    CHAR_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setMinPreferred(PREFERRED_CONN_INTERVAL_MIN);
  pAdvertising->setMaxPreferred(PREFERRED_CONN_INTERVAL_MAX);
  pAdvertising->start();

  Serial.println();
  Serial.print("Station B ready, advertising as \"");
  Serial.print(DEVICE_NAME);
  Serial.println("\".");
  Serial.println("Waiting for Station A...");
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {
  if (echoPending) {
    echoPending = false;

    pTxCharacteristic->setValue((uint8_t *)echoBuf, echoLen);
    pTxCharacteristic->notify();
  }

  if (deviceConnected && !wasConnected) {
    wasConnected = true;
  }

  if (!deviceConnected && wasConnected) {
    wasConnected = false;

    delay(500);  // da tempo da pilha BLE ficar pronta de novo
    BLEDevice::startAdvertising();
    Serial.println("Restarted advertising.");
  }

  // 1 ms, igual a estacao B do Zigbee. Este delay define quanto tempo o
  // eco pode ficar esperando antes de sair, entao ele entra direto no RTT
  // medido - com 5 ms estariamos creditando ao BLE uma latencia que e do
  // sketch. As estacoes B de Wi-Fi e ESP-NOW nao tem delay nenhum; aqui e
  // no Zigbee 1 ms e mantido para nao monopolizar a CPU unica do C6, que
  // a pilha de radio tambem precisa usar.
  //
  // ATENCAO: com o intervalo de 50 ms este 1 ms era ruido (2%). Com 7.5 ms
  // ele passa a valer ~13% de um connection event, e um eco que perca o
  // evento por causa dele custa um intervalo inteiro. Se o RTT aparecer
  // bimodal (dois picos separados por ~7.5 ms), a causa e aqui - e a saida
  // e ecoar dentro do proprio callback de write, nao neste polling.
  delay(1);
}
