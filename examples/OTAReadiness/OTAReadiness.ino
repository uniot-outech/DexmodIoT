#include <DexmodOTAReadiness.h>

// USB diagnostic only. Uploading this replaces the current sketch and stops
// its telemetry/controls. Prefer adding the diagnostic to your existing sketch.
// It never installs an OTA image or changes the selected boot partition.
void setup() {
  Serial.begin(115200);
  delay(1500);
  const DexmodOTAReadiness status = dexmodInspectOTA();
  Serial.printf("OTA slots: %s; ota_0=%lu bytes; ota_1=%lu bytes\n",
    status.twoSlots ? "present" : "missing",
    static_cast<unsigned long>(status.slot0Bytes), static_cast<unsigned long>(status.slot1Bytes));
  Serial.printf("OTA data: %s\n", status.otaData ? "present" : "missing");
  Serial.printf("Rollback SDK build flag: %s (installed bootloader still needs testing)\n",
    status.rollbackBuildEnabled ? "enabled" : "disabled");
  Serial.printf("Pending image verification: %s\n", status.pendingVerification ? "yes" : "no");
  Serial.println("Readiness only. No OTA deployment or automatic reboot is enabled.");
}

void loop() {
  delay(100);
}
