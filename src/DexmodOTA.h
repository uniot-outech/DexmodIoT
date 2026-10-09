#pragma once
#include <Arduino.h>
#include <Preferences.h>

typedef bool (*DexmodOTASafeState)();

// Opt-in HTTPS OTA pilot. Never call loop() inside an MQTT command callback.
// The caller must continually supply a real bounded startup self-test result.
class DexmodOTA {
 public:
  bool begin(const char* deviceToken, DexmodOTASafeState safeState, uint32_t downloadTimeoutMs = 180000);
  void loop(bool startupSelfTestPassed);
  bool maintenance() const { return maintenance_; }
  const char* lastError() const { return error_; }
 private:
  bool request(const String& body, String& response, bool post);
  bool phase(const String& id, const char* state, const String& sha = "", const String& grant = "");
  bool install(const String& metadata);
  bool runningDigest(size_t size, String& digest);
  void clearPending();
  bool failInstall(const char* reason);
  Preferences storage_;
  String token_, failedRelease_;
  DexmodOTASafeState safeState_ = nullptr;
  uint32_t lastPoll_ = 0, bootAt_ = 0, timeoutMs_ = 180000;
  bool started_ = false, polled_ = false, maintenance_ = false, checkedBoot_ = false, bootMismatch_ = false;
  const char* error_ = nullptr;
};
