# Bench Test Checklist — v1.0.12

Hardware validation for everything changed since v1.0.5. First bench pass ran
**2026-07-09 on v1.0.11**; it found the offset/dark-off bug fixed in v1.0.12
(commit `075fa12`), so the dark-off section needs a **re-test on v1.0.12**.
After a clean pass: `git tag v1.0.12 && git push origin v1.0.12`.

> ⚠️ **Re-flash note (v1.0.12):** the Off When Dark settings moved into their own
> config sub-section, so its three values (Enabled / Off Below Lux / On Above Lux)
> reset to defaults once — re-enter them after flashing.

---

## 1. Flash & settings page — ✅ passed 2026-07-09 (v1.0.11), one re-check

- [x] Settings page renders: sub-sections + Lux/Brightness and Readings tables OK
- [x] Dark-off fields present
- [x] Threshold clamp auto-corrects On Above < Off Below
- [x] ↻ Refresh returns a genuinely fresh reading
- [ ] **Re-check (v1.0.12):** *Off When Dark* is now its **own sub-section** with the two
      lux fields as a small Off Below / On Above table — renders correctly, values save

## 2. Auto-brightness core — ✅ mostly passed 2026-07-09

- [x] Power off from app/UI → lights stay off
- [x] Power on → clean resume, no phantom offset
- [x] Manual nudge → offset captured and tracked
- [ ] Nightlight fade untouched by auto-bri, not captured as offset *(not tested yet)*

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
| 2026-07-09 | MQTT/HA (§4), recovery (§5)| ⏳      | Not yet testable on the bench |
