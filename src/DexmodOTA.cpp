#include "DexmodOTA.h"
#include "DexmodIoT.h"
#include "DexmodOTAReadiness.h"
#include "DexmodRootCA.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <time.h>

// Arduino normally approves a pending image before setup(). Defer approval
// until DexmodOTA has checked the running digest and the caller's self-test.
// This strong hook is linked only when the opt-in receiver is used.
extern "C" bool verifyRollbackLater() { return true; }

namespace {
const char* endpoint = "https://dexmodapp.com/api/iot/ota";
int chipId() {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  return 5;
#elif defined(CONFIG_IDF_TARGET_ESP32)
  return 0;
#else
  return -1;
#endif
}
bool uuid(const String& value) {
  if (value.length() != 36) return false;
  for (size_t index = 0; index < 36; ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) { if (value[index] != '-') return false; }
    else if (!isxdigit(static_cast<unsigned char>(value[index]))) return false;
  }
  return true;
}
bool hashString(const String& value) {
  if (value.length() != 64) return false;
  for (size_t index = 0; index < 64; ++index) if (!isxdigit(static_cast<unsigned char>(value[index]))) return false;
  return true;
}
String hex(const uint8_t* bytes) {
  char result[65];
  for (size_t index = 0; index < 32; ++index) snprintf(result + index * 2, 3, "%02x", bytes[index]);
  return String(result);
}
int hashStart(mbedtls_sha256_context* context) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_starts(context, 0);
#else
  return mbedtls_sha256_starts_ret(context, 0);
#endif
}
int hashUpdate(mbedtls_sha256_context* context, const uint8_t* bytes, size_t size) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_update(context, bytes, size);
#else
  return mbedtls_sha256_update_ret(context, bytes, size);
#endif
}
int hashFinish(mbedtls_sha256_context* context, uint8_t* digest) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_finish(context, digest);
#else
  return mbedtls_sha256_finish_ret(context, digest);
#endif
}
void configure(HTTPClient& http, WiFiClientSecure& tls) {
  tls.setCACert(DEXMOD_ROOT_CA);
  tls.setHandshakeTimeout(10);
  tls.setTimeout(5000);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
}
}

bool DexmodOTA::begin(const char* token, DexmodOTASafeState safeState, uint32_t timeoutMs) {
  if (started_ || !token || !*token || !safeState || chipId() < 0) { error_ = "OTA needs a supported board, token and safe-state callback"; return false; }
  const DexmodOTAReadiness ready = dexmodInspectOTA();
  if (!ready.twoSlots || !ready.otaData || !ready.rollbackBuildEnabled) { error_ = "OTA layout or rollback SDK support is missing"; return false; }
  if (!storage_.begin("dexmod-ota", false)) { error_ = "OTA replay storage is unavailable"; return false; }
  token_ = token; safeState_ = safeState; timeoutMs_ = constrain(timeoutMs, 30000UL, 300000UL);
  bootAt_ = millis(); started_ = true;
  const String pending = storage_.getString("pending", "");
  failedRelease_ = storage_.getString("failed", "");
  maintenance_ = pending.length() > 0 || failedRelease_.length() > 0;
  if (ready.pendingVerification && !pending.length()) { safeState_(); started_ = false; error_ = "Untracked pending image: recover through USB"; return false; }
  if (maintenance_ && !safeState_()) { error_ = "Startup safe-state callback failed"; started_ = false; return false; }
  return true;
}

bool DexmodOTA::request(const String& body, String& response, bool post) {
  WiFiClientSecure tls; HTTPClient http; configure(http, tls);
  if (!http.begin(tls, endpoint)) return false;
  http.addHeader("Authorization", "Bearer " + token_);
  if (post) http.addHeader("Content-Type", "application/json");
  const int code = post ? http.POST(body) : http.GET();
  response = "";
  bool ok = code == 200 || code == 204;
  if (ok && code != 204) {
    const int length = http.getSize();
    if (length <= 0 || length > 2048) ok = false;
    else {
      auto* stream = http.getStreamPtr();
      const uint32_t start = millis();
      while (response.length() < static_cast<size_t>(length) && millis() - start < 10000) {
        if (stream->available()) response += static_cast<char>(stream->read());
        else if (!http.connected()) break;
        else delay(1);
      }
      ok = response.length() == static_cast<size_t>(length);
    }
  }
  http.end(); return ok;
}

bool DexmodOTA::phase(const String& id, const char* state, const String& sha, const String& grant) {
  String ignored;
  return request("{\"action\":\"phase\",\"releaseId\":" + DexmodIoT::jsonString(id) + ",\"phase\":" + DexmodIoT::jsonString(state) + ",\"sha256\":" + DexmodIoT::jsonString(sha) + ",\"grant\":" + DexmodIoT::jsonString(grant) + "}", ignored, true);
}

void DexmodOTA::clearPending() {
  storage_.remove("pending"); storage_.remove("digest"); storage_.remove("size"); storage_.remove("confirmed");
}

bool DexmodOTA::failInstall(const char* reason) {
  clearPending(); error_ = reason;
  // The release was journaled before downloading. Retry from loop(), even in
  // maintenance; an unavailable network must not lose the failure report.
  polled_ = false;
  return false;
}

bool DexmodOTA::runningDigest(size_t size, String& digest) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || !size || size > running->size) return false;
  mbedtls_sha256_context hash; mbedtls_sha256_init(&hash);
  bool ok = hashStart(&hash) == 0;
  uint8_t buffer[2048], result[32];
  for (size_t offset = 0; ok && offset < size; offset += sizeof(buffer)) {
    const size_t count = min(sizeof(buffer), size - offset);
    ok = esp_partition_read(running, offset, buffer, count) == ESP_OK && hashUpdate(&hash, buffer, count) == 0;
    delay(1);
  }
  ok = ok && hashFinish(&hash, result) == 0;
  mbedtls_sha256_free(&hash);
  if (ok) digest = hex(result);
  return ok;
}

bool DexmodOTA::install(const String& metadata) {
  String id, grant, sha;
  if (!DexmodIoT::extractJsonString(metadata, "releaseId", id) || !uuid(id) ||
      !DexmodIoT::extractJsonString(metadata, "grant", grant) || grant.length() != 43 ||
      !DexmodIoT::extractJsonString(metadata, "sha256", sha) || !hashString(sha)) { error_ = "Invalid OTA metadata"; return false; }
  for (size_t index = 0; index < grant.length(); ++index) if (!isalnum(static_cast<unsigned char>(grant[index])) && grant[index] != '_' && grant[index] != '-') { error_ = "Invalid OTA grant"; return false; }
  auto number = [&](const char* key, uint32_t& value) {
    String encoded;
    if (!DexmodIoT::extractJsonValue(metadata, key, encoded) || !encoded.length() || encoded.length() > 10) return false;
    for (size_t index = 0; index < encoded.length(); ++index) if (!isdigit(static_cast<unsigned char>(encoded[index]))) return false;
    const unsigned long long parsed = strtoull(encoded.c_str(), nullptr, 10);
    if (parsed > UINT32_MAX) return false;
    value = static_cast<uint32_t>(parsed); return true;
  };
  uint32_t size, expires, candidateChip, offset0, bytes0, offset1, bytes1;
  const DexmodOTAReadiness ready = dexmodInspectOTA();
  if (!number("size", size) || !number("expires", expires) || !number("chipId", candidateChip) ||
      !number("slot0Offset", offset0) || !number("slot0Bytes", bytes0) || !number("slot1Offset", offset1) || !number("slot1Bytes", bytes1) ||
      candidateChip != static_cast<uint32_t>(chipId()) || size < 24 || size > min(ready.slot0Bytes, ready.slot1Bytes) ||
      offset0 != ready.slot0Offset || offset1 != ready.slot1Offset || bytes0 != ready.slot0Bytes || bytes1 != ready.slot1Bytes ||
      time(nullptr) >= static_cast<time_t>(expires)) { error_ = "OTA chip, layout, size or expiry mismatch"; return false; }
  if (storage_.getString("attempt", "") == id) return false;
  maintenance_ = true;
  // Journal the in-flight attempt before any flash write. After an unexpected
  // reset, an attempt without pending boot verification must report failure.
  failedRelease_ = id;
  if (storage_.putString("failed", id) != id.length()) { safeState_(); error_ = "Cannot persist OTA failure journal"; return false; }
  if (storage_.putString("attempt", id) != id.length()) { safeState_(); return failInstall("Cannot persist OTA replay protection"); }
  if (!safeState_()) return failInstall("Cannot enter safe output state");
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!target || !running || target->address == running->address || size > target->size) return failInstall("No inactive OTA slot");
  WiFiClientSecure tls; HTTPClient http; configure(http, tls);
  if (!http.begin(tls, String(endpoint) + "?releaseId=" + id)) return failInstall("OTA request setup failed");
  http.addHeader("Authorization", "Bearer " + grant);
  if (http.GET() != 200 || http.getSize() != static_cast<int>(size)) { http.end(); return failInstall("OTA download was denied or size changed"); }
  esp_ota_handle_t handle = 0;
  bool open = false;
  mbedtls_sha256_context hash; mbedtls_sha256_init(&hash);
  auto abort = [&](const char* reason) {
    if (open) esp_ota_abort(handle);
    mbedtls_sha256_free(&hash); http.end();
    return failInstall(reason);
  };
  auto* stream = http.getStreamPtr();
  uint8_t buffer[2048], digest[32];
  const uint32_t startedAt = millis();
  if (stream->readBytes(buffer, 24) != 24 || buffer[0] != 0xe9 || (static_cast<uint16_t>(buffer[12]) | static_cast<uint16_t>(buffer[13]) << 8) != chipId()) return abort("Wrong or incomplete application header");
  if (esp_ota_begin(target, size, &handle) != ESP_OK) return abort("Cannot begin OTA write");
  open = true;
  if (hashStart(&hash) != 0 || hashUpdate(&hash, buffer, 24) != 0 || esp_ota_write(handle, buffer, 24) != ESP_OK) return abort("OTA header write failed");
  size_t received = 24;
  while (received < size) {
    if (millis() - startedAt >= timeoutMs_ || time(nullptr) >= static_cast<time_t>(expires)) return abort("OTA download timed out");
    const int available = stream->available();
    if (available <= 0) { if (!http.connected()) return abort("OTA download interrupted"); delay(1); continue; }
    const size_t count = min(min(static_cast<size_t>(available), sizeof(buffer)), static_cast<size_t>(size) - received);
    const int read = stream->read(buffer, count);
    if (read <= 0 || hashUpdate(&hash, buffer, read) != 0 || esp_ota_write(handle, buffer, read) != ESP_OK) return abort("OTA flash write failed");
    received += read; delay(1);
  }
  if (hashFinish(&hash, digest) != 0 || hex(digest) != sha) return abort("OTA SHA-256 mismatch");
  const esp_err_t validation = esp_ota_end(handle); open = false;
  if (validation != ESP_OK) return abort("Application image validation failed");
  mbedtls_sha256_free(&hash); http.end();
  // Save candidate identity before boot selection. Never approve an image automatically.
  if (storage_.putString("digest", sha) != sha.length() || storage_.putUInt("size", size) != sizeof(uint32_t) || storage_.putString("pending", id) != id.length()) return failInstall("Cannot save OTA boot verification");
  if (!phase(id, "staged", sha, grant)) return failInstall("Final OTA authorization failed");
  if (esp_ota_set_boot_partition(target) != ESP_OK) return failInstall("Cannot select OTA boot partition");
  ESP.restart(); return true;
}

void DexmodOTA::loop(bool startupSelfTestPassed) {
  if (!started_) return;
  const String pending = storage_.getString("pending", "");
  if (pending.length()) {
    String digest;
    if (!checkedBoot_) {
      checkedBoot_ = true;
      bootMismatch_ = !runningDigest(storage_.getUInt("size", 0), digest) || digest != storage_.getString("digest", "");
    }
    const bool networkReady = WiFi.status() == WL_CONNECTED && time(nullptr) >= 1700000000;
    if (bootMismatch_) {
      if (networkReady && (!polled_ || millis() - lastPoll_ >= 60000)) {
        polled_ = true; lastPoll_ = millis();
        if (phase(pending, "rolled_back") && (!storage_.isKey("failed") || storage_.remove("failed"))) { failedRelease_ = ""; clearPending(); }
      }
      error_ = "OTA rolled back or running image mismatch"; return;
    }
    if (!storage_.getBool("confirmed", false) && millis() - bootAt_ >= 30000 && (!startupSelfTestPassed || !networkReady)) {
      safeState_(); error_ = "Startup self-test timed out";
      esp_ota_mark_app_invalid_rollback_and_reboot(); return;
    }
    if (!storage_.getBool("confirmed", false)) {
      if (!dexmodInspectOTA().pendingVerification) { error_ = "Installed bootloader did not enter pending verification"; return; }
      if (!startupSelfTestPassed || !networkReady) return;
      if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK || storage_.putBool("confirmed", true) != 1) { error_ = "Cannot confirm new application"; return; }
    }
    if (networkReady && (!polled_ || millis() - lastPoll_ >= 60000)) {
      polled_ = true; lastPoll_ = millis();
      if (phase(pending, "healthy", storage_.getString("digest", "")) && (!storage_.isKey("failed") || storage_.remove("failed"))) { failedRelease_ = ""; clearPending(); maintenance_ = false; error_ = nullptr; }
      // Retry reporting without reapproving the image or automatically resuming cloud controls.
    }
    return;
  }
  if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000 || (polled_ && millis() - lastPoll_ < 60000)) return;
  if (failedRelease_.length()) {
    polled_ = true; lastPoll_ = millis();
    if (phase(failedRelease_, "failed") && storage_.remove("failed")) failedRelease_ = "";
    // Acknowledgement only clears the report journal, never the safe state.
    // Owner inspection/resume and an explicit reset/recovery remain required.
    return;
  }
  if (maintenance_) return;
  polled_ = true; lastPoll_ = millis();
  const DexmodOTAReadiness ready = dexmodInspectOTA();
  String ignored, metadata;
  const String capabilities = "{\"action\":\"capabilities\",\"protocol\":1,\"chipId\":" + String(chipId()) +
    ",\"slot0Offset\":" + String(ready.slot0Offset) + ",\"slot0Bytes\":" + String(ready.slot0Bytes) +
    ",\"slot1Offset\":" + String(ready.slot1Offset) + ",\"slot1Bytes\":" + String(ready.slot1Bytes) +
    ",\"otaData\":" + (ready.otaData ? "true" : "false") + ",\"rollbackBuildEnabled\":" + (ready.rollbackBuildEnabled ? "true" : "false") + "}";
  if (!request(capabilities, ignored, true) || !request("", metadata, false)) { error_ = "OTA pilot unavailable"; return; }
  if (metadata.length()) install(metadata);
}
