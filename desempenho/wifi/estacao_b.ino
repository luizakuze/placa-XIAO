#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// ============================================================
// Estacao B - Wi-Fi (Station) - Respondedora (echo) do teste
//
// B conecta na rede criada pela estacao A e devolve, via UDP,
// exatamente os bytes que recebeu, o mais rapido possivel.
// Toda a logica de medicao fica na estacao A.
// ============================================================

// -------------------- Wi-Fi --------------------

// Precisa ser IDENTICO ao definido em estacao_a.ino.
constexpr char WIFI_SSID[] = "IFSC-Perf-1";
constexpr char WIFI_PASSWORD[] = "telecom123";

constexpr uint16_t UDP_PORT = 5000;

// IP padrao do SoftAP do ESP32 (estacao A).
IPAddress apIP(192, 168, 4, 1);

WiFiUDP udp;

// -------------------- Protocolo --------------------

constexpr uint8_t TYPE_HELLO = 0x01;
constexpr uint8_t TYPE_DATA = 0x02;

constexpr size_t MAX_PACKET_SIZE = 1200;

uint8_t rxBuf[MAX_PACKET_SIZE];

bool everReceivedData = false;
unsigned long lastHelloMs = 0;

// ------------------------------------------------------------
// Anuncia esta estacao para a A ate receber o primeiro pacote
// de dados (cobre o caso do primeiro HELLO se perder).
// ------------------------------------------------------------

void sendHello() {
  uint8_t helloByte = TYPE_HELLO;

  udp.beginPacket(apIP, UDP_PORT);
  udp.write(&helloByte, 1);
  udp.endPacket();

  Serial.println("HELLO -> Station A");
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println();
  Serial.print("Connecting to Station A");

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }

  Serial.println();
  Serial.print("Connected. Local IP: ");
  Serial.println(WiFi.localIP());

  udp.begin(UDP_PORT);
}

// ------------------------------------------------------------
// Main loop: anuncia-se e ecoa qualquer pacote de dados recebido
// ------------------------------------------------------------

void loop() {
  if (!everReceivedData && millis() - lastHelloMs >= 1000) {
    sendHello();
    lastHelloMs = millis();
  }

  int packetSize = udp.parsePacket();

  if (packetSize > 0) {
    int len = udp.read(rxBuf, sizeof(rxBuf));

    if (len > 0 && rxBuf[0] == TYPE_DATA) {
      everReceivedData = true;

      IPAddress remoteIp = udp.remoteIP();
      uint16_t remotePort = udp.remotePort();

      udp.beginPacket(remoteIp, remotePort);
      udp.write(rxBuf, len);
      udp.endPacket();
    }
  }
}
