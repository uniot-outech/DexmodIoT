#include <DexmodIoT.h>
#include <WiFi.h>

#include "arduino_secrets.h"

DexmodIoT dexmod;
unsigned long lastSampleAt = 0;

void setup() {
  Serial.begin(115200);

  // Use the minimum interval displayed by Dexmod for this device/plan.
  dexmod.begin(
    WIFI_SSID,
    WIFI_PASSWORD,
    DEXMOD_DEVICE_ID,
    DEXMOD_DEVICE_TOKEN,
    15
  );

  if (!dexmod.waitUntilReady(30000)) {
    Serial.print("Dexmod will keep reconnecting: ");
    Serial.println(dexmod.lastError());
  }
}

void loop() {
  dexmod.loop();

  if (millis() - lastSampleAt < 15000) return;
  lastSampleAt = millis();

  DexmodTelemetry telemetry;
  telemetry.add("temperature", 29.1);
  telemetry.add("wifi_rssi", static_cast<double>(WiFi.RSSI()), 0);
  telemetry.add("uptime_seconds", static_cast<double>(millis() / 1000), 0);

  if (dexmod.publish(telemetry)) {
    Serial.println("Telemetry published to Dexmod");
  } else {
    Serial.print("Publish pending/failed: ");
    Serial.println(dexmod.lastError());
  }
}
