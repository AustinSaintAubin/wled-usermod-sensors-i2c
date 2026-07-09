// SPDX-License-Identifier: MIT
// usermod_v2_sensors_i2c — MIT © Austin St. Aubin
#include "wled.h"
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_HTU21DF.h>
#include <Adafruit_BMP085_U.h>

// Combined I2C environmental sensor usermod for the common 3-in-1 breakout:
//   - HTU21D     : temperature + humidity      (I2C 0x40)
//   - BMP180     : temperature + pressure       (I2C 0x77, BMP085-compatible)
//   - BH1750FVI  : ambient light / illuminance  (I2C 0x23)
//
// Uses WLED's globally configured I2C pins (i2c_sda / i2c_scl). Wire.begin() is
// already called by the WLED core, so it is NOT called here.
//
// The BH1750 lux reading can drive WLED's overall brightness (see Auto Brightness).
// MQTT / Home Assistant publishing is optional and only compiled when MQTT is enabled.

// Local usermod id so this mod stays fully self-contained (no edit to const.h).
#ifndef USERMOD_ID_SENSORS_I2C
#define USERMOD_ID_SENSORS_I2C 900
#endif

#define SENSORS_I2C_VERSION "1.0.10"  // keep in sync with library.json (CI-checked)

#define SENSORS_I2C_PROBE_INTERVAL_MS 30000UL   // re-probe cadence for missing sensors
#define SENSORS_I2C_MQTT_HEARTBEAT_MS 300000UL  // forced full republish (keeps HA alive)
#define SENSORS_I2C_HA_EXPIRE_AFTER_S 1800      // HA marks entity unavailable after this

class UsermodSensorsI2C : public Usermod
{
private:
  // ------- master -------
  bool enabled = true;
  bool initDone = false;

  // ------- sensor settings -------
  uint16_t readInterval = 5;     // seconds between sensor reads
  uint8_t  tempUnit = 0;         // 0 = Celsius, 1 = Fahrenheit
  uint8_t  decimals = 1;         // decimal places for temp/humidity/pressure
  bool     publishChangesOnly = true;
  bool     haDiscovery = false;  // publish Home Assistant MQTT discovery
  // per-reading enable toggles (all default on) — gate the info page + MQTT/HA
  bool enTemp = true, enHum = true, enPress = true, enLux = true;              // raw
  bool enAbsHum = true, enDew = true, enHeat = true, enSLP = true, enAlt = true; // derived
  int16_t  stationAltitude = 0;  // meters above sea level, for sea-level pressure
  uint8_t  bhAddress = 0x23;     // BH1750 I2C address (0x23 default, 0x5C if ADDR high)

  // ------- auto-brightness settings -------
  bool     autoBriEnabled = false;
  uint16_t luxMin = 1;           // lux mapped to Min Brightness
  uint16_t luxMax = 1000;        // lux mapped to Max Brightness
  uint8_t  briMin = 5;           // brightness at/below luxMin
  uint8_t  briMax = 255;         // brightness at/above luxMax
  uint8_t  smoothing = 70;       // EMA smoothing percent (0 = off, higher = smoother)
  uint8_t  briUpdateInterval = 2;// seconds between brightness updates
  uint16_t offBelowLux = 0;      // turn strip fully off below this lux (0 = never)
  uint16_t onAboveLux  = 0;      // turn back on at/above this lux (clamped >= offBelowLux)
  bool     allowManualOffset = true;
  bool     resetOffset = false;  // momentary; cleared on save

  // ------- runtime sensor state -------
  BH1750                    lightMeter;
  Adafruit_HTU21DF          htu = Adafruit_HTU21DF();
  Adafruit_BMP085_Unified   bmp = Adafruit_BMP085_Unified(10085);
  bool bhFound = false, htuFound = false, bmpFound = false;
  uint8_t htuFails = 0, bhFails = 0, bmpFails = 0; // consecutive read failures -> mark lost + re-probe
  unsigned long lastProbeTime = 0;     // periodic re-probe of missing sensors

  float curTempC   = NAN;   // chosen temperature source, always stored in Celsius
  float curHum     = NAN;   // %
  float curPressure= NAN;   // hPa
  float curLux     = -1;    // lx

  // derived values (computed from the above; all in base SI-ish units)
  float curAbsHum   = NAN;  // g/m³   (from temp + humidity)
  float curDewC     = NAN;  // °C     (from temp + humidity)
  float curHeatC    = NAN;  // °C     (from temp + humidity)
  float curSeaLevel = NAN;  // hPa    (pressure adjusted to sea level)
  float curAltitude = NAN;  // m      (estimated from pressure, standard atmosphere)

  // change tracking for "publish only on change"
  float lastTempC = NAN, lastHum = NAN, lastPressure = NAN, lastLux = NAN;
  float lastAbsHum = NAN, lastDewC = NAN, lastHeatC = NAN, lastSeaLevel = NAN, lastAltitude = NAN;

  unsigned long lastReadTime = 0;
  unsigned long lastBriTime  = 0;
  bool readRequested = false;    // JSON "read" command -> fresh sample on next loop()

  // ------- auto-brightness runtime -------
  float   briSmoothed   = NAN;   // EMA state (mapped brightness, before offset)
  int     lastTargetNoOffset = -1;
  int     userBriOffset = 0;     // relative offset captured from manual changes
  uint8_t lastAutoBri   = 0;     // last brightness value we applied
  bool    briBaselineSet = false;// have we applied auto brightness at least once
  bool    applyingAuto  = false; // guards onStateChange against our own writes
  bool    offPause      = false; // strip turned off by user -> pause auto-bri until back on
  bool    autoOffActive = false; // dark-off engaged (Off Below Lux); keeps monitoring lux

  bool discoveryDirty = true;    // (re)publish/clear HA discovery on next connected loop
  bool autoBriPubDirty = true;   // publish auto-brightness switch state on next connected loop
#ifndef WLED_DISABLE_MQTT
  unsigned long lastForcePublish = 0; // heartbeat timer (SENSORS_I2C_MQTT_HEARTBEAT_MS)
#endif

  // strings to reduce flash usage
  static const char _name[];
  static const char _enabled[];
  static const char _grpSensors[];
  static const char _grpReadings[];
  static const char _grpAutoBri[];
  static const char _stateKey[];

  // ---- helpers ----
  float roundDec(float v) {
    if (isnan(v)) return v;
    float f = powf(10, decimals);
    return roundf(v * f) / f;
  }

  float toDisplayTemp(float c) {
    if (isnan(c)) return c;
    return (tempUnit == 1) ? (c * 1.8f + 32.0f) : c;
  }

  const __FlashStringHelper* tempUnitStr() {
    return (tempUnit == 1) ? F("°F") : F("°C");
  }

  // Add a temperature reading to the info page in both °C and °F, ordered by the
  // configured primary unit (shared by Temperature, Dew Point, Heat Index).
  void addTempInfo(JsonObject &user, const __FlashStringHelper *label, float c) {
    JsonArray j = user.createNestedArray(label);
    if (isnan(c)) { j.add(F("-")); return; }
    float cc = roundDec(c), ff = roundDec(c * 1.8f + 32.0f);
    if (tempUnit == 1) { j.add(ff); j.add(String(F(" °F / ")) + String(cc, (unsigned)decimals) + F(" °C")); }
    else               { j.add(cc); j.add(String(F(" °C / ")) + String(ff, (unsigned)decimals) + F(" °F")); }
  }

  // NWS Rothfusz "feels like" heat index. Computed in °F, returned in °C.
  // Only meaningful when warm; below ~80 °F it just returns the air temperature.
  float heatIndexC(float tC, float rh) {
    float tF = tC * 1.8f + 32.0f;
    if (tF < 80.0f) return tC;
    float hi = -42.379f + 2.04901523f * tF + 10.14333127f * rh
             - 0.22475541f * tF * rh - 0.00683783f * tF * tF - 0.05481717f * rh * rh
             + 0.00122874f * tF * tF * rh + 0.00085282f * tF * rh * rh
             - 0.00000199f * tF * tF * rh * rh;
    if (rh < 13.0f && tF >= 80.0f && tF <= 112.0f)
      hi -= ((13.0f - rh) / 4.0f) * sqrtf((17.0f - fabsf(tF - 95.0f)) / 17.0f);
    else if (rh > 85.0f && tF >= 80.0f && tF <= 87.0f)
      hi += ((rh - 85.0f) / 10.0f) * ((87.0f - tF) / 5.0f);
    return (hi - 32.0f) / 1.8f;
  }

  // Derive absolute humidity / dew point / heat index (need temp + humidity from
  // HTU21D) and sea-level pressure / altitude (need BMP180 pressure).
  void computeDerived() {
    if (htuFound && !isnan(curTempC) && !isnan(curHum)) {
      float T = curTempC, RH = curHum;
      curAbsHum = 6.112f * expf((17.67f * T) / (T + 243.5f)) * RH * 2.1674f / (273.15f + T);
      float g = logf(RH / 100.0f) + (17.62f * T) / (243.12f + T);
      curDewC = 243.12f * g / (17.62f - g);
      curHeatC = heatIndexC(T, RH);
    } else {
      curAbsHum = curDewC = curHeatC = NAN;
    }
    if (bmpFound && !isnan(curPressure)) {
      curSeaLevel = bmp.seaLevelForAltitude((float)stationAltitude, curPressure);
      curAltitude = bmp.pressureToAltitude(1013.25f, curPressure);
    } else {
      curSeaLevel = curAltitude = NAN;
    }
  }

  void readSensors() {
    if (htuFound) {
      float t = htu.readTemperature();
      float h = htu.readHumidity();
      if (isnan(t) || isnan(h)) {
        if (++htuFails >= 3) htuFound = false; // lost -> periodic re-probe recovers it
      } else {
        htuFails = 0;
        curTempC = t;   // HTU21D is the preferred temperature source
        curHum = h;
      }
    }
    if (bmpFound) {
      float p = NAN, t = NAN;
      bmp.getPressure(&p);           // Pa
      bmp.getTemperature(&t);        // °C
      // a BMP180 that drops off the bus yields garbage (not NAN); accept only a
      // plausible atmospheric pressure and mark the sensor lost after repeated bad reads
      if (!isnan(p) && p >= 30000.0f && p <= 110000.0f) {
        bmpFails = 0;
        curPressure = p / 100.0f;    // hPa
        if (!htuFound && !isnan(t)) curTempC = t; // fall back to BMP180 temperature
      } else {
        if (++bmpFails >= 3) bmpFound = false; // lost -> periodic re-probe recovers it
      }
    }
    readLux();

    computeDerived();

#ifndef WLED_DISABLE_MQTT
    publishSensors();
#endif
  }

  // Single lux read path shared by readSensors() and auto-brightness so both
  // feed the same failure counter / dropout recovery. True when curLux is fresh.
  bool readLux() {
    if (!bhFound) return false;
    float lux = lightMeter.readLightLevel();
    if (lux < 0) {                   // read error / not ready
      if (++bhFails >= 3) bhFound = false; // lost -> periodic re-probe recovers it
      return false;
    }
    bhFails = 0;
    curLux = lux;
    return true;
  }

  void updateAutoBrightness() {
    if (nightlightActive) return;             // never fight the nightlight
    if (bri == 0 && !autoOffActive) return;   // user powered off: paused until back on
    if (!readLux()) return;

    // optional dark-off: below "Off Below Lux" switch the strip off entirely; back
    // on at/above "On Above Lux" (user-set hysteresis; clamped >= off threshold so
    // the range can't invert). While engaged, a manual power-on is respected
    // (hands off) until the room is bright again.
    if (offBelowLux > 0) {
      if (autoOffActive) {
        if (curLux < (float)onAboveLux) return;           // still dark: stay off / hands off
        autoOffActive = false;                            // bright again: resume control
        briSmoothed = NAN;                                // jump straight to the mapped target
      } else if (curLux < (float)offBelowLux) {
        autoOffActive = true;
        if (bri > 0) {
          applyingAuto = true;
          bri = 0;
          lastAutoBri = 0;
          stateUpdated(CALL_MODE_NO_NOTIFY);
        }
        return;
      }
    }

    uint16_t lMin = max((uint16_t)1, luxMin);          // keep log valid
    uint16_t lMax = max((uint16_t)(lMin + 1), luxMax); // ensure range
    float lx = constrain(curLux, (float)lMin, (float)lMax);

    float target = mapf(log10f(lx), log10f((float)lMin), log10f((float)lMax),
                        (float)briMin, (float)briMax);

    // exponential moving average to avoid flicker
    if (isnan(briSmoothed)) {
      briSmoothed = target;
    } else {
      float alpha = 1.0f - (constrain(smoothing, 0, 95) / 100.0f);
      briSmoothed += alpha * (target - briSmoothed);
    }

    lastTargetNoOffset = (int)roundf(briSmoothed);
    int finalBri = constrain(lastTargetNoOffset + userBriOffset, 0, 255);

    if (!briBaselineSet || abs(finalBri - (int)bri) >= 2) {
      applyingAuto = true;
      bri = (uint8_t)finalBri;
      lastAutoBri = bri;
      briBaselineSet = true;
      stateUpdated(CALL_MODE_NO_NOTIFY); // keep preset/effect/color; only change brightness
    }
  }

#ifndef WLED_DISABLE_MQTT
  void publishMqtt(const char *topic, const String &value) {
    if (!WLED_MQTT_CONNECTED) return;
    char buf[128];
    snprintf_P(buf, sizeof(buf), PSTR("%s/%s"), mqttDeviceTopic, topic);
    mqtt->publish(buf, 0, false, value.c_str());
  }

  void publishSensors() {
    if (!WLED_MQTT_CONNECTED) return;
    // heartbeat: republish everything periodically even with "Publish Changes Only"
    // so HA's expire_after window never lapses while values are stable
    bool force = !publishChangesOnly;
    if (millis() - lastForcePublish >= SENSORS_I2C_MQTT_HEARTBEAT_MS) force = true;
    if (force) lastForcePublish = millis();

    if (enTemp && (htuFound || bmpFound)) {
      float t = roundDec(curTempC);
      if (!isnan(t) && (force || t != lastTempC)) {
        publishMqtt("temperature", String(toDisplayTemp(curTempC), (unsigned)decimals));
        lastTempC = t;
      }
    }
    if (enHum && htuFound) {
      float h = roundDec(curHum);
      if (!isnan(h) && (force || h != lastHum)) {
        publishMqtt("humidity", String(curHum, (unsigned)decimals));
        lastHum = h;
      }
    }
    if (enPress && bmpFound) {
      float p = roundDec(curPressure);
      if (!isnan(p) && (force || p != lastPressure)) {
        publishMqtt("pressure", String(curPressure, (unsigned)decimals));
        lastPressure = p;
      }
    }
    if (enLux && bhFound && curLux >= 0) {
      float l = roundDec(curLux);
      if (force || l != lastLux) {
        publishMqtt("illuminance", String(curLux, (unsigned)decimals));
        lastLux = l;
      }
    }
    if (enAbsHum) publishDerived("absolute_humidity", curAbsHum, lastAbsHum, force);
    if (enDew)    publishDerived("dew_point",   toDisplayTemp(curDewC),  lastDewC, force);
    if (enHeat)   publishDerived("heat_index",  toDisplayTemp(curHeatC), lastHeatC, force);
    if (enSLP)    publishDerived("sea_level_pressure", curSeaLevel, lastSeaLevel, force);
    if (enAlt)    publishDerived("altitude",    curAltitude, lastAltitude, force);
  }

  // publish one derived value (rounded, change-tracked). `last` is updated in place.
  void publishDerived(const char *topic, float value, float &last, bool force) {
    if (isnan(value)) return;
    float r = roundDec(value);
    if (!force && r == last) return;
    publishMqtt(topic, String(value, (unsigned)decimals));
    last = r;
  }

  // shared discovery fragments: availability via WLED's MQTT LWT (<topic>/status
  // carries retained "online"/"offline") + the common device info block
  void addDiscoveryCommon(StaticJsonDocument<768> &doc) {
    doc[F("availability_topic")] = String(mqttDeviceTopic) + F("/status");
    doc[F("payload_available")] = F("online");
    doc[F("payload_not_available")] = F("offline");

    JsonObject device = doc.createNestedObject(F("device"));
    device[F("name")] = serverDescription;
    device[F("identifiers")] = "wled-sensor-" + String(mqttClientID);
    device[F("manufacturer")] = F(WLED_BRAND);
    device[F("model")] = F(WLED_PRODUCT_NAME);
    device[F("sw_version")] = versionString;
  }

  // Publish one HA discovery config (retained), or clear it (empty retained payload)
  // when the reading is disabled/absent so the stale HA entity actually goes away.
  void createMqttSensor(const String &name, const String &topic, const String &deviceClass, const String &unit, bool active) {
    String t = String(F("homeassistant/sensor/")) + mqttClientID + F("/") + name + F("/config");
    if (!active) { mqtt->publish(t.c_str(), 0, true, ""); return; }

    StaticJsonDocument<768> doc;
    doc[F("name")] = String(serverDescription) + " " + name;
    doc[F("state_topic")] = topic;
    doc[F("unique_id")] = String(mqttClientID) + name;
    if (unit != "") doc[F("unit_of_measurement")] = unit;
    if (deviceClass != "") doc[F("device_class")] = deviceClass;
    doc[F("expire_after")] = SENSORS_I2C_HA_EXPIRE_AFTER_S;
    addDiscoveryCommon(doc);

    String out;
    serializeJson(doc, out);
    mqtt->publish(t.c_str(), 0, true, out.c_str());
  }

  // HA switch for auto-brightness: state on <deviceTopic>/autobri, commands
  // (ON/OFF) on <deviceTopic>/autobri/set — lets HA toggle ambient control
  // without hand-written JSON API presets.
  void createMqttSwitch(const String &name, bool active) {
    String t = String(F("homeassistant/switch/")) + mqttClientID + F("/") + name + F("/config");
    if (!active) { mqtt->publish(t.c_str(), 0, true, ""); return; }

    StaticJsonDocument<768> doc;
    doc[F("name")] = String(serverDescription) + " " + name;
    String st = String(mqttDeviceTopic) + F("/autobri");
    doc[F("state_topic")] = st;
    doc[F("command_topic")] = st + F("/set");
    doc[F("payload_on")] = F("ON");
    doc[F("payload_off")] = F("OFF");
    doc[F("unique_id")] = String(mqttClientID) + name;
    addDiscoveryCommon(doc);

    String out;
    serializeJson(doc, out);
    mqtt->publish(t.c_str(), 0, true, out.c_str());
  }

  void publishAutoBriState() {
    if (!WLED_MQTT_CONNECTED) return;
    char buf[128];
    snprintf_P(buf, sizeof(buf), PSTR("%s/autobri"), mqttDeviceTopic);
    mqtt->publish(buf, 0, true, autoBriEnabled ? "ON" : "OFF"); // retained so HA restarts see current state
  }

  // (Re)publish HA discovery for enabled+present readings and clear the retained
  // config of every other reading (so toggling one off, or disabling HA discovery,
  // removes the entity). Serviced from loop() via discoveryDirty: MQTT connect,
  // settings save, or a sensor appearing late / recovering.
  void mqttInitialize() {
    char topic[128];
    const __FlashStringHelper *tu = tempUnitStr();
    bool ha = haDiscovery;
    snprintf_P(topic, sizeof(topic), PSTR("%s/temperature"), mqttDeviceTopic);
    createMqttSensor(F("Temperature"), topic, F("temperature"), tu, ha && enTemp && (htuFound || bmpFound));
    snprintf_P(topic, sizeof(topic), PSTR("%s/humidity"), mqttDeviceTopic);
    createMqttSensor(F("Humidity"), topic, F("humidity"), F("%"), ha && enHum && htuFound);
    snprintf_P(topic, sizeof(topic), PSTR("%s/pressure"), mqttDeviceTopic);
    createMqttSensor(F("Pressure"), topic, F("pressure"), F("hPa"), ha && enPress && bmpFound);
    snprintf_P(topic, sizeof(topic), PSTR("%s/illuminance"), mqttDeviceTopic);
    createMqttSensor(F("Illuminance"), topic, F("illuminance"), F("lx"), ha && enLux && bhFound);
    snprintf_P(topic, sizeof(topic), PSTR("%s/absolute_humidity"), mqttDeviceTopic);
    createMqttSensor(F("Absolute Humidity"), topic, F(""), F("g/m³"), ha && enAbsHum && htuFound);
    snprintf_P(topic, sizeof(topic), PSTR("%s/dew_point"), mqttDeviceTopic);
    createMqttSensor(F("Dew Point"), topic, F("temperature"), tu, ha && enDew && htuFound);
    snprintf_P(topic, sizeof(topic), PSTR("%s/heat_index"), mqttDeviceTopic);
    createMqttSensor(F("Heat Index"), topic, F("temperature"), tu, ha && enHeat && htuFound);
    snprintf_P(topic, sizeof(topic), PSTR("%s/sea_level_pressure"), mqttDeviceTopic);
    createMqttSensor(F("Sea-Level Pressure"), topic, F("pressure"), F("hPa"), ha && enSLP && bmpFound);
    snprintf_P(topic, sizeof(topic), PSTR("%s/altitude"), mqttDeviceTopic);
    createMqttSensor(F("Altitude"), topic, F("distance"), F("m"), ha && enAlt && bmpFound);
    createMqttSwitch(F("Auto Brightness"), ha && bhFound);
  }
#endif

  void initSensors() {
    bhFound  = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, bhAddress);
    htuFound = htu.begin();
    bmpFound = bmp.begin();
    htuFails = bhFails = bmpFails = 0;
    DEBUG_PRINTF("[%s] BH1750:%d HTU21D:%d BMP180:%d\n", _name, bhFound, htuFound, bmpFound);
  }

  // Periodically retry any sensor not currently present (boot ordering, recovery
  // after a dropout, or a wiring fix) without disturbing the ones that work.
  void probeMissing() {
    bool recovered = false;
    if (!bhFound  && lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, bhAddress)) { bhFound = true; bhFails = 0; recovered = true; }
    if (!htuFound && htu.begin()) { htuFound = true; htuFails = 0; recovered = true; }
    if (!bmpFound && bmp.begin()) { bmpFound = true; bmpFails = 0; recovered = true; }
    if (recovered) discoveryDirty = true; // late/recovered sensor -> refresh HA discovery
  }

public:
  void setup() {
    if (i2c_sda < 0 || i2c_scl < 0) { enabled = false; return; } // I2C not configured
    initSensors();
    initDone = true;
  }

  void loop() {
    if (!enabled || strip.isUpdating()) return;
    unsigned long now = millis();

    if (readRequested || now - lastReadTime >= (unsigned long)readInterval * 1000) {
      readRequested = false;
      lastReadTime = now;
      readSensors();
    }

    if (autoBriEnabled && bhFound && now - lastBriTime >= (unsigned long)briUpdateInterval * 1000) {
      lastBriTime = now;
      updateAutoBrightness();
    }

    // recover any sensor that isn't currently present
    if ((!bhFound || !htuFound || !bmpFound) && now - lastProbeTime >= SENSORS_I2C_PROBE_INTERVAL_MS) {
      lastProbeTime = now;
      probeMissing();
    }

#ifndef WLED_DISABLE_MQTT
    // (re)publish/clear HA discovery after connect, settings save, or sensor recovery
    if (discoveryDirty && WLED_MQTT_CONNECTED) {
      discoveryDirty = false;
      autoBriPubDirty = true;
      mqttInitialize();
    }
    // switch state after connect / MQTT command / JSON command / settings save
    if (autoBriPubDirty && WLED_MQTT_CONNECTED) {
      autoBriPubDirty = false;
      publishAutoBriState();
    }
#endif
  }

  // capture manual brightness changes as a relative offset
  void onStateChange(uint8_t mode) {
    if (!enabled || !autoBriEnabled) return;
    if (applyingAuto) { applyingAuto = false; return; } // our own write
    if (bri == 0) {                                     // powered off: pause, not a manual offset
      if (!autoOffActive) offPause = true;              // (during dark-off, darkness governs instead)
      return;
    }
    if (offPause) {                                     // powered back on: resume auto control;
      offPause = false;                                 // the restored brightness isn't manual
      lastAutoBri = bri;
      return;
    }
    if (autoOffActive) { lastAutoBri = bri; return; }   // manual light during dark-off: override, not offset
    if (mode == CALL_MODE_NIGHTLIGHT) return;           // nightlight fade is not a manual change
    if (!briBaselineSet || bri == lastAutoBri) return;  // not a brightness change we care about
    if (allowManualOffset && lastTargetNoOffset >= 0) {
      userBriOffset = constrain((int)bri - lastTargetNoOffset, -255, 255);
    }
    lastAutoBri = bri;
  }

  void addToJsonInfo(JsonObject &root) {
    JsonObject user = root[F("u")];
    if (user.isNull()) user = root.createNestedObject(F("u"));

    // Mod identity + version (like the word-clock usermod shows on the info page)
    JsonArray ver = user.createNestedArray(F("Sensors I2C"));
    ver.add(F("v" SENSORS_I2C_VERSION));

    if (!enabled) {
      ver.add(F(" (disabled)"));
      return;
    }

    // Each reading is prefixed with "Sensor " so the entries group together on the
    // Info page and can be picked out by the live readings table on the settings page.
    // Every reading can be turned off individually (Readings settings sub-section).
    // Temperature (HTU21D preferred, BMP180 fallback) — both °C and °F.
    if (enTemp) {
      if (!htuFound && !bmpFound) user.createNestedArray(F("Sensor Temperature")).add(F("Not Found"));
      else addTempInfo(user, F("Sensor Temperature"), curTempC);
    }
    // Humidity (HTU21D)
    if (enHum) {
      JsonArray j = user.createNestedArray(F("Sensor Humidity"));
      if (!htuFound) j.add(F("Not Found"));
      else if (isnan(curHum)) j.add(F("-"));
      else { j.add(roundDec(curHum)); j.add(F("%")); }
    }
    // Derived from temperature + humidity (HTU21D)
    if (enAbsHum && htuFound) {
      JsonArray j = user.createNestedArray(F("Sensor Absolute Humidity"));
      if (isnan(curAbsHum)) j.add(F("-")); else { j.add(roundDec(curAbsHum)); j.add(F("g/m³")); }
    }
    if (enDew  && htuFound) addTempInfo(user, F("Sensor Dew Point"), curDewC);
    if (enHeat && htuFound) addTempInfo(user, F("Sensor Heat Index"), curHeatC);
    // Pressure (BMP180)
    if (enPress) {
      JsonArray j = user.createNestedArray(F("Sensor Pressure"));
      if (!bmpFound) j.add(F("Not Found"));
      else if (isnan(curPressure)) j.add(F("-"));
      else { j.add(roundDec(curPressure)); j.add(F("hPa")); }
    }
    // Derived from pressure (BMP180)
    if (enSLP && bmpFound) {
      JsonArray j = user.createNestedArray(F("Sensor Sea-Level Pressure"));
      if (isnan(curSeaLevel)) j.add(F("-")); else { j.add(roundDec(curSeaLevel)); j.add(F("hPa")); }
    }
    if (enAlt && bmpFound) {
      JsonArray j = user.createNestedArray(F("Sensor Altitude"));
      if (isnan(curAltitude)) j.add(F("-")); else { j.add(roundDec(curAltitude)); j.add(F("m")); }
    }
    // Illuminance (BH1750)
    if (enLux) {
      JsonArray j = user.createNestedArray(F("Sensor Illuminance"));
      if (!bhFound) j.add(F("Not Found"));
      else if (curLux < 0) j.add(F("-"));
      else { j.add(roundDec(curLux)); j.add(F("lx")); }
    }
    // Auto-brightness status: applied brightness (/255), the lux-mapped target,
    // and the current manual offset. The second array element is always non-empty
    // so WLED renders "value + suffix" (an empty suffix would stringify the whole
    // array and show a stray trailing comma).
    if (autoBriEnabled && bhFound) {
      JsonArray j = user.createNestedArray(F("Sensor Auto-Brightness"));
      j.add(bri);
      String suffix = F(" / 255");
      if (lastTargetNoOffset >= 0) suffix += String(F(", target ")) + lastTargetNoOffset;
      suffix += String(F(", offset ")) + (userBriOffset > 0 ? "+" : "") + userBriOffset;
      j.add(suffix);
    }
  }

  // expose current auto-brightness state for external automation
  void addToJsonState(JsonObject &root) {
    JsonObject um = root[FPSTR(_stateKey)];
    if (um.isNull()) um = root.createNestedObject(FPSTR(_stateKey));
    um[F("autoBri")] = autoBriEnabled;
    um[F("offset")] = userBriOffset;
  }

  // accept commands (also fires when a preset is applied) -> preset-triggerable reset
  void readFromJsonState(JsonObject &root) {
    JsonObject um = root[FPSTR(_stateKey)];
    if (um.isNull()) return;
    bool b;
    if (getJsonValue(um[F("autoBri")], b)) { autoBriEnabled = b; autoBriPubDirty = true; }
    if (getJsonValue(um[F("resetOffset")], b) && b) userBriOffset = 0;
    if (getJsonValue(um[F("read")], b) && b) readRequested = true; // I2C happens in loop(), not here
  }

  void appendConfigData() {
    // Temperature unit dropdown
    oappend(F("dd=addDropdown('Sensors I2C:Sensors','Temperature Unit');"));
    oappend(F("addOption(dd,'Celsius (°C)','0');"));
    oappend(F("addOption(dd,'Fahrenheit (°F)','1');"));
    // BH1750 address dropdown (0x23 default / 0x5C when ADDR pin high)
    oappend(F("dd=addDropdown('Sensors I2C:Sensors','BH1750 Address');"));
    oappend(F("addOption(dd,'0x23 (default)','35');"));
    oappend(F("addOption(dd,'0x5C','92');"));
    // unit / help hints
    oappend(F("addInfo('Sensors I2C:Sensors:Read Interval',1,'sec');"));
    oappend(F("addInfo('Sensors I2C:Sensors:Decimals',1,'(0-3)');"));
    oappend(F("addInfo('Sensors I2C:Sensors:Station Altitude',1,'m (for sea-level pressure)');"));
    oappend(F("addInfo('Sensors I2C:Auto Brightness:Smoothing',1,'% (0=off)');"));
    oappend(F("addInfo('Sensors I2C:Auto Brightness:Update Interval',1,'sec');"));
    oappend(F("addInfo('Sensors I2C:Auto Brightness:Off Below Lux',1,'lx (0=never)');"));
    oappend(F("addInfo('Sensors I2C:Auto Brightness:On Above Lux',1,'lx (back on; kept >= Off Below Lux)');"));
    oappend(F("addInfo('Sensors I2C:Auto Brightness:Reset Offset',1,'clears manual offset');"));

    // Arrange the four Lux/Brightness fields into a 2x2 mapping table
    // (rows Min/Max x columns Lux|Brightness), styled to match WLED's settings
    // (centered, borderless with a header underline, WLED 's' number inputs).
    // Moves the existing input nodes so their name/value are preserved; guarded
    // so it no-ops if the settings DOM ever changes.
    oappend(F("(function(){try{var P='Sensors I2C:Auto Brightness:';"));
    oappend(F("function vis(n){var a=d.getElementsByName(P+n);return a.length?a[a.length-1]:null;}"));
    oappend(F("function hid(e){var p=e.previousSibling;while(p&&p.nodeType!=1)p=p.previousSibling;return(p&&p.tagName=='INPUT'&&p.type=='hidden')?p:null;}"));
    oappend(F("function strip(e){var s=hid(e)||e,n=s.previousSibling;while(n&&n.nodeType==3){var x=n;n=n.previousSibling;x.remove();}var m=e.nextSibling;while(m){var q=m.nextSibling,b=(m.nodeType==1&&m.tagName=='BR');m.remove();if(b)break;m=q;}}"));
    oappend(F("var F=['Lux Min','Lux Max','Brightness Min','Brightness Max'],E={},ok=1;"));
    oappend(F("F.forEach(function(n){E[n]=vis(n);if(!E[n])ok=0;});if(!ok)return;"));
    oappend(F("var pa=E['Lux Min'].parentNode;F.forEach(function(n){strip(E[n]);});"));
    oappend(F("var ref=hid(E['Lux Min'])||E['Lux Min'];"));
    oappend(F("function C(t,x,hd){var c=d.createElement(t);if(x!=null)c.textContent=x;c.style.padding='1px 7px';c.style.textAlign='center';if(hd)c.style.borderBottom='1px solid #666';return c;}"));
    oappend(F("var T=d.createElement('table');T.style.borderCollapse='collapse';T.style.margin='4px auto 6px';"));
    oappend(F("var h=d.createElement('tr');h.appendChild(C('th','',1));h.appendChild(C('th','Lux (lx)',1));h.appendChild(C('th','Brightness',1));T.appendChild(h);"));
    oappend(F("var r1=d.createElement('tr'),a1=C('td'),b1=C('td');r1.appendChild(C('th','Min'));r1.appendChild(a1);r1.appendChild(b1);T.appendChild(r1);"));
    oappend(F("var r2=d.createElement('tr'),a2=C('td'),b2=C('td');r2.appendChild(C('th','Max'));r2.appendChild(a2);r2.appendChild(b2);T.appendChild(r2);"));
    oappend(F("pa.insertBefore(T,ref);"));
    oappend(F("function mv(n,c){var e=E[n],g=hid(e);e.className='s';if(g)c.appendChild(g);c.appendChild(e);}"));
    oappend(F("mv('Lux Min',a1);mv('Brightness Min',b1);mv('Lux Max',a2);mv('Brightness Max',b2);"));
    oappend(F("}catch(e){}})();"));

    // "Readings" subsection (matches the Sensors/Auto Brightness sub-headers): a
    // live table populated from /json/info plus a WLED-styled Refresh button.
    oappend(F("(function(){try{if(d.getElementById('si2cRd'))return;"));
    oappend(F("var en=d.getElementsByName('Sensors I2C:Enabled');if(!en.length)return;"));
    oappend(F("var sec=en[en.length-1];while(sec&&!(sec.nodeType==1&&sec.tagName=='DIV'&&sec.className=='sec'))sec=sec.parentNode;if(!sec)return;"));
    oappend(F("function C(t,x,r,hd){var c=d.createElement(t);if(x!=null)c.textContent=x;c.style.padding='1px 10px';c.style.textAlign=r?'right':'left';if(hd){c.style.textAlign='center';c.style.borderBottom='1px solid #666';}return c;}"));
    oappend(F("var T=d.createElement('table');T.id='si2cRd';T.style.borderCollapse='collapse';T.style.margin='4px auto';T.style.minWidth='250px';"));
    oappend(F("function row(k,v,hd){var tr=d.createElement('tr');tr.appendChild(C(hd?'th':'td',k,0,hd));tr.appendChild(C(hd?'th':'td',v,1,hd));T.appendChild(tr);}"));
    oappend(F("function refresh(){T.innerHTML='';row('Reading','Value',1);fetch('/json/info').then(function(r){return r.json();}).then(function(j){var u=(j&&j.u)||{},any=0;Object.keys(u).forEach(function(k){if(k.indexOf('Sensor ')!==0)return;any=1;var v=u[k];row(k.replace(/^Sensor /,''),Array.isArray(v)?v.filter(function(x){return x!==''&&x!=null;}).join(' '):(''+v));});if(!any)row('(no readings)','');}).catch(function(){row('(fetch failed)','');});}"));
    oappend(F("var hr=d.createElement('hr');hr.className='sml';"));
    oappend(F("var p=d.createElement('p'),u2=d.createElement('u');u2.textContent='Live Readings';p.appendChild(u2);"));
    oappend(F("function reread(){fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:'{\"SensorsI2C\":{\"read\":true}}'}).then(function(){setTimeout(refresh,400);}).catch(function(){refresh();});}"));
    oappend(F("var btn=d.createElement('button');btn.type='button';btn.className='btn sml';btn.textContent='\\u21bb Refresh';btn.addEventListener('click',reread);"));
    oappend(F("var bw=d.createElement('div');bw.appendChild(btn);"));
    oappend(F("var an=sec.querySelector('hr.sml');function ins(n){if(an)sec.insertBefore(n,an);else sec.appendChild(n);}"));
    oappend(F("ins(hr);ins(p);ins(T);ins(bw);"));
    oappend(F("refresh();}catch(e){}})();"));

    // Arrange the Readings enable checkboxes into a 2-column table
    // (Measured | Derived). Moves the existing checkbox+label nodes into cells
    // (names/values preserved); guarded so it no-ops if the DOM changes.
    oappend(F("(function(){try{var P='Sensors I2C:Readings:';"));
    oappend(F("function get(n){var a=d.getElementsByName(P+n);return a.length?a[a.length-1]:null;}"));
    oappend(F("function cap(e){var h=null,p=e.previousSibling;while(p&&p.nodeType!=1)p=p.previousSibling;if(p&&p.tagName=='INPUT'&&p.type=='hidden')h=p;var s=h||e,l='',n=s.previousSibling;while(n&&n.nodeType==3){l=n.textContent+l;var x=n;n=n.previousSibling;x.remove();}var m=e.nextSibling;while(m){var q=m.nextSibling,b=(m.nodeType==1&&m.tagName=='BR');m.remove();if(b)break;m=q;}return{cb:e,hid:h,label:l.trim()};}"));
    oappend(F("var L=['Temperature','Humidity','Pressure','Illuminance'],R=['Absolute Humidity','Dew Point','Heat Index','Sea-Level Pressure','Altitude'];"));
    oappend(F("var all=L.concat(R),E={},ok=1;all.forEach(function(n){var e=get(n);if(!e)ok=0;else E[n]=e;});if(!ok)return;"));
    oappend(F("var pa=E['Temperature'].parentNode,K={};all.forEach(function(n){K[n]=cap(E[n]);});"));
    oappend(F("var ref=K['Temperature'].hid||K['Temperature'].cb;"));
    oappend(F("function C(t,x,hd){var c=d.createElement(t);if(x!=null)c.textContent=x;c.style.padding='1px 10px';c.style.textAlign='left';if(hd){c.style.textAlign='center';c.style.borderBottom='1px solid #666';}return c;}"));
    oappend(F("var T=d.createElement('table');T.style.borderCollapse='collapse';T.style.margin='4px auto 2px';"));
    oappend(F("var h=d.createElement('tr');h.appendChild(C('th','Measured',1));h.appendChild(C('th','Derived',1));T.appendChild(h);"));
    oappend(F("function put(c,n){if(!n)return;var k=K[n];if(k.hid)c.appendChild(k.hid);c.appendChild(k.cb);var s=d.createElement('span');s.textContent=' '+k.label;c.appendChild(s);}"));
    oappend(F("var rows=Math.max(L.length,R.length);for(var i=0;i<rows;i++){var tr=d.createElement('tr'),c1=C('td'),c2=C('td');put(c1,L[i]);put(c2,R[i]);tr.appendChild(c1);tr.appendChild(c2);T.appendChild(tr);}"));
    oappend(F("pa.insertBefore(T,ref);"));
    oappend(F("}catch(e){}})();"));
  }

  void addToConfig(JsonObject &root) {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;

    JsonObject s = top.createNestedObject(FPSTR(_grpSensors));
    s[F("Read Interval")] = readInterval;
    s[F("Temperature Unit")] = tempUnit;
    s[F("Decimals")] = decimals;
    s[F("BH1750 Address")] = bhAddress;
    s[F("Station Altitude")] = stationAltitude;
    s[F("Publish Changes Only")] = publishChangesOnly;
    s[F("Home Assistant Discovery")] = haDiscovery;

    JsonObject r = top.createNestedObject(FPSTR(_grpReadings));
    r[F("Temperature")] = enTemp;
    r[F("Humidity")] = enHum;
    r[F("Absolute Humidity")] = enAbsHum;
    r[F("Dew Point")] = enDew;
    r[F("Heat Index")] = enHeat;
    r[F("Pressure")] = enPress;
    r[F("Sea-Level Pressure")] = enSLP;
    r[F("Altitude")] = enAlt;
    r[F("Illuminance")] = enLux;

    JsonObject a = top.createNestedObject(FPSTR(_grpAutoBri));
    a[F("Enabled")] = autoBriEnabled;
    a[F("Lux Min")] = luxMin;
    a[F("Lux Max")] = luxMax;
    a[F("Brightness Min")] = briMin;
    a[F("Brightness Max")] = briMax;
    a[F("Smoothing")] = smoothing;
    a[F("Update Interval")] = briUpdateInterval;
    a[F("Off Below Lux")] = offBelowLux;
    a[F("On Above Lux")] = onAboveLux;
    a[F("Allow Manual Offset")] = allowManualOffset;
    a[F("Reset Offset")] = false; // momentary: never persists as checked
  }

  bool readFromConfig(JsonObject &root) {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) {
      DEBUG_PRINTF("[%s] No config found. (Using defaults.)\n", _name);
      return false;
    }
    bool configComplete = true;

    configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled, true);

    JsonObject s = top[FPSTR(_grpSensors)];
    configComplete &= getJsonValue(s[F("Read Interval")], readInterval, 5);
    configComplete &= getJsonValue(s[F("Temperature Unit")], tempUnit, 0);
    configComplete &= getJsonValue(s[F("Decimals")], decimals, 1);
    configComplete &= getJsonValue(s[F("BH1750 Address")], bhAddress, 0x23);
    configComplete &= getJsonValue(s[F("Station Altitude")], stationAltitude, 0);
    configComplete &= getJsonValue(s[F("Publish Changes Only")], publishChangesOnly, true);
    configComplete &= getJsonValue(s[F("Home Assistant Discovery")], haDiscovery, false);

    JsonObject r = top[FPSTR(_grpReadings)];
    configComplete &= getJsonValue(r[F("Temperature")], enTemp, true);
    configComplete &= getJsonValue(r[F("Humidity")], enHum, true);
    configComplete &= getJsonValue(r[F("Absolute Humidity")], enAbsHum, true);
    configComplete &= getJsonValue(r[F("Dew Point")], enDew, true);
    configComplete &= getJsonValue(r[F("Heat Index")], enHeat, true);
    configComplete &= getJsonValue(r[F("Pressure")], enPress, true);
    configComplete &= getJsonValue(r[F("Sea-Level Pressure")], enSLP, true);
    configComplete &= getJsonValue(r[F("Altitude")], enAlt, true);
    configComplete &= getJsonValue(r[F("Illuminance")], enLux, true);

    JsonObject a = top[FPSTR(_grpAutoBri)];
    configComplete &= getJsonValue(a[F("Enabled")], autoBriEnabled, false);
    configComplete &= getJsonValue(a[F("Lux Min")], luxMin, 1);
    configComplete &= getJsonValue(a[F("Lux Max")], luxMax, 1000);
    configComplete &= getJsonValue(a[F("Brightness Min")], briMin, 5);
    configComplete &= getJsonValue(a[F("Brightness Max")], briMax, 255);
    configComplete &= getJsonValue(a[F("Smoothing")], smoothing, 70);
    configComplete &= getJsonValue(a[F("Update Interval")], briUpdateInterval, 2);
    configComplete &= getJsonValue(a[F("Off Below Lux")], offBelowLux, 0);
    configComplete &= getJsonValue(a[F("On Above Lux")], onAboveLux, 0);
    configComplete &= getJsonValue(a[F("Allow Manual Offset")], allowManualOffset, true);
    getJsonValue(a[F("Reset Offset")], resetOffset, false);

    // sanity / clamping
    readInterval = constrain(readInterval, 1, 3600);
    decimals = constrain(decimals, 0, 3);
    tempUnit = constrain(tempUnit, 0, 1);
    if (bhAddress != 0x23 && bhAddress != 0x5C) bhAddress = 0x23;
    stationAltitude = constrain(stationAltitude, -430, 9000); // Dead Sea .. Everest
    if (luxMax <= luxMin) luxMax = luxMin + 1;
    if (briMax < briMin) { uint8_t t = briMin; briMin = briMax; briMax = t; }
    smoothing = constrain(smoothing, 0, 95);
    briUpdateInterval = constrain(briUpdateInterval, 1, 600);
    if (onAboveLux < offBelowLux) onAboveLux = offBelowLux; // never invert the dark-off range

    if (resetOffset) { userBriOffset = 0; resetOffset = false; }

    // I2C is a hard requirement: never let a saved "Enabled" re-arm the mod while
    // the global I2C pins are unconfigured (setup() disables it for the same reason;
    // WLED parses the hw section before usermod config, so the pins are current here)
    if (i2c_sda < 0 || i2c_scl < 0) enabled = false;

    if (initDone) {
      // re-probe in case wiring/addresses changed and reset smoothing state
      initSensors();
      briSmoothed = NAN;
      autoOffActive = false; // re-evaluate dark-off against the new settings
      discoveryDirty = true; // re-publish/clear HA discovery with the new settings
    }
    return configComplete;
  }

#ifndef WLED_DISABLE_MQTT
  void onMqttConnect(bool sessionPresent) {
    if (!enabled) return;
    char buf[128];
    snprintf_P(buf, sizeof(buf), PSTR("%s/autobri/set"), mqttDeviceTopic);
    mqtt->subscribe(buf, 0);
    discoveryDirty = true; // serviced from loop() while connected
    autoBriPubDirty = true;
  }

  // handles <deviceTopic>/autobri/set (WLED strips the device-topic prefix)
  bool onMqttMessage(char* topic, char* payload) {
    if (strcmp_P(topic, PSTR("/autobri/set")) != 0) return false;
    if      (!strcasecmp(payload, "ON")  || !strcmp(payload, "1")) autoBriEnabled = true;
    else if (!strcasecmp(payload, "OFF") || !strcmp(payload, "0")) autoBriEnabled = false;
    autoBriPubDirty = true; // echo state back (unknown payloads re-assert the truth)
    return true;
  }
#endif

  // API for inter-usermod data exchange
  inline float getTemperatureC() { return roundDec(curTempC); }
  inline float getTemperatureF() { return roundDec(curTempC * 1.8f + 32.0f); }
  inline float getHumidity()     { return roundDec(curHum); }
  inline float getPressure()     { return roundDec(curPressure); }
  inline float getLux()          { return (curLux < 0) ? NAN : curLux; } // NAN = no reading (like the other getters)

  uint16_t getId() { return USERMOD_ID_SENSORS_I2C; }
};

const char UsermodSensorsI2C::_name[]       PROGMEM = "Sensors I2C";
const char UsermodSensorsI2C::_enabled[]    PROGMEM = "Enabled";
const char UsermodSensorsI2C::_grpSensors[] PROGMEM = "Sensors";
const char UsermodSensorsI2C::_grpReadings[] PROGMEM = "Readings";
const char UsermodSensorsI2C::_grpAutoBri[] PROGMEM = "Auto Brightness";
const char UsermodSensorsI2C::_stateKey[]   PROGMEM = "SensorsI2C";

static UsermodSensorsI2C sensors_i2c;
REGISTER_USERMOD(sensors_i2c);
