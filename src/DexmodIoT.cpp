#include "DexmodIoT.h"

#include <WiFi.h>
#include <esp_system.h>
#include <math.h>
#include <time.h>

#include "DexmodRootCA.h"

namespace {
constexpr char kBrokerHost[] = "mqtt.dexmodapp.com";
constexpr uint16_t kBrokerPort = 8883;
constexpr size_t kMaximumMetrics = 32;
constexpr size_t kMaximumMetricNameLength = 40;
constexpr size_t kMaximumPayloadBytes = 4096;

bool isAsciiLetter(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool isAsciiDigit(char value) {
  return value >= '0' && value <= '9';
}
}  // namespace

DexmodTelemetry::DexmodTelemetry() {
  clear();
}

void DexmodTelemetry::clear() {
  body_ = "";
  body_.reserve(256);
  count_ = 0;
}

bool DexmodTelemetry::validMetricName(const char* name) {
  if (name == nullptr || !isAsciiLetter(name[0])) return false;
  size_t length = 1;
  for (; name[length] != '\0'; ++length) {
    const char value = name[length];
    if (length >= kMaximumMetricNameLength) return false;
    if (!isAsciiLetter(value) && !isAsciiDigit(value) && value != '_' && value != '.' && value != '-') return false;
  }
  return length <= kMaximumMetricNameLength;
}

bool DexmodTelemetry::addEncoded(const char* name, const String& encodedValue) {
  if (!validMetricName(name) || count_ >= kMaximumMetrics) return false;
  if (count_ > 0) body_ += ',';
  body_ += '"';
  body_ += name;
  body_ += "\":";
  body_ += encodedValue;
  ++count_;
  return true;
}

bool DexmodTelemetry::add(const char* name, double value, unsigned int decimalPlaces) {
  if (!isfinite(value)) return false;
  if (decimalPlaces > 9) decimalPlaces = 9;
  return addEncoded(name, String(value, decimalPlaces));
}

bool DexmodTelemetry::add(const char* name, bool value) {
  return addEncoded(name, value ? "true" : "false");
}

bool DexmodTelemetry::empty() const {
  return count_ == 0;
}

size_t DexmodTelemetry::count() const {
  return count_;
}

String DexmodTelemetry::json() const {
  return String('{') + body_ + '}';
}

DexmodIoT::DexmodIoT()
  : mqtt_(tlsClient_),
    lastError_("begin() has not been called"),
    minimumPublishIntervalMs_(15000),
    lastWifiAttemptAt_(0),
    lastMqttAttemptAt_(0),
    lastPublishedAt_(0),
    bootId_(0),
    sequence_(0),
    started_(false),
    hasPublished_(false),
    clockRequested_(false),
    clockReady_(false) {}

void DexmodIoT::begin(
  const char* wifiSsid,
  const char* wifiPassword,
  const char* deviceId,
  const char* deviceToken,
  uint32_t minimumPublishIntervalSeconds
) {
  wifiSsid_ = wifiSsid == nullptr ? "" : wifiSsid;
  wifiPassword_ = wifiPassword == nullptr ? "" : wifiPassword;
  deviceId_ = deviceId == nullptr ? "" : deviceId;
  deviceToken_ = deviceToken == nullptr ? "" : deviceToken;

  if (wifiSsid_.isEmpty() || deviceId_.isEmpty() || deviceToken_.isEmpty()) {
    started_ = false;
    setError("Wi-Fi SSID, device ID, and device token are required");
    return;
  }

  if (minimumPublishIntervalSeconds > UINT32_MAX / 1000UL) {
    minimumPublishIntervalSeconds = UINT32_MAX / 1000UL;
  }
  minimumPublishIntervalMs_ = minimumPublishIntervalSeconds * 1000UL;
  topic_ = "dexmod/devices/" + deviceId_ + "/telemetry";
  bootId_ = esp_random();
  sequence_ = 0;
  hasPublished_ = false;
  clockRequested_ = false;
  clockReady_ = false;
  lastWifiAttemptAt_ = millis() - kRetryIntervalMs;
  lastMqttAttemptAt_ = millis() - kRetryIntervalMs;

  tlsClient_.setCACert(DEXMOD_ROOT_CA);
  mqtt_.setServer(kBrokerHost, kBrokerPort);
  mqtt_.setBufferSize(kMqttBufferBytes);
  mqtt_.setKeepAlive(30);
  mqtt_.setSocketTimeout(15);
  WiFi.mode(WIFI_STA);
  started_ = true;
  setError("Connecting");
  loop();
}

void DexmodIoT::maintainWifi(uint32_t nowMs) {
  if (WiFi.status() == WL_CONNECTED) return;
  if (nowMs - lastWifiAttemptAt_ < kRetryIntervalMs) return;
  lastWifiAttemptAt_ = nowMs;
  WiFi.begin(wifiSsid_.c_str(), wifiPassword_.c_str());
  setError("Connecting to Wi-Fi");
}

void DexmodIoT::maintainClock() {
  if (WiFi.status() != WL_CONNECTED || clockReady_) return;
  if (!clockRequested_) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    clockRequested_ = true;
  }
  const time_t now = time(nullptr);
  if (now >= static_cast<time_t>(kValidUnixTime)) {
    clockReady_ = true;
    setError("Connecting to MQTT");
  } else {
    setError("Synchronizing clock");
  }
}

void DexmodIoT::maintainMqtt(uint32_t nowMs) {
  if (!clockReady_ || WiFi.status() != WL_CONNECTED || mqtt_.connected()) return;
  if (nowMs - lastMqttAttemptAt_ < kRetryIntervalMs) return;
  lastMqttAttemptAt_ = nowMs;
  if (mqtt_.connect(deviceId_.c_str(), deviceId_.c_str(), deviceToken_.c_str())) {
    setError(nullptr);
  } else {
    setError("MQTT connection failed; retrying");
  }
}

void DexmodIoT::loop() {
  if (!started_) return;
  const uint32_t nowMs = millis();
  maintainWifi(nowMs);
  maintainClock();
  maintainMqtt(nowMs);
  if (mqtt_.connected()) mqtt_.loop();
}

bool DexmodIoT::waitUntilReady(uint32_t timeoutMs) {
  const uint32_t startedAt = millis();
  while (!connected() && millis() - startedAt < timeoutMs) {
    loop();
    delay(10);
  }
  if (!connected()) setError("Connection timed out; loop() will keep retrying");
  return connected();
}

bool DexmodIoT::connected() {
  return started_ && clockReady_ && WiFi.status() == WL_CONNECTED && mqtt_.connected();
}

String DexmodIoT::jsonString(const String& value) {
  String result = "\"";
  result.reserve(value.length() + 2);
  for (size_t index = 0; index < value.length(); ++index) {
    const char current = value[index];
    if (current == '\\' || current == '"') {
      result += '\\';
      result += current;
    } else if (current == '\n') {
      result += "\\n";
    } else if (current == '\r') {
      result += "\\r";
    } else if (current == '\t') {
      result += "\\t";
    } else if (static_cast<uint8_t>(current) >= 0x20) {
      result += current;
    }
  }
  result += '"';
  return result;
}

String DexmodIoT::nextMessageId() {
  ++sequence_;
  char bootHex[9];
  snprintf(bootHex, sizeof(bootHex), "%08lx", static_cast<unsigned long>(bootId_));
  return "esp32-" + String(bootHex) + "-" + String(sequence_);
}

bool DexmodIoT::publishJson(const String& metricsJson) {
  loop();
  if (!connected()) {
    setError("Telemetry was not sent because MQTT is disconnected");
    return false;
  }

  const uint32_t nowMs = millis();
  if (hasPublished_ && nowMs - lastPublishedAt_ < minimumPublishIntervalMs_) {
    setError("Telemetry was not sent because the minimum interval has not elapsed");
    return false;
  }

  const String messageId = nextMessageId();
  String payload = "{\"token\":" + jsonString(deviceToken_) +
    ",\"messageId\":" + jsonString(messageId) +
    ",\"metrics\":" + metricsJson + '}';
  if (payload.length() >= kMaximumPayloadBytes) {
    setError("Telemetry payload exceeds the library buffer");
    return false;
  }

  if (!mqtt_.publish(topic_.c_str(), payload.c_str(), false)) {
    setError("MQTT publish failed");
    return false;
  }

  lastPublishedAt_ = nowMs;
  hasPublished_ = true;
  setError(nullptr);
  return true;
}

bool DexmodIoT::publish(const DexmodTelemetry& telemetry) {
  if (telemetry.empty()) {
    setError("Add at least one metric before publishing");
    return false;
  }
  return publishJson(telemetry.json());
}

bool DexmodIoT::publish(const char* metricName, double value, unsigned int decimalPlaces) {
  DexmodTelemetry telemetry;
  if (!telemetry.add(metricName, value, decimalPlaces)) {
    setError("Metric name or value is invalid");
    return false;
  }
  return publish(telemetry);
}

bool DexmodIoT::publish(const char* metricName, bool value) {
  DexmodTelemetry telemetry;
  if (!telemetry.add(metricName, value)) {
    setError("Metric name is invalid");
    return false;
  }
  return publish(telemetry);
}

const char* DexmodIoT::lastError() const {
  return lastError_ == nullptr ? "" : lastError_;
}

int DexmodIoT::mqttState() {
  return mqtt_.state();
}

uint32_t DexmodIoT::lastPublishedAt() const {
  return lastPublishedAt_;
}

void DexmodIoT::setError(const char* message) {
  lastError_ = message;
}
