# Sensors I2C | WLED usermod (HTU21D + BMP180 + BH1750FVI)
**Author:** Austin St. Aubin <austinsaintaubin@gmail.com> | **License:** MIT

A community [WLED](https://github.com/wled/WLED) usermod for the common 3-in-1 I²C
environmental breakout. It reports temperature, humidity, pressure and ambient light to
WLED's Info page / `/json` and (optionally) over MQTT / Home Assistant, and can use the
light sensor to **drive WLED's overall brightness** from ambient light.

| Sensor       | Measures                         | Default I²C address |
|--------------|----------------------------------|---------------------|
| **HTU21D**   | Temperature, Humidity            | `0x40`              |
| **BMP180**   | Temperature (fallback), Pressure | `0x77` (BMP085-compatible) |
| **BH1750FVI**| Ambient light (illuminance)      | `0x23`              |

The three addresses don't collide, so the whole module sits on one bus.

> **Note:** this usermod (code, settings UI, and docs) was developed with **AI assistance**
> and validated by building against WLED. Review before use and verify on your own hardware.

> **Community usermod — use at your own risk.** This is a third-party usermod and is **not**
> reviewed, tested, endorsed, or supported by the WLED project. Usermods compile into your
> firmware with full access to your device. Read the source before flashing.

## Features

- Reads all three sensors over WLED's **globally configured I²C pins** (`i2c_sda` / `i2c_scl`).
- Shows every reading on the WLED **Info page** and exposes them in **`/json/info`**.
- Publishes readings over **MQTT** with optional **Home Assistant auto-discovery**
  (entirely optional — the mod also runs fine with MQTT disabled).
- Uses the **light sensor to drive overall LED brightness** with a perceptual
  (logarithmic) lux→brightness map, smoothing, and a relative manual-offset.
- A missing chip only disables that one sensor; the others keep working.
- **Self-contained** — makes no changes to `wled00/` and uses a local usermod id, so it
  drops in cleanly as an out-of-tree module.

## Hardware / wiring

1. Wire the module's `SDA`/`SCL` to your ESP32's I²C pins, plus `3V3` and `GND`.
2. In WLED, set the **I²C GPIOs** under *Config → LED Preferences* (or via the
   `I2CSDAPIN` / `I2CSCLPIN` build flags). The usermod refuses to start if I²C
   pins are not configured.

> Targeted at **ESP32**.

## Install / Build

This is an **out-of-tree** usermod, consumed via WLED's git-URL `custom_usermods` mechanism —
you don't copy it into the WLED source tree. See the WLED docs:
[Writing a usermod → Share it via git URL](https://kno.wled.ge/advanced/custom-features/#writing-a-usermod).

1. Get the [WLED](https://github.com/wled/WLED) source.
2. In a `platformio_override.ini` at the WLED repo root, reference this repo by URL in your build
   environment's `custom_usermods`:
   ```ini
   custom_usermods = https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c.git#main
   ```
   PlatformIO fetches it automatically — no manual copy and no git submodule needed. The `wled-`
   library name is auto-recognized as a usermod. Pin a release with `#v1.0.0` instead of `#main`
   if you prefer a fixed version. For local development you can instead point at a checkout:
   `custom_usermods = symlink:///absolute/path/to/wled-usermod-sensors-i2c`.

   To combine with other usermods, use the **multiline** form (one entry per indented line) —
   mixing a bare name and a URL on a single line breaks parsing:
   ```ini
   custom_usermods =
     https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c.git#main
     four_line_display_ALT
   ```
3. Build & flash for your ESP32.

The required sensor libraries (BH1750, Adafruit HTU21DF, Adafruit BMP085 Unified,
Adafruit Unified Sensor) are listed as `dependencies` in `library.json` and installed
**automatically** by PlatformIO — no manual `lib_deps`.

A ready-to-copy [`platformio_override.sample.ini`](examples/platformio_override.sample.ini) is included
(the git-URL `custom_usermods` line, the required I²C pin flags, OTA upload env, and size-trim
flags) — copy it to the WLED repo root as `platformio_override.ini` and adjust.

## Settings (Config → Usermods → "Sensors I2C")

The top of the section shows a **live readings table** (Temperature / Humidity / Pressure /
Illuminance / Auto-Brightness) with a **↻ Refresh readings** button that re-fetches current
values from `/json/info` — so you can check the sensors without leaving the settings page.

**Sensors**

| Setting                    | Default | Notes |
|----------------------------|---------|-------|
| Read Interval              | 5 s     | How often sensors are sampled / published |
| Temperature Unit           | Celsius | °C or °F (display + MQTT) |
| Decimals                   | 1       | Rounding for temp / humidity / pressure (0–3) |
| Publish Changes Only       | on      | Only publish a value over MQTT when it changes |
| Home Assistant Discovery   | off     | Publish HA MQTT discovery configs |

**Auto Brightness**

| Setting               | Default | Notes |
|-----------------------|---------|-------|
| Enabled               | off     | Master switch for ambient-light brightness control |
| Lux Min               | 1       | Lux value mapped to *Min Brightness* |
| Lux Max               | 1000    | Lux value mapped to *Max Brightness* |
| Min Brightness        | 5       | Brightness at/below Lux Min (0–255) |
| Max Brightness        | 255     | Brightness at/above Lux Max (0–255) |
| Smoothing             | 70 %    | Exponential smoothing (0 = instant, higher = smoother) |
| Update Interval       | 2 s     | How often brightness is recomputed |
| Allow Manual Offset   | on      | See "Manual adjustments" below |
| Reset Offset          | —       | Tick + Save once to clear the current manual offset |

## Auto-brightness behaviour

Brightness is derived from the BH1750 lux reading using a **logarithmic** map,
which matches how the eye perceives the very wide ambient range:

```
target = map( log10(lux) , log10(luxMin) , log10(luxMax) , briMin , briMax )
```

The target is **exponentially smoothed** to avoid flicker, then applied to WLED's
global brightness via `stateUpdated(CALL_MODE_NO_NOTIFY)`. This keeps the active
preset / effect / colors intact — only overall brightness changes — and does not
broadcast to sync peers.

### Manual adjustments (relative offset)

With *Allow Manual Offset* on, if you manually change brightness (UI, app, etc.)
the difference from the current auto value is captured as an **offset** and added
to all future auto values, so the system keeps tracking ambient light but shifted
to your preference. Clear it with *Reset Offset* (or the JSON command below).

## External access (Home Assistant & similar)

- **Info page / `/json/info`** — readings appear (grouped, each prefixed with `Sensor `) as
  `Sensor Temperature`, `Sensor Humidity`, `Sensor Pressure`, `Sensor Illuminance`, and a
  `Sensor Auto-Brightness` status line.
- **MQTT** — published under your WLED device topic:

  ```
  <mqttDeviceTopic>/temperature
  <mqttDeviceTopic>/humidity
  <mqttDeviceTopic>/pressure
  <mqttDeviceTopic>/illuminance
  ```

- **Home Assistant** — with *Home Assistant Discovery* on and MQTT connected, four
  sensor entities (with proper device classes/units) auto-register under the WLED
  device.
- **`/json/state`** — exposes `{"SensorsI2C":{"autoBri":<bool>,"offset":<int>}}`.

## Controlling auto-brightness from a preset / API

The usermod accepts commands in the JSON state under the `SensorsI2C` key, which
is also processed when a **preset is applied**. Create a preset of type
**"API command"** (or send the JSON to `/json/state`):

```json
{ "SensorsI2C": { "autoBri": true } }                    // re-engage automatic control
{ "SensorsI2C": { "resetOffset": true } }                // clear the manual offset
{ "SensorsI2C": { "autoBri": true, "resetOffset": true } }// both at once
{ "SensorsI2C": { "autoBri": false } }                   // hand brightness back to manual
```

This makes it easy to bind a button / schedule / scene to "return to automatic
brightness".

## Notes / limitations

- I²C addresses are fixed defaults (no per-sensor address setting yet).
- ESP32 only (no ESP8266-specific tuning).
- Uses `USERMOD_ID_SENSORS_I2C` defined locally in the `.cpp` (defaults to `900`) so the
  module needs no edit to `wled00/const.h`.

## License

[MIT](LICENSE) © 2026 Austin St. Aubin
