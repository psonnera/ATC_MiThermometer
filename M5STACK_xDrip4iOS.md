# Glucose on the thermometer display with xDrip4iOS

The `iPhone` branch adds an emulation of the **M5Stack** Bluetooth peripheral
that xDrip4iOS ([xdripswift](https://github.com/JohanDegraeve/xdripswift))
already supports. xDrip4iOS connects to the thermometer as if it were an
[M5_NightscoutMon](https://github.com/JohanDegraeve/M5_NightscoutMon) and
pushes every new glucose reading to the display. No hub, no cloud, no extra app.

This is the iOS counterpart of the Android *WatchDrip+ "Thermometers (Telink)"*
feature, and the display uses the same conventions.

Source: `src/m5stack.c`, enabled with `USE_M5STACK_EMU` (on by default for
`LYWSD03MMC` and `MJWSD06MMC` builds on this branch).

## Setup

1. Flash the firmware built from this branch (`ATC_vNN.bin` or `MJ6_vNN.bin`)
   with [TelinkMiFlasher](https://pvvx.github.io/ATC_MiThermometer/TelinkMiFlasher.html).
2. The default device name becomes `M5Stack_xxxx`. xDrip4iOS pairs with the
   first device whose name **contains "M5Stack"**, so if you set a custom name
   in TelinkMiFlasher keep "M5Stack" in it. After the first pairing the app
   remembers the device and the name no longer matters.
3. In xDrip4iOS: *Settings → Bluetooth devices → + → M5Stack*, then *Scan*.
   Leave the *BLE password* field empty: the thermometer generates one, sends it
   to the app and stores it in flash.
4. Keep the thermometer in BLE range of the iPhone. xDrip4iOS keeps a
   permanent connection and writes each new reading (about every 5 minutes).

## What the display shows

| Field | Meaning |
|---|---|
| Big number | Glucose. mg/dL as an integer, mmol/L with one decimal (unit follows the xDrip4iOS setting). |
| Small number | Change since the previous reading, without sign (mg/dL, or tenths of mmol/L). |
| Smiley | `(^_^)` in range 70–180 mg/dL, `(-^-)` high or low, `(ooo)` reading older than 15 minutes. |
| Trend bars (next to °C/°F) | top = rising, top+middle = slowly rising, middle = flat, middle+bottom = slowly falling, bottom = falling. Hidden when the reading is old. |
| Bluetooth symbol | Connected to the phone. |

After 60 minutes without a new reading the display returns to temperature and
humidity. The clock of the thermometer is set from the phone at each connection.

The in-range limits can be changed at build time with `M5_BG_LOW_MGDL` and
`M5_BG_HIGH_MGDL` (`src/m5stack.c`).

## Password

The first time the app asks for a password the thermometer generates a random
10-character one, stores it in flash and sends it to the app. If xDrip4iOS is
reinstalled, leave the password empty again: unlike the original M5Stack, the
thermometer sends the stored password back instead of requiring a reset. If a
password is typed in xDrip4iOS before the first connection, the thermometer
adopts it.

## Protocol notes

* Service `AF6E5F78-706A-43FB-B1F4-C27D7D5C762F`, characteristic
  `6D810E9F-0983-4030-BDA7-C7C9A6A19C1C` (write without response, notify).
* Text messages use 20-byte packets `[opcode][packet n][total][text]`.
* Implemented: password (0x09/0x0A → 0x0E/0x0B/0x0C), unit 0x03, time 0x12 and
  offset 0x14, reading 0x10 `"<mg/dL> <epoch>"`, slope 0x13, battery 0x21 → 0x20,
  request-all-parameters 0x16 after the first time sync, heartbeat 0x21 once a
  minute when the reading is older than 5 minutes.
* Colors, rotation, brightness and power-off opcodes are accepted and ignored.

## Limitations

* Only LYWSD03MMC and MJWSD06MMC have the display mapping. Other LCD models need
  a `show_big_number()` in their `lcd_*.c`; E-ink models are not suitable
  (slow, wearing refresh every 5 minutes).
* The first pairing must be done with xDrip4iOS in the foreground (the app scans
  without a service filter, as it does for a real M5Stack).
* A permanent connection shortens battery life compared with beacon-only use.
