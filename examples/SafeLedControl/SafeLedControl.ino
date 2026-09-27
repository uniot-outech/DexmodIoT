#include <DexmodIoT.h>

#include "arduino_secrets.h"

// Change this for boards whose built-in LED is not exposed as LED_BUILTIN.
#ifndef LED_BUILTIN
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define LED_BUILTIN 8
#else
#define LED_BUILTIN 2
#endif
#endif

// Set to 1 when the board LED turns on at LOW, as on many ESP32-C3 Super Mini boards.
#define DEXMOD_LED_ACTIVE_LOW 0

constexpr uint32_t kTelemetryIntervalMs = 60000;

DexmodIoT dexmod;
bool wasConnected = false;
bool lampEnabled = false;
uint32_t lastTelemetryAt = 0;
String previousStatus;

void setLamp(bool enabled) {
  lampEnabled = enabled;
  const uint8_t level = enabled
    ? (DEXMOD_LED_ACTIVE_LOW ? LOW : HIGH)
    : (DEXMOD_LED_ACTIVE_LOW ? HIGH : LOW);
  digitalWrite(LED_BUILTIN, level);
}

void handleCommand(DexmodIoT& client, const DexmodCommand& command) {
  if (strcmp(command.control(), "lamp") != 0) {
    client.fail(command, "UNKNOWN_CONTROL", "Only the lamp control is supported");
    return;
  }
  if (command.valueType() != DexmodCommandValueType::Boolean) {
    client.fail(command, "INVALID_VALUE", "The lamp control requires a boolean");
    return;
  }

  const bool enabled = command.booleanValue();
  setLamp(enabled);
  if (!client.acknowledge(command, enabled)) {
    // Fail safe: do not leave the output on if its state cannot be confirmed.
    setLamp(false);
  }
}

void printConnectionStatus() {
  const String currentStatus = dexmod.connected()
    ? "Connected to Dexmod"
    : String(dexmod.lastError());
  if (currentStatus == previousStatus) return;
  Serial.print("[Dexmod] ");
  Serial.println(currentStatus);
  previousStatus = currentStatus;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  setLamp(false);

  dexmod.onCommand(handleCommand);
  dexmod.begin(
    WIFI_SSID,
    WIFI_PASSWORD,
    DEXMOD_DEVICE_ID,
    DEXMOD_DEVICE_TOKEN,
    60
  );
}

void loop() {
  // Check both before and after loop(): begin/reconnect may complete in one call.
  if (wasConnected && !dexmod.connected()) setLamp(false);
  dexmod.loop();
  printConnectionStatus();

  const bool isConnected = dexmod.connected();
  if (wasConnected && !isConnected) {
    // A disconnected device cannot receive a later OFF command.
    setLamp(false);
  }
  wasConnected = isConnected;

  const uint32_t now = millis();
  if (isConnected && (lastTelemetryAt == 0 || now - lastTelemetryAt >= kTelemetryIntervalMs)) {
    // This heartbeat keeps the dashboard online and reports the actual output state.
    if (dexmod.publish("lamp", lampEnabled)) {
      lastTelemetryAt = now;
      Serial.println("[Dexmod] Lamp telemetry published");
    }
  }
}
