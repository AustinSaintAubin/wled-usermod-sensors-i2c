# Bench Test Checklist — v1.0.11

Hardware validation for everything changed since v1.0.5 (commits `36ebb95`…`60d0e7b`,
v1.0.6–v1.0.11). All items are compile-verified (CI green, both MQTT variants) but
**not yet tested on hardware**. After a clean pass: `git tag v1.0.11 && git push origin v1.0.11`.

**Setup:** ESP32 + 3-in-1 module flashed with current `main`, MQTT broker + Home Assistant
reachable for the MQTT section.

> ⚠️ **Config migration note:** dark-off is now gated by the new *Off When Dark* checkbox
> (default **off**). A pre-1.0.11 config that had *Off Below Lux* set will have dark-off
> disabled after flashing until the checkbox is ticked.

---

## 1. Flash & settings page (~5 min)

- [ ] *Config → Usermods → Sensors I2C* renders: all three sub-sections, the Lux/Brightness
      2×2 table and the Readings Measured|Derived table lay out correctly
      (settings JS was touched in v1.0.7 / v1.0.11)
- [ ] New Auto Brightness fields present: **Off When Dark** checkbox, **Off Below Lux**,
      **On Above Lux**
- [ ] Save with *On Above Lux* < *Off Below Lux* → after reload it is auto-corrected to ≥ Off Below
- [ ] **↻ Refresh button (v1.0.7):** change the light hitting the sensor, click Refresh →
      table shows the *new* value immediately (forces a real read, not a stale re-fetch)

## 2. Auto-brightness core (v1.0.6)

- [ ] With auto-bri on, power **off** from app/UI → lights **stay off** (no self-resurrection)
- [ ] Power **on** → auto control resumes within ~2 s at a sane level, and no phantom manual
      offset appears (info page: `Sensor Auto-Brightness … offset 0`)
- [ ] Manual brightness nudge while on → offset captured and tracked as before (regression)
- [ ] Nightlight fade runs undisturbed → auto-bri doesn't fight it, fade not captured as offset

## 3. Dark-off (v1.0.9–v1.0.11)

Suggested settings for the test: *Off When Dark* ✓, *Off Below Lux* = 5, *On Above Lux* = 20.

- [ ] Cover the sensor (< 5 lx) → strip turns **fully off**
- [ ] Light in the 5–20 lx band → stays off (hysteresis band, no flapping)
- [ ] Above 20 lx → comes back on, **jumping straight** to the mapped brightness (no slow fade-up)
- [ ] While dark/off, turn lights **on** manually → they stay on (auto keeps hands off);
      brighten past 20 lx → auto control resumes
- [ ] Untick *Off When Dark*, save → lux thresholds inert, normal auto-bri behavior

## 4. MQTT / Home Assistant (v1.0.6 + v1.0.8)

- [ ] Enable *Home Assistant Discovery* while MQTT is already connected → entities appear in HA
      **without a reboot or MQTT reconnect** (v1.0.6 discovery fix)
- [ ] **Overnight soak:** *Publish Changes Only* on, stable/dark conditions → entities stay
      *available* past 30 min (heartbeat; old bug made Illuminance go unavailable every night)
- [ ] Pull device power → HA entities go *unavailable* within the broker keepalive
      (LWT availability, v1.0.8); repower → back to *available*
- [ ] **Auto Brightness switch** in HA: toggle in HA → device engages/disengages auto-bri;
      toggle from the WLED side (settings save or JSON preset) → HA switch state follows
- [ ] Untick one reading (e.g. Heat Index), save → its HA entity **disappears**
      (retained-config cleanup); re-tick → it returns

## 5. Sensor dropout recovery (v1.0.6)

- [ ] Unplug the **BMP180** (or module SDA) → within ~15 s pressure shows *Not Found*,
      **no garbage values** published on MQTT
- [ ] Replug → recovers within 30 s **and** HA entities reappear without a reboot
      (discovery-on-recovery)
- [ ] Quick regression: same unplug/replug for HTU21D and BH1750 if convenient

---

## Results / notes

| Date | Item | Result | Notes |
|------|------|--------|-------|
|      |      |        |       |
