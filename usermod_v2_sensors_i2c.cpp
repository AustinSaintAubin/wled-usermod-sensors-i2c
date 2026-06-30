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

  // ------- auto-brightness settings -------
  bool     autoBriEnabled = false;
  uint16_t luxMin = 1;           // lux mapped to Min Brightness
  uint16_t luxMax = 1000;        // lux mapped to Max Brightness
  uint8_t  briMin = 5;           // brightness at/below luxMin
  uint8_t  briMax = 255;         // brightness at/above luxMax
  uint8_t  smoothing = 70;       // EMA smoothing percent (0 = off, higher = smoother)
  uint8_t  briUpdateInterval = 2;// seconds between brightness updates
  bool     allowManualOffset = true;
  bool     resetOffset = false;  // momentary; cleared on save

  // ------- runtime sensor state -------
  BH1750                    lightMeter;
  Adafruit_HTU21DF          htu = Adafruit_HTU21DF();
  Adafruit_BMP085_Unified   bmp = Adafruit_BMP085_Unified(10085);
  bool bhFound = false, htuFound = false, bmpFound = false;

  float curTempC   = NAN;   // chosen temperature source, always stored in Celsius
  float curHum     = NAN;   // %
  float curPressure= NAN;   // hPa
  float curLux     = -1;    // lx

  // change tracking for "publish only on change"
  float lastTempC = NAN, lastHum = NAN, lastPressure = NAN, lastLux = NAN;

  unsigned long lastReadTime = 0;
  unsigned long lastBriTime  = 0;

  // ------- auto-brightness runtime -------
  float   briSmoothed   = NAN;   // EMA state (mapped brightness, before offset)
  int     lastTargetNoOffset = -1;
  int     userBriOffset = 0;     // relative offset captured from manual changes
  uint8_t lastAutoBri   = 0;     // last brightness value we applied
  bool    briBaselineSet = false;// have we applied auto brightness at least once
  bool    applyingAuto  = false; // guards onStateChange against our own writes

#ifndef WLED_DISABLE_MQTT
  bool mqttInitialized = false;
#endif

  // strings to reduce flash usage
  static const char _name[];
  static const char _enabled[];
  static const char _grpSensors[];
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

  void readSensors() {
    if (htuFound) {
      float t = htu.readTemperature();
      float h = htu.readHumidity();
      if (!isnan(t)) curTempC = t;   // HTU21D is the preferred temperature source
      if (!isnan(h)) curHum = h;
    }
    if (bmpFound) {
      float p = NAN, t = NAN;
      bmp.getPressure(&p);           // Pa
      bmp.getTemperature(&t);        // °C
      if (!isnan(p)) curPressure = p / 100.0f; // hPa
      if (!htuFound && !isnan(t)) curTempC = t; // fall back to BMP180 temperature
    }
    if (bhFound) {
      float lux = lightMeter.readLightLevel();
      if (lux >= 0) curLux = lux;
    }

#ifndef WLED_DISABLE_MQTT
    publishSensors();
#endif
  }

  void updateAutoBrightness() {
    if (!bhFound) return;
    float lux = lightMeter.readLightLevel();
    if (lux < 0) return;             // read error / not ready
    curLux = lux;

    uint16_t lMin = max((uint16_t)1, luxMin);          // keep log valid
    uint16_t lMax = max((uint16_t)(lMin + 1), luxMax); // ensure range
    float lx = constrain(lux, (float)lMin, (float)lMax);

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
    snprintf_P(buf, 127, PSTR("%s/%s"), mqttDeviceTopic, topic);
    mqtt->publish(buf, 0, false, value.c_str());
  }

  void publishSensors() {
    if (!WLED_MQTT_CONNECTED) return;
    if (htuFound || bmpFound) {
      float t = roundDec(curTempC);
      if (!isnan(t) && (!publishChangesOnly || t != lastTempC)) {
        publishMqtt("temperature", String(toDisplayTemp(curTempC), (unsigned)decimals));
        lastTempC = t;
      }
    }
    if (htuFound) {
      float h = roundDec(curHum);
      if (!isnan(h) && (!publishChangesOnly || h != lastHum)) {
        publishMqtt("humidity", String(curHum, (unsigned)decimals));
        lastHum = h;
      }
    }
    if (bmpFound) {
      float p = roundDec(curPressure);
      if (!isnan(p) && (!publishChangesOnly || p != lastPressure)) {
        publishMqtt("pressure", String(curPressure, (unsigned)decimals));
        lastPressure = p;
      }
    }
    if (bhFound) {
      if (curLux >= 0 && (!publishChangesOnly || curLux != lastLux)) {
        publishMqtt("illuminance", String(curLux, 1));
        lastLux = curLux;
      }
    }
  }

  void createMqttSensor(const String &name, const String &topic, const String &deviceClass, const String &unit) {
    String t = String(F("homeassistant/sensor/")) + mqttClientID + F("/") + name + F("/config");

    StaticJsonDocument<600> doc;
    doc[F("name")] = String(serverDescription) + " " + name;
    doc[F("state_topic")] = topic;
    doc[F("unique_id")] = String(mqttClientID) + name;
    if (unit != "") doc[F("unit_of_measurement")] = unit;
    if (deviceClass != "") doc[F("device_class")] = deviceClass;
    doc[F("expire_after")] = 1800;

    JsonObject device = doc.createNestedObject(F("device"));
    device[F("name")] = serverDescription;
    device[F("identifiers")] = "wled-sensor-" + String(mqttClientID);
    device[F("manufacturer")] = F(WLED_BRAND);
    device[F("model")] = F(WLED_PRODUCT_NAME);
    device[F("sw_version")] = versionString;

    String out;
    serializeJson(doc, out);
    mqtt->publish(t.c_str(), 0, true, out.c_str());
  }

  void mqttInitialize() {
    char topic[128];
    if (htuFound || bmpFound) {
      snprintf_P(topic, 127, PSTR("%s/temperature"), mqttDeviceTopic);
      createMqttSensor(F("Temperature"), topic, F("temperature"), tempUnit == 1 ? F("°F") : F("°C"));
    }
    if (htuFound) {
      snprintf_P(topic, 127, PSTR("%s/humidity"), mqttDeviceTopic);
      createMqttSensor(F("Humidity"), topic, F("humidity"), F("%"));
    }
    if (bmpFound) {
      snprintf_P(topic, 127, PSTR("%s/pressure"), mqttDeviceTopic);
      createMqttSensor(F("Pressure"), topic, F("pressure"), F("hPa"));
    }
    if (bhFound) {
      snprintf_P(topic, 127, PSTR("%s/illuminance"), mqttDeviceTopic);
      createMqttSensor(F("Illuminance"), topic, F("illuminance"), F("lx"));
    }
  }
#endif

  void initSensors() {
    bhFound  = lightMeter.begin();
    htuFound = htu.begin();
    bmpFound = bmp.begin();
    DEBUG_PRINTF("[%s] BH1750:%d HTU21D:%d BMP180:%d\n", _name, bhFound, htuFound, bmpFound);
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

    if (now - lastReadTime >= (unsigned long)readInterval * 1000) {
      lastReadTime = now;
      readSensors();
    }

    if (autoBriEnabled && bhFound && now - lastBriTime >= (unsigned long)briUpdateInterval * 1000) {
      lastBriTime = now;
      updateAutoBrightness();
    }
  }

  // capture manual brightness changes as a relative offset
  void onStateChange(uint8_t mode) {
    if (!enabled || !autoBriEnabled) return;
    if (applyingAuto) { applyingAuto = false; return; } // our own write
    if (!briBaselineSet || bri == lastAutoBri) return;  // not a brightness change we care about
    if (allowManualOffset && lastTargetNoOffset >= 0) {
      userBriOffset = constrain((int)bri - lastTargetNoOffset, -255, 255);
    }
    lastAutoBri = bri;
  }

  void addToJsonInfo(JsonObject &root) {
    JsonObject user = root[F("u")];
    if (user.isNull()) user = root.createNestedObject(F("u"));

    if (!enabled) {
      JsonArray j = user.createNestedArray(F("Sensors I2C"));
      j.add(F("disabled"));
      return;
    }

    // Each reading is prefixed with "Sensor " so the entries group together on the
    // Info page and can be picked out by the live readings table on the settings page.
    // Temperature (HTU21D preferred, BMP180 fallback)
    {
      JsonArray j = user.createNestedArray(F("Sensor Temperature"));
      if (!htuFound && !bmpFound) j.add(F("Not Found"));
      else if (isnan(curTempC)) j.add(F("-"));
      else { j.add(roundDec(toDisplayTemp(curTempC))); j.add(tempUnitStr()); }
    }
    // Humidity (HTU21D)
    {
      JsonArray j = user.createNestedArray(F("Sensor Humidity"));
      if (!htuFound) j.add(F("Not Found"));
      else if (isnan(curHum)) j.add(F("-"));
      else { j.add(roundDec(curHum)); j.add(F("%")); }
    }
    // Pressure (BMP180)
    {
      JsonArray j = user.createNestedArray(F("Sensor Pressure"));
      if (!bmpFound) j.add(F("Not Found"));
      else if (isnan(curPressure)) j.add(F("-"));
      else { j.add(roundDec(curPressure)); j.add(F("hPa")); }
    }
    // Illuminance (BH1750)
    {
      JsonArray j = user.createNestedArray(F("Sensor Illuminance"));
      if (!bhFound) j.add(F("Not Found"));
      else if (curLux < 0) j.add(F("-"));
      else { j.add(roundf(curLux * 10) / 10); j.add(F("lx")); }
    }
    // Auto-brightness status
    if (autoBriEnabled && bhFound) {
      JsonArray j = user.createNestedArray(F("Sensor Auto-Brightness"));
      j.add(bri);
      if (userBriOffset != 0) {
        j.add(String(F(" (offset ")) + (userBriOffset > 0 ? "+" : "") + userBriOffset + F(")"));
      } else {
        j.add(F(""));
      }
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
    if (getJsonValue(um[F("autoBri")], b)) autoBriEnabled = b;
    if (getJsonValue(um[F("resetOffset")], b) && b) userBriOffset = 0;
  }

  void appendConfigData() {
    // Temperature unit dropdown
    oappend(F("dd=addDropdown('Sensors I2C:Sensors','Temperature Unit');"));
    oappend(F("addOption(dd,'Celsius (°C)','0');"));
    oappend(F("addOption(dd,'Fahrenheit (°F)','1');"));
    // unit / help hints
    oappend(F("addInfo('Sensors I2C:Sensors:Read Interval',1,'sec');"));
    oappend(F("addInfo('Sensors I2C:Sensors:Decimals',1,'(0-3)');"));
    oappend(F("addInfo('Sensors I2C:Auto Brightness:Smoothing',1,'% (0=off, higher=smoother)');"));
    oappend(F("addInfo('Sensors I2C:Auto Brightness:Update Interval',1,'sec');"));
    oappend(F("addInfo('Sensors I2C:Auto Brightness:Reset Offset',1,'apply once to clear manual offset');"));

    // Arrange the four Lux/Brightness fields into a 2x2 mapping table
    // (rows Min/Max x columns Lux|Brightness). Moves the existing input nodes
    // so their name/value are preserved; guarded so it no-ops if the settings
    // DOM ever changes (falls back to WLED's default field rendering).
    oappend(F("(function(){try{var P='Sensors I2C:Auto Brightness:';"));
    oappend(F("function vis(n){var a=d.getElementsByName(P+n);return a.length?a[a.length-1]:null;}"));
    oappend(F("function hid(e){var p=e.previousSibling;while(p&&p.nodeType!=1)p=p.previousSibling;return(p&&p.tagName=='INPUT'&&p.type=='hidden')?p:null;}"));
    oappend(F("function strip(e){var s=hid(e)||e,n=s.previousSibling;while(n&&n.nodeType==3){var x=n;n=n.previousSibling;x.remove();}var m=e.nextSibling;while(m){var q=m.nextSibling,b=(m.nodeType==1&&m.tagName=='BR');m.remove();if(b)break;m=q;}}"));
    oappend(F("var F=['Lux Min','Lux Max','Brightness Min','Brightness Max'],E={},ok=1;"));
    oappend(F("F.forEach(function(n){E[n]=vis(n);if(!E[n])ok=0;});if(!ok)return;"));
    oappend(F("var pa=E['Lux Min'].parentNode;F.forEach(function(n){strip(E[n]);});"));
    oappend(F("var ref=hid(E['Lux Min'])||E['Lux Min'];"));
    oappend(F("function C(t,x){var c=d.createElement(t);if(x!=null)c.textContent=x;c.style.padding='2px 8px';c.style.border='1px solid rgba(128,128,128,.35)';c.style.textAlign='center';return c;}"));
    oappend(F("var T=d.createElement('table');T.style.borderCollapse='collapse';T.style.margin='3px 0 6px 0';"));
    oappend(F("var h=d.createElement('tr');h.appendChild(C('th',''));h.appendChild(C('th','Lux (lx)'));h.appendChild(C('th','Brightness (0-255)'));T.appendChild(h);"));
    oappend(F("var r1=d.createElement('tr'),a1=C('td'),b1=C('td');r1.appendChild(C('td','Min'));r1.appendChild(a1);r1.appendChild(b1);T.appendChild(r1);"));
    oappend(F("var r2=d.createElement('tr'),a2=C('td'),b2=C('td');r2.appendChild(C('td','Max'));r2.appendChild(a2);r2.appendChild(b2);T.appendChild(r2);"));
    oappend(F("pa.insertBefore(T,ref);"));
    oappend(F("function mv(n,c){var e=E[n],g=hid(e);e.className='';e.style.setProperty('width','6em','important');if(g)c.appendChild(g);c.appendChild(e);}"));
    oappend(F("mv('Lux Min',a1);mv('Brightness Min',b1);mv('Lux Max',a2);mv('Brightness Max',b2);"));
    oappend(F("}catch(e){}})();"));

    // Live readings panel at the top of the section: a table populated from
    // /json/info (the "Sensor ..." entries) plus a Refresh button. Guarded so it
    // no-ops if anything is missing.
    oappend(F("(function(){try{if(d.getElementById('si2cRd'))return;"));
    oappend(F("var en=d.getElementsByName('Sensors I2C:Enabled');if(!en.length)return;"));
    oappend(F("var sec=en[en.length-1];while(sec&&!(sec.nodeType==1&&sec.tagName=='DIV'&&sec.className=='sec'))sec=sec.parentNode;if(!sec)return;"));
    oappend(F("var box=d.createElement('div');box.style.margin='4px 0 8px 0';"));
    oappend(F("var T=d.createElement('table');T.id='si2cRd';T.style.borderCollapse='collapse';box.appendChild(T);box.appendChild(d.createElement('br'));"));
    oappend(F("var btn=d.createElement('button');btn.type='button';btn.textContent='\\u21bb Refresh readings';btn.style.marginTop='4px';box.appendChild(btn);"));
    oappend(F("function C(t,x,r){var c=d.createElement(t);if(x!=null)c.textContent=x;c.style.padding='2px 10px';c.style.border='1px solid rgba(128,128,128,.35)';if(r)c.style.textAlign='right';return c;}"));
    oappend(F("function row(k,v){var tr=d.createElement('tr');tr.appendChild(C('td',k));tr.appendChild(C('td',v,1));T.appendChild(tr);}"));
    oappend(F("function refresh(){T.innerHTML='';row('Reading','Value');fetch('/json/info').then(function(r){return r.json();}).then(function(j){var u=(j&&j.u)||{},any=0;Object.keys(u).forEach(function(k){if(k.indexOf('Sensor ')!==0)return;any=1;var v=u[k];row(k.replace(/^Sensor /,''),Array.isArray(v)?v.filter(function(x){return x!==''&&x!=null;}).join(' '):(''+v));});if(!any)row('(no readings)','');}).catch(function(){row('(fetch failed)','');});}"));
    oappend(F("btn.addEventListener('click',refresh);"));
    oappend(F("var h3=sec.querySelector('h3');if(h3&&h3.nextSibling)sec.insertBefore(box,h3.nextSibling);else sec.appendChild(box);"));
    oappend(F("refresh();}catch(e){}})();"));
  }

  void addToConfig(JsonObject &root) {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;

    JsonObject s = top.createNestedObject(FPSTR(_grpSensors));
    s[F("Read Interval")] = readInterval;
    s[F("Temperature Unit")] = tempUnit;
    s[F("Decimals")] = decimals;
    s[F("Publish Changes Only")] = publishChangesOnly;
    s[F("Home Assistant Discovery")] = haDiscovery;

    JsonObject a = top.createNestedObject(FPSTR(_grpAutoBri));
    a[F("Enabled")] = autoBriEnabled;
    a[F("Lux Min")] = luxMin;
    a[F("Lux Max")] = luxMax;
    a[F("Brightness Min")] = briMin;
    a[F("Brightness Max")] = briMax;
    a[F("Smoothing")] = smoothing;
    a[F("Update Interval")] = briUpdateInterval;
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
    configComplete &= getJsonValue(s[F("Publish Changes Only")], publishChangesOnly, true);
    configComplete &= getJsonValue(s[F("Home Assistant Discovery")], haDiscovery, false);

    JsonObject a = top[FPSTR(_grpAutoBri)];
    configComplete &= getJsonValue(a[F("Enabled")], autoBriEnabled, false);
    configComplete &= getJsonValue(a[F("Lux Min")], luxMin, 1);
    configComplete &= getJsonValue(a[F("Lux Max")], luxMax, 1000);
    configComplete &= getJsonValue(a[F("Brightness Min")], briMin, 5);
    configComplete &= getJsonValue(a[F("Brightness Max")], briMax, 255);
    configComplete &= getJsonValue(a[F("Smoothing")], smoothing, 70);
    configComplete &= getJsonValue(a[F("Update Interval")], briUpdateInterval, 2);
    configComplete &= getJsonValue(a[F("Allow Manual Offset")], allowManualOffset, true);
    getJsonValue(a[F("Reset Offset")], resetOffset, false);

    // sanity / clamping
    readInterval = constrain(readInterval, 1, 3600);
    decimals = constrain(decimals, 0, 3);
    tempUnit = constrain(tempUnit, 0, 1);
    if (luxMax <= luxMin) luxMax = luxMin + 1;
    if (briMax < briMin) { uint8_t t = briMin; briMin = briMax; briMax = t; }
    smoothing = constrain(smoothing, 0, 95);
    briUpdateInterval = constrain(briUpdateInterval, 1, 600);

    if (resetOffset) { userBriOffset = 0; resetOffset = false; }

    if (initDone) {
      // re-probe in case wiring/addresses changed and reset smoothing state
      initSensors();
      briSmoothed = NAN;
#ifndef WLED_DISABLE_MQTT
      mqttInitialized = false;
#endif
    }
    return configComplete;
  }

#ifndef WLED_DISABLE_MQTT
  void onMqttConnect(bool sessionPresent) {
    if (!enabled || !WLED_MQTT_CONNECTED) return;
    if (haDiscovery && !mqttInitialized) {
      mqttInitialize();
      mqttInitialized = true;
    }
  }
#endif

  // API for inter-usermod data exchange
  inline float getTemperatureC() { return roundDec(curTempC); }
  inline float getTemperatureF() { return roundDec(curTempC * 1.8f + 32.0f); }
  inline float getHumidity()     { return roundDec(curHum); }
  inline float getPressure()     { return roundDec(curPressure); }
  inline float getLux()          { return curLux; }

  uint16_t getId() { return USERMOD_ID_SENSORS_I2C; }
};

const char UsermodSensorsI2C::_name[]       PROGMEM = "Sensors I2C";
const char UsermodSensorsI2C::_enabled[]    PROGMEM = "Enabled";
const char UsermodSensorsI2C::_grpSensors[] PROGMEM = "Sensors";
const char UsermodSensorsI2C::_grpAutoBri[] PROGMEM = "Auto Brightness";
const char UsermodSensorsI2C::_stateKey[]   PROGMEM = "SensorsI2C";

static UsermodSensorsI2C sensors_i2c;
REGISTER_USERMOD(sensors_i2c);
