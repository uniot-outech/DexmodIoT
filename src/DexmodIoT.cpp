#include "DexmodIoT.h"

#include <WiFi.h>
#include <esp_system.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "DexmodRootCA.h"

namespace {
constexpr char kBrokerHost[] = "mqtt.dexmodapp.com";
constexpr uint16_t kBrokerPort = 8883;
constexpr size_t kMaximumMetrics = 32;
constexpr size_t kMaximumMetricNameLength = 40;
constexpr size_t kMaximumPayloadBytes = 4096;
constexpr size_t kMaximumCommandIdLength = 128;
constexpr size_t kMaximumControlNameLength = 40;

bool isAsciiLetter(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool isAsciiDigit(char value) {
  return value >= '0' && value <= '9';
}

bool isJsonWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool validCommandId(const String& value) {
  if (value.isEmpty() || value.length() > kMaximumCommandIdLength) return false;
  if (!isAsciiLetter(value[0]) && !isAsciiDigit(value[0])) return false;
  for (size_t index = 1; index < value.length(); ++index) {
    const char current = value[index];
    if (!isAsciiLetter(current) && !isAsciiDigit(current) && current != '.' && current != '_' && current != ':' && current != '-') return false;
  }
  return true;
}

bool validControlName(const String& value) {
  if (value.isEmpty() || value.length() > kMaximumControlNameLength || !isAsciiLetter(value[0])) return false;
  for (size_t index = 1; index < value.length(); ++index) {
    const char current = value[index];
    if (!isAsciiLetter(current) && !isAsciiDigit(current) && current != '.' && current != '_' && current != '-') return false;
  }
  return true;
}

bool validErrorCode(const char* value) {
  if (value == nullptr || value[0] == '\0') return false;
  size_t length = 0;
  for (; value[length] != '\0'; ++length) {
    if (length >= 60 || (!isAsciiLetter(value[length]) && !isAsciiDigit(value[length]) && value[length] != '_')) return false;
  }
  return length > 0;
}

bool isLeapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int daysInMonth(int year, int month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && isLeapYear(year) ? 29 : days[month - 1];
}

int64_t daysSinceUnixEpoch(int year, unsigned int month, unsigned int day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned int yearOfEra = static_cast<unsigned int>(year - era * 400);
  const unsigned int adjustedMonth = month > 2 ? month - 3 : month + 9;
  const unsigned int dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
  const unsigned int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
}
}  // namespace

const char* DexmodCommand::id() const {
  return id_.c_str();
}

const char* DexmodCommand::control() const {
  return control_.c_str();
}

DexmodCommandValueType DexmodCommand::valueType() const {
  return valueType_;
}

bool DexmodCommand::booleanValue() const {
  return booleanValue_;
}

double DexmodCommand::numberValue() const {
  return numberValue_;
}

const char* DexmodCommand::stringValue() const {
  return stringValue_.c_str();
}

time_t DexmodCommand::expiresAt() const {
  return expiresAt_;
}

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
    nextRecentCommand_(0),
    commandHandler_(nullptr),
    lastError_("begin() has not been called"),
    minimumPublishIntervalMs_(15000),
    lastWifiAttemptAt_(0),
    lastMqttAttemptAt_(0),
    lastPublishedAt_(0),
    bootId_(0),
    sequence_(0),
    started_(false),
    wifiAttempted_(false),
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
  commandTopic_ = "dexmod/devices/" + deviceId_ + "/commands";
  acknowledgementTopic_ = "dexmod/devices/" + deviceId_ + "/command-acks";
  for (size_t index = 0; index < kRecentCommandCount; ++index) recentCommandIds_[index] = "";
  nextRecentCommand_ = 0;
  bootId_ = esp_random();
  sequence_ = 0;
  wifiAttempted_ = false;
  hasPublished_ = false;
  clockRequested_ = false;
  clockReady_ = false;
  lastWifiAttemptAt_ = 0;
  lastMqttAttemptAt_ = millis() - kRetryIntervalMs;

  tlsClient_.setCACert(DEXMOD_ROOT_CA);
  mqtt_.setServer(kBrokerHost, kBrokerPort);
  mqtt_.setBufferSize(kMqttBufferBytes);
  mqtt_.setKeepAlive(30);
  mqtt_.setSocketTimeout(15);
  mqtt_.setCallback([this](char* topic, uint8_t* payload, unsigned int length) {
    handleMqttMessage(topic, payload, length);
  });
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  started_ = true;
  setError("Connecting");
  loop();
}

void DexmodIoT::maintainWifi(uint32_t nowMs) {
  if (WiFi.status() == WL_CONNECTED) return;
  if (wifiAttempted_ && nowMs - lastWifiAttemptAt_ < kWifiConnectTimeoutMs) return;
  lastWifiAttemptAt_ = nowMs;
  wifiAttempted_ = true;
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
    if (commandHandler_ == nullptr || mqtt_.subscribe(commandTopic_.c_str(), 1)) {
      setError(nullptr);
    } else {
      mqtt_.disconnect();
      setError("MQTT command subscription failed; retrying");
    }
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

void DexmodIoT::onCommand(DexmodCommandHandler handler) {
  commandHandler_ = handler;
  if (!mqtt_.connected()) return;
  if (handler == nullptr) {
    mqtt_.unsubscribe(commandTopic_.c_str());
    return;
  }
  if (!mqtt_.subscribe(commandTopic_.c_str(), 1)) {
    setError("MQTT command subscription failed; reconnecting");
    mqtt_.disconnect();
  }
}

bool DexmodIoT::extractJsonString(const String& json, const char* key, String& value) {
  const String marker = String('"') + key + '"';
  int cursor = json.indexOf(marker);
  if (cursor < 0) return false;
  cursor += marker.length();
  while (cursor < static_cast<int>(json.length()) && isJsonWhitespace(json[cursor])) ++cursor;
  if (cursor >= static_cast<int>(json.length()) || json[cursor++] != ':') return false;
  while (cursor < static_cast<int>(json.length()) && isJsonWhitespace(json[cursor])) ++cursor;
  if (cursor >= static_cast<int>(json.length()) || json[cursor++] != '"') return false;

  value = "";
  while (cursor < static_cast<int>(json.length())) {
    const char current = json[cursor++];
    if (current == '"') return true;
    if (current != '\\') {
      if (static_cast<uint8_t>(current) < 0x20) return false;
      value += current;
      continue;
    }
    if (cursor >= static_cast<int>(json.length())) return false;
    const char escaped = json[cursor++];
    if (escaped == '"' || escaped == '\\' || escaped == '/') value += escaped;
    else if (escaped == 'b') value += '\b';
    else if (escaped == 'f') value += '\f';
    else if (escaped == 'n') value += '\n';
    else if (escaped == 'r') value += '\r';
    else if (escaped == 't') value += '\t';
    else return false;
  }
  return false;
}

bool DexmodIoT::extractJsonValue(const String& json, const char* key, String& value) {
  const String marker = String('"') + key + '"';
  int cursor = json.indexOf(marker);
  if (cursor < 0) return false;
  cursor += marker.length();
  while (cursor < static_cast<int>(json.length()) && isJsonWhitespace(json[cursor])) ++cursor;
  if (cursor >= static_cast<int>(json.length()) || json[cursor++] != ':') return false;
  while (cursor < static_cast<int>(json.length()) && isJsonWhitespace(json[cursor])) ++cursor;
  if (cursor >= static_cast<int>(json.length())) return false;

  const int start = cursor;
  if (json[cursor] == '"') {
    ++cursor;
    bool escaped = false;
    while (cursor < static_cast<int>(json.length())) {
      const char current = json[cursor++];
      if (escaped) {
        escaped = false;
      } else if (current == '\\') {
        escaped = true;
      } else if (current == '"') {
        value = json.substring(start, cursor);
        return true;
      }
    }
    return false;
  }

  while (cursor < static_cast<int>(json.length()) && json[cursor] != ',' && json[cursor] != '}') ++cursor;
  value = json.substring(start, cursor);
  value.trim();
  return !value.isEmpty();
}

bool DexmodIoT::parseIsoTimestamp(const String& value, time_t& timestamp) {
  if (value.length() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[value.length() - 1] != 'Z') return false;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (sscanf(value.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) return false;
  if (year < 2020 || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month) ||
      hour > 23 || minute > 59 || second > 59) return false;
  const int64_t seconds = daysSinceUnixEpoch(year, month, day) * 86400LL +
    static_cast<int64_t>(hour) * 3600 + minute * 60 + second;
  timestamp = static_cast<time_t>(seconds);
  return timestamp >= static_cast<time_t>(kValidUnixTime);
}

bool DexmodIoT::parseCommand(const String& json, DexmodCommand& command) {
  String expiresAt;
  String rawValue;
  if (!extractJsonString(json, "commandId", command.id_) || !validCommandId(command.id_) ||
      !extractJsonString(json, "control", command.control_) || !validControlName(command.control_) ||
      !extractJsonString(json, "expiresAt", expiresAt) || !parseIsoTimestamp(expiresAt, command.expiresAt_) ||
      !extractJsonValue(json, "value", rawValue)) return false;

  command.stringValue_ = "";
  command.booleanValue_ = false;
  command.numberValue_ = 0;
  command.valueType_ = DexmodCommandValueType::Invalid;
  if (rawValue == "true" || rawValue == "false") {
    command.valueType_ = DexmodCommandValueType::Boolean;
    command.booleanValue_ = rawValue == "true";
    return true;
  }
  if (rawValue[0] == '"') {
    if (!extractJsonString(json, "value", command.stringValue_)) return false;
    command.valueType_ = DexmodCommandValueType::String;
    return true;
  }

  char* end = nullptr;
  const double number = strtod(rawValue.c_str(), &end);
  if (end == rawValue.c_str() || *end != '\0' || !isfinite(number)) return false;
  command.valueType_ = DexmodCommandValueType::Number;
  command.numberValue_ = number;
  return true;
}

bool DexmodIoT::rememberCommand(const String& commandId) {
  for (size_t index = 0; index < kRecentCommandCount; ++index) {
    if (recentCommandIds_[index] == commandId) return false;
  }
  recentCommandIds_[nextRecentCommand_] = commandId;
  nextRecentCommand_ = (nextRecentCommand_ + 1) % kRecentCommandCount;
  return true;
}

void DexmodIoT::handleMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
  if (topic == nullptr || commandTopic_ != topic) return;
  if (payload == nullptr || length == 0 || length >= kMaximumPayloadBytes) {
    setError("Ignored an invalid command payload");
    return;
  }
  String json;
  json.reserve(length);
  for (unsigned int index = 0; index < length; ++index) json += static_cast<char>(payload[index]);

  DexmodCommand command;
  if (!parseCommand(json, command)) {
    setError("Ignored a malformed command");
    return;
  }
  if (time(nullptr) >= command.expiresAt()) {
    setError("Ignored an expired command");
    return;
  }
  if (!rememberCommand(command.id_)) return;
  if (commandHandler_ == nullptr) {
    fail(command, "NO_COMMAND_HANDLER", "No command handler is registered");
    setError("Command received without a registered handler");
    return;
  }
  setError(nullptr);
  commandHandler_(*this, command);
}

bool DexmodIoT::publishAcknowledgement(
  const DexmodCommand& command,
  bool success,
  const String& stateJson,
  const char* errorCode,
  const char* message
) {
  if (!connected()) {
    setError("Command acknowledgement was not sent because MQTT is disconnected");
    return false;
  }
  String payload = "{\"commandId\":" + jsonString(command.id_) +
    ",\"control\":" + jsonString(command.control_) +
    ",\"success\":" + String(success ? "true" : "false");
  if (success) {
    payload += ",\"state\":" + stateJson;
  } else {
    payload += ",\"errorCode\":" + jsonString(validErrorCode(errorCode) ? String(errorCode) : String("COMMAND_FAILED"));
    payload += ",\"error\":" + jsonString(message == nullptr ? "Command failed" : String(message));
  }
  payload += '}';
  if (payload.length() >= kMaximumPayloadBytes) {
    setError("Command acknowledgement exceeds the library buffer");
    return false;
  }
  if (!mqtt_.publish(acknowledgementTopic_.c_str(), payload.c_str(), false)) {
    setError("MQTT command acknowledgement publish failed");
    return false;
  }
  setError(nullptr);
  return true;
}

bool DexmodIoT::acknowledge(const DexmodCommand& command, bool reportedState) {
  return publishAcknowledgement(command, true, reportedState ? "true" : "false", nullptr, nullptr);
}

bool DexmodIoT::acknowledge(const DexmodCommand& command, double reportedState, unsigned int decimalPlaces) {
  if (!isfinite(reportedState)) {
    setError("Reported command state must be a finite number");
    return false;
  }
  if (decimalPlaces > 9) decimalPlaces = 9;
  return publishAcknowledgement(command, true, String(reportedState, decimalPlaces), nullptr, nullptr);
}

bool DexmodIoT::acknowledge(const DexmodCommand& command, const char* reportedState) {
  return publishAcknowledgement(command, true, jsonString(reportedState == nullptr ? "" : String(reportedState)), nullptr, nullptr);
}

bool DexmodIoT::fail(const DexmodCommand& command, const char* errorCode, const char* message) {
  return publishAcknowledgement(command, false, "", errorCode, message);
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
