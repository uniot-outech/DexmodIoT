#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

class DexmodTelemetry {
 public:
  DexmodTelemetry();

  void clear();
  bool add(const char* name, double value, unsigned int decimalPlaces = 2);
  bool add(const char* name, bool value);
  bool empty() const;
  size_t count() const;
  String json() const;

 private:
  bool addEncoded(const char* name, const String& encodedValue);
  static bool validMetricName(const char* name);

  String body_;
  size_t count_;
};

class DexmodIoT {
 public:
  DexmodIoT();

  // Starts Wi-Fi, TLS, time synchronization, and MQTT. This call is non-blocking.
  // Call loop() continuously, or use waitUntilReady() during setup.
  void begin(
    const char* wifiSsid,
    const char* wifiPassword,
    const char* deviceId,
    const char* deviceToken,
    uint32_t minimumPublishIntervalSeconds = 15
  );

  void loop();
  bool waitUntilReady(uint32_t timeoutMs = 30000);
  bool connected();

  bool publish(const DexmodTelemetry& telemetry);
  bool publish(const char* metricName, double value, unsigned int decimalPlaces = 2);
  bool publish(const char* metricName, bool value);

  const char* lastError() const;
  int mqttState();
  uint32_t lastPublishedAt() const;

 private:
  static constexpr uint32_t kRetryIntervalMs = 3000;
  static constexpr uint32_t kValidUnixTime = 1700000000UL;
  static constexpr size_t kMqttBufferBytes = 4096;

  void maintainWifi(uint32_t nowMs);
  void maintainClock();
  void maintainMqtt(uint32_t nowMs);
  bool publishJson(const String& metricsJson);
  String nextMessageId();
  static String jsonString(const String& value);
  void setError(const char* message);

  WiFiClientSecure tlsClient_;
  PubSubClient mqtt_;
  String wifiSsid_;
  String wifiPassword_;
  String deviceId_;
  String deviceToken_;
  String topic_;
  const char* lastError_;
  uint32_t minimumPublishIntervalMs_;
  uint32_t lastWifiAttemptAt_;
  uint32_t lastMqttAttemptAt_;
  uint32_t lastPublishedAt_;
  uint32_t bootId_;
  uint32_t sequence_;
  bool started_;
  bool hasPublished_;
  bool clockRequested_;
  bool clockReady_;
};
