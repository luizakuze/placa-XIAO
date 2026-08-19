#include <Arduino.h>

#ifndef ZIGBEE_MODE_ZCZR
#error "Select Zigbee ZCZR in Tools > Zigbee Mode"
#endif

#include "Zigbee.h"

// -------------------- Hardware --------------------

constexpr uint8_t SIGNAL_BUTTON_PIN = D1;
constexpr uint8_t RESET_BUTTON_PIN = D2;

constexpr uint8_t SWITCH_ENDPOINT = 5;
constexpr uint8_t LIGHT_ENDPOINT = 10;

// -------------------- Timing --------------------

constexpr unsigned long DEBOUNCE_MS = 50;
constexpr unsigned long FINISH_HOLD_MS = 5000;
constexpr unsigned long BLINK_INTERVAL_MS = 500;

// -------------------- Zigbee --------------------

ZigbeeSwitch zbSwitch(SWITCH_ENDPOINT);
ZigbeeLight zbLight(LIGHT_ENDPOINT);

uint16_t stationBAddress = 0xFFFF;

// -------------------- Button debounce --------------------

struct DebouncedButton {
  uint8_t pin;
  bool stablePressed;
  bool lastRawPressed;
  unsigned long lastChangeMs;
};

DebouncedButton signalButton = {
  SIGNAL_BUTTON_PIN,
  false,
  false,
  0
};

DebouncedButton resetButton = {
  RESET_BUTTON_PIN,
  false,
  false,
  0
};

// -------------------- Session state --------------------

bool localPressed = false;

volatile bool remotePressed = false;
volatile bool remoteStateChanged = false;

bool sessionFinished = false;

bool finishTimerRunning = false;
unsigned long finishTimerStart = 0;

bool blinkState = false;
unsigned long lastBlinkTime = 0;

// ------------------------------------------------------------
// Button handling
// ------------------------------------------------------------

void initializeButton(DebouncedButton &button) {
  pinMode(button.pin, INPUT_PULLUP);

  bool pressed = digitalRead(button.pin) == LOW;

  button.stablePressed = pressed;
  button.lastRawPressed = pressed;
  button.lastChangeMs = millis();
}

bool updateButton(DebouncedButton &button, bool &newPressed) {
  bool rawPressed = digitalRead(button.pin) == LOW;

  if (rawPressed != button.lastRawPressed) {
    button.lastRawPressed = rawPressed;
    button.lastChangeMs = millis();
  }

  if (
    millis() - button.lastChangeMs >= DEBOUNCE_MS &&
    rawPressed != button.stablePressed
  ) {
    button.stablePressed = rawPressed;
    newPressed = rawPressed;
    return true;
  }

  return false;
}

// ------------------------------------------------------------
// Zigbee receive callback
// ------------------------------------------------------------

void onRemoteSignal(bool state) {
  remotePressed = state;
  remoteStateChanged = true;
}

// ------------------------------------------------------------
// Send a direct Zigbee ON/OFF command
// ------------------------------------------------------------

void sendOnOffCommand(uint16_t destinationAddress, bool pressed) {
  if (destinationAddress == 0xFFFF) {
    return;
  }

  esp_zb_zcl_on_off_cmd_t command = {};

  command.zcl_basic_cmd.src_endpoint = SWITCH_ENDPOINT;
  command.zcl_basic_cmd.dst_endpoint = LIGHT_ENDPOINT;

  command.zcl_basic_cmd.dst_addr_u.addr_short = destinationAddress;

  command.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;

  command.on_off_cmd_id =
    pressed
      ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
      : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_on_off_cmd_req(&command);
  esp_zb_lock_release();

  Serial.print("TX -> ");
  Serial.println(pressed ? "ON" : "OFF");
}

// ------------------------------------------------------------
// Reset only the current application session
// ------------------------------------------------------------

void resetSession() {
  sessionFinished = false;

  finishTimerRunning = false;
  finishTimerStart = 0;

  remotePressed = false;
  remoteStateChanged = false;

  localPressed = signalButton.stablePressed;

  blinkState = false;
  lastBlinkTime = millis();

  digitalWrite(LED_BUILTIN, HIGH);

  Serial.println();
  Serial.println("*** SESSION RESET ***");
  Serial.println();
}

// ------------------------------------------------------------
// Check whether both users have held D1 for five seconds
// ------------------------------------------------------------

void updateFinishDetection() {
  if (sessionFinished) {
    return;
  }

  bool remote = remotePressed;

  if (localPressed && remote) {

    if (!finishTimerRunning) {
      finishTimerRunning = true;
      finishTimerStart = millis();

      Serial.println("Both buttons pressed: 5-second timer started.");
    }

    if (millis() - finishTimerStart >= FINISH_HOLD_MS) {
      sessionFinished = true;
      finishTimerRunning = false;

      blinkState = false;
      lastBlinkTime = millis();

      Serial.println();
      Serial.println("***************************");
      Serial.println("*** SESSION FINISHED!  ***");
      Serial.println("***************************");
      Serial.println();
    }

  } else {

    if (finishTimerRunning) {
      Serial.println("Finish timer cancelled.");
    }

    finishTimerRunning = false;
  }
}

// ------------------------------------------------------------
// Built-in LED control
// ------------------------------------------------------------

void updateLed() {
  // XIAO ESP32C6 built-in LED is active LOW.

  if (sessionFinished) {

    if (millis() - lastBlinkTime >= BLINK_INTERVAL_MS) {
      lastBlinkTime = millis();

      blinkState = !blinkState;

      digitalWrite(
        LED_BUILTIN,
        blinkState ? LOW : HIGH
      );
    }

  } else {

    // During normal operation, reproduce the remote button state.
    digitalWrite(
      LED_BUILTIN,
      remotePressed ? LOW : HIGH
    );
  }
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  initializeButton(signalButton);
  initializeButton(resetButton);

  localPressed = signalButton.stablePressed;

  // Configure TX endpoint.
  zbSwitch.setManufacturerAndModel(
    "IFSC",
    "Telecom-Station-A-TX"
  );

  // Configure RX endpoint.
  zbLight.setManufacturerAndModel(
    "IFSC",
    "Telecom-Station-A-RX"
  );

  zbLight.onLightChange(onRemoteSignal);

  // Add the switch first so the Coordinator discovers
  // Station B's light endpoint when B joins.
  Zigbee.addEndpoint(&zbSwitch);
  Zigbee.addEndpoint(&zbLight);

  // Keep the network open for Station B after reboot.
  Zigbee.setRebootOpenNetwork(180);

  Serial.println();
  Serial.println("Starting Station A as Zigbee Coordinator...");

  if (!Zigbee.begin(ZIGBEE_COORDINATOR)) {
    Serial.println("Failed to start Zigbee Coordinator.");
    delay(1000);
    ESP.restart();
  }

  Serial.println("Waiting for Station B...");

  // Coordinator automatically discovers and binds the switch
  // to Station B's light endpoint when B joins.
  while (!zbSwitch.bound()) {
    Serial.print(".");
    delay(500);
  }

  // Obtain Station B's Zigbee short address.
  while (stationBAddress == 0xFFFF) {

    std::list<zb_device_params_t *> devices =
      zbSwitch.getBoundDevices();

    if (!devices.empty()) {
      stationBAddress = devices.front()->short_addr;
    } else {
      delay(100);
    }
  }

  Serial.println();
  Serial.printf(
    "Station B discovered at address 0x%04X\n",
    stationBAddress
  );

  Serial.println("Bidirectional link ready.");
}

// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------

void loop() {
  bool newState;

  // Communication button.
  if (updateButton(signalButton, newState)) {

    localPressed = newState;

    if (!sessionFinished) {
      sendOnOffCommand(stationBAddress, localPressed);
    }
  }

  // Session reset button.
  if (updateButton(resetButton, newState)) {

    if (newState) {
      resetSession();
    }
  }

  // Print received state outside the Zigbee callback.
  if (remoteStateChanged) {

    remoteStateChanged = false;

    Serial.print("RX <- ");
    Serial.println(remotePressed ? "ON" : "OFF");
  }

  updateFinishDetection();
  updateLed();

  delay(5);
}