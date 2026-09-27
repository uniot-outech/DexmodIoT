# DexmodIoT

`DexmodIoT` connects ESP32 sketches to
[Dexmod Remote IoT Monitoring](https://dexmodapp.com) through authenticated
MQTT over TLS.

The library manages:

- the public CA trust anchor for `mqtt.dexmodapp.com`;
- Wi-Fi, clock synchronization, MQTT TLS, and automatic reconnects;
- the device-specific Dexmod topic and authenticated telemetry envelope;
- unique message IDs and the plan's minimum publication interval;
- up to 32 numeric or boolean metrics per sample;
- typed boolean, number, and string remote commands;
- command expiry checks, recent-command deduplication, and acknowledgements.

Wi-Fi credentials and the Dexmod device token stay in the sketch. The library
never uses `setInsecure()`.

## Requirements

- An ESP32 board supported by the Espressif Arduino core.
- A 2.4 GHz Wi-Fi network supported by the board (ESP32 does not join 5 GHz-only SSIDs).
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

## Remote control

Remote control is opt-in on the Dexmod server. Once it is enabled for a device,
register a short, non-blocking handler before calling `begin()`. Always validate
the control name and value type before changing hardware, then acknowledge the
state actually applied:

```cpp
void handleCommand(DexmodIoT& dexmod, const DexmodCommand& command) {
  if (strcmp(command.control(), "lamp") != 0 ||
      command.valueType() != DexmodCommandValueType::Boolean) {
    dexmod.fail(command, "UNSUPPORTED_COMMAND", "Expected lamp boolean");
    return;
  }

  const bool enabled = command.booleanValue();
  digitalWrite(LED_BUILTIN, enabled ? HIGH : LOW);
  dexmod.acknowledge(command, enabled);
}
```

Call `dexmod.loop()` frequently. The callback is invoked from `loop()` rather
than an interrupt, but it must still avoid delays and blocking work. See
**File → Examples → DexmodIoT → SafeLedControl** for a complete fail-safe
example that turns the output off after a connection loss and publishes a
periodic state heartbeat so the Dexmod dashboard can determine that the device
is online. Set `DEXMOD_LED_ACTIVE_LOW` to `1` when the board LED uses inverted
logic, as on many ESP32-C3 Super Mini boards.
When the board core does not define `LED_BUILTIN`, the example safely falls
back to GPIO 2 on classic ESP32 and GPIO 8 on ESP32-C3. GPIO 8 must not be used
as an LED fallback on classic ESP32 modules because it may be connected to the
SPI flash.

The library allows a Wi-Fi association attempt to run for 20 seconds before
retrying it. The example prints connection-stage changes to Serial at 115200
baud, which helps distinguish Wi-Fi, clock synchronization, and MQTT failures.

## Credential safety

- Never commit a real device token or Wi-Fi password.
- A Dexmod token is used as the MQTT password and is also included in the
  telemetry envelope for server-side authorization.
- If a token is exposed, rotate it in Dexmod and update the device immediately.
- The CA in `DexmodRootCA.h` is public and safe to distribute.

## Publishing this folder as its own GitHub repository

The contents of this folder are already arranged as an Arduino library root.
Copy this folder to a repository named `DexmodIoT`, tag releases using semantic
versions such as `v0.2.3`, and attach a ZIP whose top-level directory is
`DexmodIoT`. See [PUBLISHING.md](PUBLISHING.md) for the release checklist.

## License

MIT. See [LICENSE](LICENSE).
