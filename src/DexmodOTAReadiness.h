#pragma once

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <sdkconfig.h>

// Read-only diagnostic. This does not download, erase, change the boot slot,
// reboot, or confirm a pending image. A build flag does not attest the flashed bootloader.
struct DexmodOTAReadiness {
  bool twoSlots = false;
  bool otaData = false;
  bool rollbackBuildEnabled = false;
  bool pendingVerification = false;
  uint32_t slot0Bytes = 0;
  uint32_t slot1Bytes = 0;
  uint32_t slot0Offset = 0;
  uint32_t slot1Offset = 0;
};

inline DexmodOTAReadiness dexmodInspectOTA() {
  DexmodOTAReadiness result;
  const esp_partition_t* slot0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t* slot1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  const esp_partition_t* data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  result.twoSlots = slot0 && slot1;
  result.otaData = data && data->size >= 0x2000;
  result.slot0Bytes = slot0 ? slot0->size : 0;
  result.slot1Bytes = slot1 ? slot1->size : 0;
  result.slot0Offset = slot0 ? slot0->address : 0;
  result.slot1Offset = slot1 ? slot1->address : 0;
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  result.rollbackBuildEnabled = true;
#endif
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  result.pendingVerification = running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
  return result;
}
