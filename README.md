# DexmodIoT

`DexmodIoT` connects ESP32 sketches to
[Dexmod Remote IoT Monitoring](https://dexmodapp.com) through authenticated
MQTT over TLS.

The library manages:

- the public CA trust anchor for `mqtt.dexmodapp.com`;
- Wi-Fi, clock synchronization, MQTT TLS, and automatic reconnects;
- the device-specific Dexmod topic and authenticated telemetry envelope;
- unique message IDs and the plan's minimum publication interval;
- up to 32 numeric or boolean metrics per sample.

Wi-Fi credentials and the Dexmod device token stay in the sketch. The library
never uses `setInsecure()`.

## Requirements

- An ESP32 board supported by the Espressif Arduino core.
- [PubSubClient](https://github.com/knolleary/pubsubclient) 2.8 or newer.
- A Remote IoT device created in Dexmod. Save its one-time device token.

## Install in Arduino IDE

Until the library is indexed by the Arduino Library Manager:

1. Download this repository as a ZIP, or download the ZIP from a GitHub release.
2. In Arduino IDE select **Sketch → Include Library → Add .ZIP Library…**.
3. Install `PubSubClient` from **Library Manager**.
4. Open **File → Examples → DexmodIoT → BasicTelemetry**.
5. Replace the placeholders in `arduino_secrets.h`.
6. Set the minimum interval in `dexmod.begin(...)` to the value shown by Dexmod.

## Basic use

```cpp
#include <DexmodIoT.h>

DexmodIoT dexmod;

void setup() {
  Serial.begin(115200);
  dexmod.begin(
    "WIFI_NAME",
    "WIFI_PASSWORD",
    "DEXMOD_DEVICE_ID",
    "DEXMOD_DEVICE_TOKEN",
    15
  );
  dexmod.waitUntilReady(30000);
}

void loop() {
  dexmod.loop();

  static unsigned long lastSampleAt = 0;
  if (millis() - lastSampleAt < 15000) return;
  lastSampleAt = millis();

  DexmodTelemetry telemetry;
  telemetry.add("temperature", 29.1);
  telemetry.add("pump", true);
  dexmod.publish(telemetry);
}
```

Metric names must begin with a letter and contain at most 40 letters, numbers,
underscores, dots, or hyphens. A sample may contain 1–32 metrics. Numeric values
must be finite.

## Credential safety

- Never commit a real device token or Wi-Fi password.
- A Dexmod token is used as the MQTT password and is also included in the
  telemetry envelope for server-side authorization.
- If a token is exposed, rotate it in Dexmod and update the device immediately.
- The CA in `DexmodRootCA.h` is public and safe to distribute.

## Publishing this folder as its own GitHub repository

The contents of this folder are already arranged as an Arduino library root.
Copy this folder to a repository named `DexmodIoT`, tag releases using semantic
versions such as `v0.1.0`, and attach a ZIP whose top-level directory is
`DexmodIoT`. See [PUBLISHING.md](PUBLISHING.md) for the release checklist.

## License

MIT. See [LICENSE](LICENSE).
