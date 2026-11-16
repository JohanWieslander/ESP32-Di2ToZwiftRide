# Di2 to OpenBikeControl bridge

Arduino sketch for an ESP32 that reads Shimano Di2 D-Fly button indications
over BLE and advertises an OpenBikeControl TCP endpoint over Wi-Fi and mDNS.

## Hardware and setup

- ESP32 classic, ESP32-S3, or ESP32-C3 board with Wi-Fi and BLE.
- Shimano Di2 with an EW-WU111 wireless unit or built-in BLE.
- Pair/bond the Di2 unit with the ESP32 before using it. Assign the physical
  buttons to D-Fly channels 1-4 in Shimano E-Tube Project.
- Put the ESP32 and the OpenBikeControl app on the same Wi-Fi network.

No Wi-Fi credentials need to be edited into the sketch. On first boot, the
ESP32 starts an open setup network named `Di2-Bridge-XXXX`; no Wi-Fi password
is needed to join it. Connect a phone or computer to that network and open
the captive portal (or visit `http://192.168.4.1`), then submit the Wi-Fi SSID
and password. The credentials are stored in ESP32 NVS and the bridge restarts
to connect.

The bridge advertises `_openbikecontrol._tcp.local.` on TCP port 8080 as
**Di2 Bridge** after connecting to Wi-Fi. If it cannot connect within two minutes
at boot, it keeps the saved credentials and starts the setup portal so you can update them;
the credentials are replaced only when you submit new ones. If Wi-Fi is lost
for 10 seconds while running, it restarts and retries the saved network.

## Default D-Fly mappings

| D-Fly channel | Short press | Long press | Double press |
|---|---|---|---|
| CH1 | Shift up | Increase difficulty | Select/confirm |
| CH2 | Shift down | Decrease difficulty | Back/cancel |
| CH3 | Navigate up | Menu | Emote |
| CH4 | Navigate down | Screenshot | Lap |

These are firmware defaults; edit `CHANNEL_MAP` in the sketch to change them.
Short and double presses are sent as 80 ms momentary presses. Long presses
remain pressed until Di2 reports release.

The first Di2 indication after each connection establishes the channel baseline
without triggering actions. Later indications trigger actions only for channels
whose raw value changed; the serial monitor logs the complete indication.

## Build and flash

The project pins PlatformIO's Espressif32 platform to 6.10.0 (Arduino-ESP32
2.0.17) and NimBLE-Arduino to 1.4.2. Install PlatformIO Core, connect the
board by USB, then select the matching environment:

```sh
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor -e esp32dev
```

For ESP32-S3 use `esp32-s3-devkitc-1`; for ESP32-C3 use
`esp32-c3-devkitm-1`. The environments pin the same framework and library
versions.

## Limitations

- This implementation has not been tested with physical Di2 or trainer
  hardware. A successful firmware build does not verify BLE pairing, D-Fly
  indications, Wi-Fi, mDNS discovery, or trainer-app behavior.
- The bridge requires an indication-capable Di2 button characteristic and
  reports itself ready only after the indication subscription succeeds.
- It ignores app-to-device haptic and app-information messages.
- The open setup network allows nearby devices to join while provisioning.
  The local setup page uses HTTP, so configure it only while connected to the
  bridge's setup network.
