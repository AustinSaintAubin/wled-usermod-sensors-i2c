# Sensors I2C | WLED usermod (HTU21D + BMP180 + BH1750FVI)
**Author:** Austin St. Aubin <austinsaintaubin@gmail.com> | **License:** MIT
[![build](https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c/actions/workflows/build.yml/badge.svg)](https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c/actions/workflows/build.yml)

A community [WLED](https://github.com/wled/WLED) usermod for the common 3-in-1 I²C
environmental breakout. It reports temperature, humidity, pressure and ambient light to
WLED's Info page / `/json` and (optionally) over MQTT / Home Assistant.

> **v2.0.0 — auto-brightness moved:** the ambient-light brightness control that used to
> live in this usermod is now its own usermod,
> [wled-usermod-auto-brightness](https://github.com/AustinSaintAubin/wled-usermod-auto-brightness)
> (which adds VEML7700 and analog photocell support). Both usermods can be installed
> together and can share the same BH1750 — see [Install / Build](#install--build).
> Presets/automations change from `{"SensorsI2C":{"autoBri":…}}` to `{"AutoBri":{"on":…}}`;
> the MQTT `<deviceTopic>/autobri` topics are now served by the new usermod.

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
- **Derived values** from the raw readings: absolute humidity, dew point, heat index
  (temp + humidity) and sea-level pressure + estimated altitude (pressure).
- A missing chip only disables that one sensor; the others keep working, and a sensor that
  drops off the bus is **re-probed automatically** (every 30 s) and recovers.
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

   To combine with other usermods — e.g. the companion
   [wled-usermod-auto-brightness](https://github.com/AustinSaintAubin/wled-usermod-auto-brightness) —
   use the **multiline** form (one entry per indented line); mixing a bare name and a URL on a
   single line breaks parsing:
   ```ini
   custom_usermods =
     https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c.git#main
     https://github.com/AustinSaintAubin/wled-usermod-auto-brightness.git#main
   ```
3. Build & flash for your ESP32.

The required sensor libraries (BH1750, Adafruit HTU21DF, Adafruit BMP085 Unified,
Adafruit Unified Sensor) are listed as `dependencies` in `library.json` and installed
**automatically** by PlatformIO — no manual `lib_deps`.

A ready-to-copy [`platformio_override.sample.ini`](examples/platformio_override.sample.ini) is included
(the git-URL `custom_usermods` line, the required I²C pin flags, OTA upload env, and size-trim
flags) — copy it to the WLED repo root as `platformio_override.ini` and adjust.

## Settings (Config → Usermods → "Sensors I2C")

The section starts with the master **Enabled** checkbox (hint: needs the global I²C pins
configured at the top of the Usermods settings page). Directly below it, a **Live Readings
table** (Temperature / Humidity / Pressure / Illuminance / …) with a
**↻ Refresh** button takes a **fresh sensor reading** (via the JSON `read` command below)
and re-fetches the values from `/json/info` — so you can check the sensors without leaving
the settings page. When there's nothing to show, the table says *(usermod disabled)* or
*(no readings — check sensor wiring)* instead of sitting empty.

**Sensors**

| Setting                    | Default | Notes |
|----------------------------|---------|-------|
| Read Interval              | 5 s     | How often sensors are sampled / published |
| Temperature Unit           | Celsius | °C or °F (display + MQTT); temperatures show both units |
| Decimals                   | 1       | Rounding for temp / humidity / pressure (0–3) |
| BH1750 Address             | 0x23    | Light-sensor I²C address (`0x23`, or `0x5C` if its ADDR pin is high) |
| Station Altitude           | 0 m     | Your altitude above sea level, used for sea-level pressure |

**Readings** — a checkbox per reading (all **on** by default) to individually show/publish or
hide each one: Temperature, Humidity, Absolute Humidity, Dew Point, Heat Index, Pressure,
Sea-Level Pressure, Altitude, Illuminance. Turning one off removes it from the info page, MQTT,
and Home Assistant discovery.

**MQTT & Home Assistant** (the page header displays as "MQTT Home Assistant" — WLED strips
punctuation from group titles; upgrading from ≤ v1.0.14 migrates both values automatically
from their old spot under *Sensors* on first boot)

| Setting                  | Default | Notes |
|--------------------------|---------|-------|
| Publish Changes Only     | on      | Only publish a value over MQTT when it changes (a full refresh still goes out every 5 min so Home Assistant entities never expire) |
| Home Assistant Discovery | off     | Publish HA MQTT discovery configs |

## External access (Home Assistant & similar)

- **Info page / `/json/info`** — readings appear (grouped, each prefixed with `Sensor `) as
  `Sensor Temperature`, `Sensor Humidity`, `Sensor Pressure`, `Sensor Illuminance`, and the
  derived `Sensor Absolute Humidity` / `Dew Point` / `Heat Index` / `Sea-Level Pressure` /
  `Altitude` (each removable via its **Readings** toggle).
- **MQTT** — published under your WLED device topic:

  ```
  <mqttDeviceTopic>/temperature
  <mqttDeviceTopic>/humidity
  <mqttDeviceTopic>/pressure
  <mqttDeviceTopic>/illuminance
  <mqttDeviceTopic>/absolute_humidity   (derived)
  <mqttDeviceTopic>/dew_point           (derived)
  <mqttDeviceTopic>/heat_index          (derived)
  <mqttDeviceTopic>/sea_level_pressure  (derived)
  <mqttDeviceTopic>/altitude            (derived)
  ```

- **Home Assistant** — with *Home Assistant Discovery* on and MQTT connected, the sensor
  entities (with proper device classes/units) auto-register under the WLED device. All
  entities use WLED's `/status` LWT as their availability topic, so they show
  *unavailable* whenever the device itself is offline.

## JSON commands (preset / API)

The usermod accepts commands in the JSON state under the `SensorsI2C` key, which
is also processed when a **preset is applied**. Create a preset of type
**"API command"** (or send the JSON to `/json/state`):

```json
{ "SensorsI2C": { "read": true } }   // take a fresh sensor reading now
```

The former `autoBri` / `resetOffset` commands moved to the
[auto-brightness usermod](https://github.com/AustinSaintAubin/wled-usermod-auto-brightness)
as `{"AutoBri":{"on":…}}` / `{"AutoBri":{"resetOffset":true}}`.

## Notes / limitations

- BH1750 address is selectable (`0x23`/`0x5C`); HTU21D (`0x40`) and BMP180 (`0x77`) are fixed.
- ESP32 only (no ESP8266-specific tuning).
- Uses `USERMOD_ID_SENSORS_I2C` defined locally in the `.cpp` (defaults to `900`) so the
  module needs no edit to `wled00/const.h`.

## License

[MIT](LICENSE) © 2026 Austin St. Aubin
