#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

// ============================================================
// Estacao B - ESP-NOW - Respondedora (echo) do teste
//
// B recebe cada quadro ESP-NOW e devolve exatamente os mesmos
// bytes via broadcast, o mais rapido possivel. Nao precisa saber
// o MAC da estacao A com antecedencia.
// ============================================================

// -------------------- ESP-NOW --------------------

// Precisa ser o MESMO canal e TEAM_ID definidos em estacao_a.ino.
constexpr uint8_t WIFI_CHANNEL = 6;
constexpr uint8_t TEAM_ID = 0x01;

constexpr uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// -------------------- Protocolo --------------------

constexpr uint8_t TYPE_DATA = 0x02;
constexpr size_t HEADER_SIZE = 10;
constexpr size_t MAX_PACKET_SIZE = 200;

// -------------------- Buffers compartilhados com o callback --------------------

uint8_t rxBuf[MAX_PACKET_SIZE];

volatile bool rxFlag = false;
volatile int rxLen = 0;

bool everReceivedData = false;

// ------------------------------------------------------------
// Callback de recepcao ESP-NOW (roda na task de Wi-Fi).
// So copia e sinaliza - o envio do eco acontece no loop(),
// fora do contexto do callback.
// ------------------------------------------------------------

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;

  if (len < 1 || data[0] != TEAM_ID) {
    return;
  }

  if (len > (int)sizeof(rxBuf)) {
    len = sizeof(rxBuf);
  }

  memcpy((void *)rxBuf, data, len);
  rxLen = len;
  rxFlag = true;
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
  Serial.print("Station B ready. MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("Waiting for Station A...");
}

// ------------------------------------------------------------
// Main loop: ecoa qualquer pacote de dados recebido
// ------------------------------------------------------------

void loop() {
  if (rxFlag) {
    rxFlag = false;
    int len = rxLen;

    if (len >= (int)HEADER_SIZE && rxBuf[1] == TYPE_DATA) {
      if (!everReceivedData) {
        everReceivedData = true;
        Serial.println("Station A found. Echoing pings...");
      }

      esp_now_send(BROADCAST_ADDR, rxBuf, len);
    }
  }
}
