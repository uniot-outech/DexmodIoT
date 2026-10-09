#include <DexmodIoT.h>
#include <DexmodOTA.h>
#include "arduino_secrets.h"

DexmodIoT dexmod;
DexmodOTA ota;
uint32_t lastSample = 0;
uint32_t connectedSince = 0;

// This demonstration has NO physical actuators. For a relay sketch, disable
// every output here and stop local timers; return false unless safety is confirmed.
bool prepareSafeState() {
  Serial.println("Entering OTA maintenance (this example has no actuators).");
  return true;
}

void setup() {
  Serial.begin(115200);
  dexmod.begin(WIFI_SSID, WIFI_PASSWORD, DEXMOD_DEVICE_ID, DEXMOD_DEVICE_TOKEN, 40);
  if (!ota.begin(DEXMOD_DEVICE_TOKEN, prepareSafeState)) Serial.println(ota.lastError());
}

void loop() {
  dexmod.loop();
  if (dexmod.connected()) {
    if (!connectedSince) connectedSince = millis();
  } else connectedSince = 0;
  // Demo startup test: stable TLS/MQTT connectivity. Real sketches must also
  // validate their hardware and safe output state before passing this test.
  ota.loop(connectedSince && millis() - connectedSince >= 5000);
  if (!ota.maintenance() && dexmod.connected() && millis() - lastSample >= 40000) {
    if (dexmod.publish("uptime_seconds", millis() / 1000.0, 0)) lastSample = millis();
  }
  delay(5);
}
