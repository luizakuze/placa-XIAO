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

constexpr size_t MAX_PACKET_SIZE = 200;

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

  delay(5);
}
