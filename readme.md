# Sensors I2C | WLED usermod (HTU21D + BMP180 + BH1750FVI)
**Author:** Austin St. Aubin <austinsaintaubin@gmail.com> | **License:** MIT
[![build](https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c/actions/workflows/build.yml/badge.svg)](https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c/actions/workflows/build.yml)

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
- **Derived values** from the raw readings: absolute humidity, dew point, heat index
  (temp + humidity) and sea-level pressure + estimated altitude (pressure).
- Uses the **light sensor to drive overall LED brightness** with a perceptual
  (logarithmic) lux→brightness map, smoothing, and a relative manual-offset.
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

The section starts with the master **Enabled** checkbox (hint: needs the global I²C pins
configured at the top of the Usermods settings page). Directly below it, a **Live Readings
table** (Temperature / Humidity / Pressure / Illuminance / Auto-Brightness) with a
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
| Reset Offset          | button  | Instantly clears the manual offset (sends the `resetOffset` JSON command — no Save needed) |

**Off When Dark** (own sub-section; the two lux fields render as a small table)

| Setting       | Default | Notes |
|---------------|---------|-------|
| Enabled       | off     | Master switch: turn the LEDs fully off in darkness |
| Off Below Lux | 5       | Lux below which the LEDs switch off |
| On Above Lux  | 20      | Lux at/above which normal auto-brightness resumes; kept ≥ *Off Below Lux* (set higher for hysteresis) |

**MQTT & Home Assistant** (the page header displays as "MQTT Home Assistant" — WLED strips
punctuation from group titles; upgrading from ≤ v1.0.14 migrates both values automatically
from their old spot under *Sensors* on first boot)

| Setting                  | Default | Notes |
|--------------------------|---------|-------|
| Publish Changes Only     | on      | Only publish a value over MQTT when it changes (a full refresh still goes out every 5 min so Home Assistant entities never expire) |
| Home Assistant Discovery | off     | Publish HA MQTT discovery configs |

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

Turning the LEDs **off** pauses auto-brightness (it will never switch them back on);
control resumes automatically the next time you turn them on. Nightlight fades are
likewise left alone.

With **Off When Dark** enabled, the strip is switched fully off when ambient light
drops below **Off Below Lux** — for rooms where even *Min Brightness* would glow —
and normal control resumes once lux reaches **On Above Lux**. Set *On Above Lux*
higher than *Off Below Lux* to add hysteresis so the lights don't flap around a
single boundary (equal values give a plain threshold; the pair is auto-corrected so
it can never invert).

While it's dark, **darkness wins**: adjusting brightness only updates your manual
offset and the strip switches back off on the next update. The one exception is
explicitly turning the lights **on** from the dark-off state — that is honored
(lights stay on) until the room reaches *On Above Lux*, after which darkness can win
again. The current state is visible on the info page as `dark-off` /
`dark-off (overridden)`. Note that normal auto-brightness never dims to 0 on its
own — only *Off When Dark* switches the strip off.

### Manual adjustments (relative offset)

With *Allow Manual Offset* on, if you manually change brightness (UI, app, etc.)
the difference from the current auto value is captured as an **offset** and added
to all future auto values, so the system keeps tracking ambient light but shifted
to your preference. Clear it with *Reset Offset* (or the JSON command below).

## External access (Home Assistant & similar)

- **Info page / `/json/info`** — readings appear (grouped, each prefixed with `Sensor `) as
  `Sensor Temperature`, `Sensor Humidity`, `Sensor Pressure`, `Sensor Illuminance`, the
  derived `Sensor Absolute Humidity` / `Dew Point` / `Heat Index` / `Sea-Level Pressure` /
  `Altitude` (each removable via its **Readings** toggle), and a `Sensor Auto-Brightness` status line.
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
  <mqttDeviceTopic>/autobri             (auto-brightness state, ON/OFF, retained)
  <mqttDeviceTopic>/autobri/set         (command topic: ON/OFF or 1/0)
  ```

- **Home Assistant** — with *Home Assistant Discovery* on and MQTT connected, the sensor
  entities (with proper device classes/units) auto-register under the WLED device, plus an
  **Auto Brightness switch** so ambient control can be toggled straight from HA. All
  entities use WLED's `/status` LWT as their availability topic, so they show
  *unavailable* whenever the device itself is offline.
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
{ "SensorsI2C": { "read": true } }                       // take a fresh sensor reading now
```

This makes it easy to bind a button / schedule / scene to "return to automatic
brightness".

## Notes / limitations

- BH1750 address is selectable (`0x23`/`0x5C`); HTU21D (`0x40`) and BMP180 (`0x77`) are fixed.
- ESP32 only (no ESP8266-specific tuning).
- Uses `USERMOD_ID_SENSORS_I2C` defined locally in the `.cpp` (defaults to `900`) so the
  module needs no edit to `wled00/const.h`.

## License

[MIT](LICENSE) © 2026 Austin St. Aubin
