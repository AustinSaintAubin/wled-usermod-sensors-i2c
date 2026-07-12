# Bench Test Checklist — v1.0.16

Hardware validation for everything changed since v1.0.5. Bench passes ran
2026-07-09/10; §1–§3 are fully green (v1.0.15/16 add new §1 re-test items).
**Remaining: §1 v1.0.16 re-checks, §4 MQTT/Home Assistant (incl. overnight
soak) and §5 dropout recovery.**
After a clean pass: `git tag v1.0.16 && git push origin v1.0.16`.

> ⚠️ **Re-flash note (v1.0.12):** the Off When Dark settings moved into their own
> config sub-section, so its three values (Enabled / Off Below Lux / On Above Lux)
> reset to defaults once — re-enter them after flashing.

> ℹ️ **Re-flash note (v1.0.15):** *Publish Changes Only* and *Home Assistant
> Discovery* moved from *Sensors* into a new *MQTT & Home Assistant* sub-section.
> Unlike the v1.0.12 move these **migrate automatically** (readFromConfig falls back
> to the old keys) — verify the values carried over, don't re-enter them.

---

## 1. Flash & settings page — ✅ passed in full (2026-07-09 v1.0.11, re-checks 2026-07-10 v1.0.14)

- [x] Settings page renders: sub-sections + Lux/Brightness and Readings tables OK
- [x] Dark-off fields present
- [x] Threshold clamp auto-corrects On Above < Off Below
- [x] ↻ Refresh returns a genuinely fresh reading
- [x] **Re-check (v1.0.12):** *Off When Dark* is now its **own sub-section** with the two
      lux fields as a small Off Below / On Above table — renders correctly, values save
- [x] **Reset Offset button (v1.0.14):** with an offset active, click *Reset Offset* →
      shows "Offset Cleared ✓" and the info line offset returns to 0 instantly, no Save

**v1.0.15 layout regroup — re-test:**

- [x] *Enabled* renders **directly under the "Sensors I2C" title** with its hint text
      ("master switch — needs global I2C pins"), no longer orphaned below Live Readings
      *(screenshot 2026-07-12)*
- [x] *Live Readings* sits **between Enabled and Sensors**, exactly one rule between
      each section (none doubled) *(screenshot 2026-07-12)*
- [x] New *MQTT & Home Assistant* group renders **last** (header displays without the
      `&` — WLED strips punctuation) *(screenshot 2026-07-12)*; still to check: both
      values save/reload
- [ ] **Migration:** flash over a v1.0.14 config → *Publish Changes Only* / *HA
      Discovery* values preserved; after one boot, cfg.json (via `/edit`) shows them
      under the new group and no longer under *Sensors*
- [ ] Untick *Enabled* (no Save) → ↻ Refresh → live table shows "(usermod disabled)";
      re-tick → values return; with sensors dark/unwired shows "(no readings — check
      sensor wiring)"
- [x] Regression: Reset Offset button, Lux/Brightness 2×2 table, Off When Dark table
      still render *(screenshot 2026-07-12)*

**v1.0.16 — Readings table fix — re-test:**

- [ ] **Readings Measured | Derived table renders** with all 9 checkboxes (was
      **empty since introduction**: the table was inserted after its anchor node had
      already been moved into it, so `insertBefore` threw and the guard silently ate
      the table — this was the original "Readings section is empty" complaint)
- [ ] Toggling a checkbox off + Save removes that reading from info page / Live
      Readings; re-tick → returns

## 2. Auto-brightness core — ✅ passed in full (2026-07-09; nightlight 2026-07-10)

- [x] Power off from app/UI → lights stay off
- [x] Power on → clean resume, no phantom offset
- [x] Manual nudge → offset captured and tracked
- [X] Nightlight fade untouched by auto-bri, not captured as offset; lights stay off after fading to 0

  *How to test nightlight:* in WLED, *Config → LED Preferences → Timed light*: set
  Duration = 1 min, Target brightness = 0, mode = Fade. Back on the main page, click
  the **moon icon** in the top toolbar to start it. Expected: brightness fades down
  smoothly over the minute with auto-bri enabled (no bouncing back up every 2 s);
  after it reaches 0 the lights stay off; the info line offset is unchanged.

## 3. Dark-off — ✅ passed in full (base 2026-07-09, v1.0.12 semantics re-test 2026-07-10)

Passed on v1.0.11: turns off in dark ✓, no flapping in band ✓, back on when bright ✓,
manual override + disable ✓.

**Found bug (fixed in v1.0.12):** after a slider adjustment (manual offset), covering
the sensor no longer switched the strip off — the override latch caught any brightness
change and only released at On Above Lux.

Re-tested with v1.0.12 (Off Below = 5, On Above = 20 — now the defaults as of v1.0.13):

- [X] Adjust brightness via slider (offset captured), then cover the sensor →
      strip **switches off** (darkness wins over adjustments)
- [X] While off due to darkness, turn lights **on** (slider up from 0 or power toggle) →
      they **stay on** in the dark; info line shows `dark-off (overridden)`
- [X] Brighten the room past On Above Lux → override releases, normal auto-brightness
      resumes at the mapped level
- [X] While overridden (lit in the dark), power **off** → dark-off re-arms immediately
      (strip stays off in darkness; no override left behind)
- [X] Info line shows `dark-off` while engaged, nothing when bright
- [X] Slider all the way down while bright → display dims to 1 but never turns off
      (mapping floor; only dark-off may write 0)

## 4. MQTT / Home Assistant — ⏳ not tested yet

- [ ] Enable *HA Discovery* while MQTT connected → entities appear without reboot/reconnect
- [ ] **Overnight soak:** Publish Changes Only on, stable/dark → entities stay *available*
      past 30 min (heartbeat)
- [ ] Pull device power → entities *unavailable* (LWT); repower → back
- [ ] *Auto Brightness* switch in HA works both directions (HA→device, device→HA)
- [ ] Untick a reading (e.g. Heat Index) → HA entity disappears; re-tick → returns

## 5. Sensor dropout recovery — ⏳ not tested yet

- [ ] Unplug BMP180 → *Not Found* within ~15 s, no garbage on MQTT
- [ ] Replug → recovers ≤ 30 s, HA entities reappear without reboot
- [ ] Quick regression: HTU21D / BH1750 unplug-replug

---

## Results / notes

| Date       | Item                       | Result | Notes |
|------------|----------------------------|--------|-------|
| 2026-07-09 | Settings page (§1)         | ✅ pass | Requested own sub-section + table for dark-off → shipped in v1.0.12 |
| 2026-07-09 | Auto-bri core (§2)         | ✅ pass | Nightlight still pending |
| 2026-07-09 | Dark-off base (§3)         | ✅ pass | — |
| 2026-07-09 | Dark-off + offset (§3)     | ❌ fail | Override latch ate the off; fixed in v1.0.12 (`075fa12`) |
| 2026-07-10 | Dark-off v1.0.12 semantics (§3) | ✅ pass | All six re-test items; 5/20 lux promoted to defaults in v1.0.13 |
| 2026-07-10 | Nightlight fade (§2)       | ✅ pass | Fade undisturbed, clean after, no offset capture |
| 2026-07-10 | UI re-checks (§1)          | ✅ pass | Dark-off sub-section/table + Reset Offset button (v1.0.14) |
| 2026-07-10 | MQTT/HA (§4), recovery (§5)| ⏳      | Awaiting broker/HA access and a cable-pull session |
| 2026-07-12 | UI regroup v1.0.15 (§1)    | ✅ pass | Screenshot: Enabled-first + hint, Live Readings placement, MQTT group last, other tables OK; migration & empty-state checks still pending |
| 2026-07-12 | Readings table (§1)        | ❌ fail | Measured/Derived table empty — insert-after-move bug (present since v1.0.11); fixed in v1.0.16 |
| —          | Readings table v1.0.16 (§1)| ⏳      | Re-test: table renders, toggles still work end-to-end |
