# Di2 to OpenBikeControl bridge

## Experimental Zwift Ride branch

This branch replaces the two-button Click emulation with an **experimental,
unencrypted Zwift Ride** controller. The original Click branch is unchanged.
After flashing, remove the old Click pairing in Zwift and pair **Zwift Ride**
under **Controls**. Watch the serial monitor for `central connected`,
`negotiated MTU`, `handshake`, and `GET object` messages.

The protocol implementation follows the wire format of the
[Zwift Joypad reference](https://github.com/rainyx/zwift-joypad-android).
It advertises a Ride user-input capability, answers device/input/battery queries,
and sends digital buttons and analog steering paddles with a one-second keepalive.
It does **not** implement encryption. Successful builds do not verify
compatibility with Zwift; this branch is for manual testing.

| D-Fly channel | Short press in Zwift | Long press in Zwift | Double press in Zwift |
|---|---|---|---|
| CH1 | Shift down | Y (Use power-up) | B (Back/menu) |
| CH2 | Shift up | Z (Ride On; hold for bomb) | A (Select) |
| CH3 | D-pad left (junction left) | Steer left | U-turn (2-second D-pad-down hold) |
| CH4 | D-pad right (junction right) | Steer right | Action bar |

These are Zwift's contextual Ride bindings, not arbitrary keyboard commands.
CH3/CH4 short presses select junction turns, not analog road steering.
Long presses keep the corresponding Ride key held until Di2 reports release.
CH3/CH4 long presses send full paddle deflection: left horizontal
axis -100 or right horizontal axis +100, matching the reference app's
steering (not braking) direction. Release returns that paddle to zero;
Di2 disconnect resets both paddles. Simultaneous holds report both paddles.
Steering strength and Zwift's response still need manual testing.
A double press emits a single momentary Ride button event, except CH3,
which holds D-pad down for two seconds to trigger U-turn.
CH4 uses Action bar instead of Coffee Break, for which there is no verified
direct Ride button mapping.

The existing OBC GATT service and action IDs are retained, but its service UUID
is no longer advertised: Ride discovery fills the primary advertising packet.
Apps that require OBC UUID-based discovery may not find this experimental build.
Short/double presses retain their 80 ms duration except the Ride U-turn hold.
Haptics are logged but not executed.
Only one central is intended at a time; advertising resumes after disconnect.
Device information requires an exchanged MTU of at least 42; an insufficient
MTU is logged and disconnected rather than transmitting a truncated response.

Arduino sketch for an ESP32 that reads Shimano Di2 D-Fly button indications
over BLE and sends Zwift Ride and OpenBikeControl button events over BLE.

## Hardware and setup

- ESP32 classic, ESP32-S3, or ESP32-C3 board with BLE.
- Shimano Di2 with an EW-WU111 wireless unit or built-in BLE.
- Pair/bond the Di2 unit with the ESP32 before using it. Assign the physical
  buttons to D-Fly channels 1-4 in Shimano E-Tube Project.

## Default D-Fly mappings

| D-Fly channel | Short press | Long press | Double press |
|---|---|---|---|
| CH1 | Shift down | Power-up 1 | Back/cancel |
| CH2 | Shift up | Emote | Select/confirm |
| CH3 | Navigate left | Steer left | U-turn |
| CH4 | Navigate right | Steer right | Navigate up (Action bar in Ride) |

These are firmware defaults; edit `CHANNEL_MAP` in the sketch to change them.
Short and double presses are sent as 80 ms momentary presses over OBC.
The Ride output extends U-turn to a two-second hold. Long presses
remain pressed until Di2 reports release.

The first Di2 indication after each connection establishes the channel baseline
without triggering actions. Later indications trigger actions only for channels
whose raw value changed. An idle or unmapped `0xF0` value releases a held
button and re-arms the channel. The serial monitor logs the complete indication.

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
  indications or trainer-app behavior.
- The bridge requires an indication-capable Di2 button characteristic and
  reports itself ready only after the indication subscription succeeds.
- It ignores app-to-device haptic and app-information messages.
