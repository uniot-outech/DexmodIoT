# GitHub and Arduino release checklist

1. Create `https://github.com/uniot-outech/DexmodIoT`.
2. Use the contents of this directory as the repository root.
3. Confirm that no real Wi-Fi password, device ID, or device token is present.
4. Install the folder locally and compile `examples/BasicTelemetry` for an ESP32.
5. Create an annotated `v0.1.0` tag and a GitHub release.
6. Attach `DexmodIoT-0.1.0.zip`; the archive must contain a single top-level
   `DexmodIoT` directory with `library.properties` directly inside it.
7. After the public repository and release are stable, submit the repository URL
   to the Arduino Library Registry. Future releases must update
   `library.properties` and use a matching semantic-version tag.

Users can install the release immediately with Arduino IDE's **Add .ZIP
Library…** command; registry publication is only required for searchable Library
Manager installation.
