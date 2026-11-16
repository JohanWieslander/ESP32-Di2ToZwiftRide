# Di2 to Zwift Ride BLE bridge

ESP32 firmware that reads Shimano Di2 D-Fly button indications over Bluetooth
Low Energy and acts as a Zwift Ride controller. It supports virtual shifting,
junction selection, steering, power-ups, Ride Ons and menu controls.
Buttons are configured through an open Wi-Fi captive portal; no network setup is required.

## Hardware and setup

- ESP32 classic, ESP32-S3, or ESP32-C3 board with BLE.
- Shimano Di2 with an EW-WU111 wireless unit or built-in BLE.
- Assign the physical buttons to D-Fly channels 1-4 in Shimano E-Tube Project.

Flash the firmware and power on Di2. The bridge scans for the Shimano service,
connects, establishes a secure/bonded connection and subscribes to D-Fly
indications. In Zwift, pair **Zwift Ride** under **Controls**.
The ESP32 maintains the Di2 connection and the Zwift connection simultaneously.

## Button layout

| D-Fly channel | Single press | Long press | Double press |
|---|---|---|---|
| CH1 | Shift down | Y - Use power-up | B - Back/menu |
| CH2 | Shift up | Z - Ride On (hold for bomb) | A - Select |
| CH3 | D-pad left - Junction left | Steer left | U-turn |
| CH4 | D-pad right - Junction right | Steer right | Action bar |

These are Zwift's contextual Ride bindings, not keyboard shortcuts.
To change the layout, join the open (passwordless) Wi-Fi network
**Di2-Zwift-Bridge**. The captive portal opens automatically (or browse to
`http://192.168.4.1`) with a simple page for choosing the action of each
CH1-4 single, long and double press. Settings are saved to flash and apply
immediately; "Save and close portal" saves and shuts the portal down, and
"Restore defaults" resets the table above. The portal closes 30 seconds after boot if nobody has joined (reboot to
reopen it). The network has no password, so anyone in Wi-Fi range can change
the mapping while it is open.

Single and double presses send an 80 ms button press. U-turn sends a two-second
D-pad-down hold on the Ride output. Long presses stay active until Di2 reports
release.

Steering uses the Ride analog paddles: CH3 holds the left horizontal axis at
-100, and CH4 holds the right horizontal axis at +100. Releasing a button
returns its paddle to zero. Both paddles can be held simultaneously.
Di2 disconnect releases all buttons and resets steering to neutral.

## BLE behavior

The bridge advertises as **Zwift Ride**, using the unencrypted Ride protocol
version 2.4. It answers device information, input configuration and battery
queries, and sends button/paddle reports with a one-second keepalive.
Battery queries report a fixed 100%, not a measured Di2 battery level.
Advertising resumes after the central disconnects; one central is intended
at a time.

The Di2 input format is `[counter, ch1, ch2, ch3, ch4]`. The first indication
after connection establishes a baseline without triggering actions. Subsequent
actions are dispatched only when a raw channel byte changes. A transition to
idle/unmapped `0xF0` releases a held action and re-arms the channel.

## Build and flash

The project pins PlatformIO's Espressif32 platform to 6.10.0 (Arduino-ESP32
2.0.17) and NimBLE-Arduino to 1.4.2. Install PlatformIO Core, connect the
board by USB, then select the matching environment:

| Board | Environment |
|---|---|
| ESP32 classic | `esp32dev` |
| ESP32-S3 | `esp32-s3-devkitc-1` |
| ESP32-C3 | `esp32-c3-devkitm-1` |

```sh
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor -e esp32dev
```

Replace `esp32dev` with your board's environment. Run `pio run` to build
all three environments.

## Diagnostics and limitations

The serial monitor runs at 115200 baud. It logs Di2 connection/subscription
status, complete input packets, mapped button events, the Ride handshake,
negotiated MTU and object queries.

- Shifting and steering have been reported working with Zwift on hardware.
  The complete current layout and every supported board are not yet verified
  on hardware.
- Di2 must provide an indication-capable button characteristic; the bridge
  reports readiness only after subscription succeeds.
- The Ride protocol is unencrypted; encrypted controller modes are not
  implemented.
- Device information requires a negotiated MTU of at least 42. The firmware
  requests 185 and disconnects with a log message if a response cannot fit,
  rather than sending truncated data.
- Ride haptic/game-state commands are logged but not acted on.
- There are no direct Ride mappings for Screenshot, Lap or Coffee Break.

The Ride wire format follows the
[Zwift Joypad reference implementation](https://github.com/rainyx/zwift-joypad-android).
Di2 input handling is based on the
[Di2 media controls service](https://github.com/NitorCreations/shimano-di2-media-controls).
