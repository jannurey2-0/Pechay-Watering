# IoT Pechay Watering & Yield Prediction System — Implementation Plan

Working prototype scope: the four core goals working end-to-end with simple implementations.
Items tagged **[MVP]** are required to call the prototype done. Items tagged **[FULL]** are
optional polish beyond the 50% target — do them only after the MVP works.

**Starter code already in this folder** (referenced throughout the plan):

```
Pechay Watering/
├── IMPLEMENTATION_PLAN.md          ← this file
├── SOLAR_WIRING.md                 ← CURRENT wiring guide: solar/battery, 3 pots, 12V pump, LCD
├── firmware/
│   ├── calibrate_moisture/         ← run this FIRST to calibrate the 3 YL-69 probes
│   ├── test_sensors_dashboard/     ← sensors-only test: proves data reaches the dashboard
│   └── pechay_watering/            ← main ESP32 firmware (3 pots + LCD + auto-watering)
└── backend/
    ├── package.json  server.js  predict.js
    └── public/                     ← dashboard (index.html, app.js, style.css)
```

> **Hardware upgrade note:** the build now runs on solar + battery with **3 soil probes
> (one per pot), a 12V pump, and a 16x2 LCD**. For wiring, pin assignments, and power
> budget, **SOLAR_WIRING.md is the current reference — it supersedes the pin diagram in §1
> and all of §2 below.** Everything else in this plan (backend, dashboard, prediction,
> Telegram, roadmap, testing) still applies; the code in this folder is already the 3-pot
> version.

---

## 1. System Architecture

Decision: the ESP32 makes its own watering decisions (edge logic), so watering keeps working
even if the laptop/server is off. The backend only stores data and serves the dashboard.
Telegram alerts go directly from ESP32 → Telegram over HTTPS.

```
  ┌────────────┐  AO (analog)   ┌──────────────────────────┐
  │  YL-69     │───────────────►│ GPIO34  (ADC1 - required │
  │  moisture  │                │         when Wi-Fi on)   │
  └────────────┘                │                          │
  ┌────────────┐  data          │         ESP32            │   Wi-Fi HTTP POST (JSON)
  │  DHT11     │───────────────►│  GPIO4                   │──────────────────────────┐
  │  temp/hum  │                │                          │                          │
  └────────────┘                │  GPIO26 ──► relay ──► 5V │                          ▼
                                │           (active-LOW)   │              ┌────────────────────┐
                                │           pump           │              │ Node.js + Express  │
                                │                          │   watering   │ node:sqlite (file) │
                                │  Wi-Fi HTTPS             │   events ──► │                    │
                                └─────────┬────────────────┘────────────►│  REST API + static │
                                          │                              │  dashboard files   │
                                          ▼                              └─────────┬──────────┘
                                ┌────────────────────┐                           │
                                │  Telegram servers  │              phone/laptop │ (same Wi-Fi)
                                │  (Bot API)         │◄──────────────────────────┘
                                └────────────────────┘              browser polls /api/* every 5-30 s
```

Data flow:
1. ESP32 reads sensors every 5 s; POSTs a reading to `POST /api/sensor-data` every 30 s.
2. ESP32 evaluates watering locally (moisture threshold + cooldown + air-condition gate),
   then POSTs each watering event to `POST /api/watering`.
3. ESP32 sends Telegram messages directly on alert transitions (with hysteresis so one dry
   spell = one message, not fifty).
4. Browser dashboard polls the backend and renders cards, chart, watering log, prediction.

## 2. Hardware Wiring Guide

> **Superseded for the current build:** see **SOLAR_WIRING.md** for the solar/battery power
> chain, the 3-probe pin map (GPIO34/35/36), the 12V pump relay, and the I2C LCD. This
> section is kept as the original single-probe reference only.

### Pin assignments (ESP32 DevKit)

| Component | ESP32 pin | Notes |
|---|---|---|
| YL-69 VCC | 3.3V | 3.3V is enough for the AO signal and keeps ADC in range |
| YL-69 GND | GND | |
| YL-69 AO | **GPIO34** | Must be ADC1 (GPIO 32–39). **ADC2 pins fail while Wi-Fi is active** — classic ESP32 gotcha |
| YL-69 DO | (unused) | we use analog AO, not the digital threshold pin |
| DHT11 VCC | 3.3V (VIN ok) | |
| DHT11 GND | GND | |
| DHT11 DATA | **GPIO4** | Modules include the required 10k pull-up to VCC; bare sensors need one added |
| Relay VCC | 5V (VIN) | |
| Relay GND | GND | |
| Relay IN | **GPIO26** | Avoid strapping pins (0, 2, 12, 15) for outputs — GPIO26 is safe |
| Relay COM/NO | pump + and pump supply + | Pump sits on the relay's normally-open contact |

### Pump power — important

- **Do not power the pump from an ESP32 GPIO or the 3.3V rail.** A 5V submersible pump
  draws 200–500 mA+ (spike at start), more than the board can source.
- Simplest option: relay switches a separate 5V supply (USB phone charger or 4xAA pack) that
  feeds the pump. ESP32 ground and pump-supply ground must be tied together.
- Most 1-channel relay boards are **active-LOW**: `digitalWrite(PIN, LOW)` turns the pump ON.
  If yours behaves backwards, swap `RELAY_PUMP_ON` / `RELAY_PUMP_OFF` at the top of the .ino.
- Keep pump wires away from the DHT11 and short the sensor/pump grounds to one point to
  reduce noise on the analog moisture reading.

### Optional light sensor [FULL]

BH1750 (I2C: SDA→GPIO21, SCL→GPIO22) — add to the JSON payload and DB only if you want it
later; not needed for any MVP feature.

## 3. Firmware Plan (`firmware/pechay_watering/pechay_watering.ino`)

### Libraries (Arduino IDE → Library Manager)

- **DHT sensor library** (Adafruit) + its dependency **Adafruit Unified Sensor**
- Built-in: `WiFi`, `HTTPClient`, `WiFiClientSecure` (all included with the ESP32 core)

Board: *ESP32 Dev Module* in Tools → Board. Set Tools → Upload Speed and leave flash defaults.

### Main loop logic (already implemented in the starter file)

```
every 5 s (SENSOR_INTERVAL_MS):
    read moisture  = average of 8 analog samples -> map(raw, WATER, AIR, 100, 0) -> 0-100%
    read DHT11 temp/humidity (track consecutive failures -> sensor-fail alert after 10)
    evaluate alerts with hysteresis -> sendTelegram() on state transitions only
continuously (updateWatering):
    if pump running and duration elapsed -> pump OFF, POST /api/watering (before/after moisture)
    else if moisture < 30% AND cooldown (10 min) passed AND air not extreme -> pump ON for 4 s
every 30 s (SEND_INTERVAL_MS):
    POST /api/sensor-data {moisture, temperature, humidity, pumpOn}
if Wi-Fi dropped: retry connect every 30 s (sensors + watering keep running offline)
```

Design notes:
- **Non-blocking pump timing** via `millis()` state machine — `delay()` would block sensor
  and Wi-Fi handling.
- **Alert hysteresis**: alert fires below 30% moisture, clears at 35%. One message per
  event on entry and one "recovered" message on exit. Same pattern for temp (>35°C / <33°C)
  and humidity (<40% / >45%).
- **Telegram from the ESP32** uses `WiFiClientSecure` with `setInsecure()` (skips cert
  checks). Fine for a hobby prototype; [FULL] would relay through the backend with proper certs.
- The server timestamps everything, so no NTP needed on the device [FULL].

### YL-69 calibration procedure [MVP]

1. Upload `firmware/calibrate_moisture/calibrate_moisture.ino`, open Serial Monitor at 115200.
2. Hold the probe **dry in air** for ~10 s, note the raw value → put it in `MOISTURE_RAW_AIR`.
3. Push the probe into a cup of **water** (up to the marked line), note the raw value →
   `MOISTURE_RAW_WATER`. (Typical: air ≈ 2900–3400, water ≈ 900–1400 at 3.3V.)
4. Re-upload the main sketch. If percent reads backwards (100% in air), swap the two constants.

Notes: the ESP32 ADC is nonlinear and noisy — that's why the code averages 8 samples. For a
prototype, ±5% accuracy is fine. The probe also slowly corrodes if left powered constantly
[FULL: power it from a GPIO to only enable during reads].

## 4. Backend Plan (`backend/`)

Stack: **Node.js + Express + node:sqlite** (SQLite built into Node ≥22.12 — zero native
dependencies, no build tools, one data file `pechay.db`). The backend runs on your laptop;
dashboard and ESP32 must be on the same Wi-Fi/network.

```bash
cd backend
npm install
npm start          # serves dashboard at http://localhost:3000
```

### API endpoints (all implemented in `server.js`)

| Method | Path | Purpose | Caller |
|---|---|---|---|
| POST | `/api/sensor-data` | `{moisture, temperature, humidity, pumpOn}` → store reading | ESP32 |
| GET | `/api/latest` | newest reading + last watering + days since planting | dashboard |
| GET | `/api/history?hours=6` | readings for the chart (max 7 days, capped 5000 rows) | dashboard |
| POST | `/api/watering` | `{duration_ms, moisture_before, moisture_after}` → store event | ESP32 |
| GET | `/api/watering-log` | last 50 watering events, newest first | dashboard |
| POST | `/api/config` | `{planting_date: "YYYY-MM-DD"}` → set/change planting date | dashboard |
| GET | `/api/predict` | rule-based yield estimate (see §7) | dashboard |

### Database schema (auto-created on first run)

```sql
sensor_readings(id INTEGER PK, ts INTEGER (epoch ms), moisture REAL, temperature REAL,
                humidity REAL, pump_on INTEGER)
watering_events(id INTEGER PK, ts INTEGER, duration_ms INTEGER,
                moisture_before REAL, moisture_after REAL)
config(key TEXT PK, value TEXT)        -- holds planting_date
```

Day 1 = planting day; `days_since_planting = floor(now - planting_date) + 1`, computed at
query time so changing the date needs no data migration.

## 5. Web Dashboard Plan (`backend/public/`)

Already built as a single-page dark-green dashboard:

- **Top cards** (poll `/api/latest` every 5 s): soil moisture (with % bar), temperature,
  humidity, days-since-planting card with a date picker that POSTs `/api/config`, and
  last-watering summary.
- **Prediction panel** (poll every 30 s): estimated grams/plant, category badge
  (Low/Medium/High), per-factor score bars, growth stage, and explanatory note.
- **History chart**: Chart.js 4 (CDN) line chart, datasets for moisture/temperature/humidity
  on one shared 0–100 axis (temp °C sits naturally inside that range — one axis is simpler
  than dual axes for a prototype). Range selector (1 h / 6 h / 24 h / 3 d) hits `/api/history`.
- **Watering log table** (poll every 30 s): time, pump duration, moisture before → after.

Refresh strategy: live cards 5 s, everything else 30 s, chart redraws without animation.
Works on a phone browser on the same Wi-Fi — no app needed.

## 6. Notification Integration — Telegram (recommended)

**Why Telegram**: free, no quota to speak of, bot setup takes 5 minutes, messages arrive
anywhere (not just the LAN, unlike a local-only dashboard), and the ESP32 can call it
directly. Pushover is also easy but caps free usage at a week; Blynk locks features behind
its app/account system.

Setup:
1. In Telegram, open **@BotFather** → `/newbot` → name it → copy the **token** it gives you.
2. Start a chat with your new bot, send it any message (e.g. "hi").
3. Open `https://api.telegram.org/bot<YOUR_TOKEN>/getUpdates` in a browser, find
   `"chat":{"id":123456789` → that's your **chat ID** (negative number is fine, use it as-is).
4. Put token + chat ID into `BOT_TOKEN` / `CHAT_ID` in the .ino and re-upload.
5. Test: unplug the moisture probe or breathe on the DHT11 to force a threshold crossing —
   you should get exactly one message, and a "recovered" message when it clears.

Trigger conditions and message format (all implemented):
- `Soil moisture is 24% (below 30%).` → clear at 35%
- `Temperature is 36.4C (above 35.0C).` → clear at 33°C
- `Humidity is 37.0% (below 40.0%).` → clear at 45%
- `DHT11 sensor is not responding.` → sensor-fail alert (fires once, not per failed sample)

## 7. Yield Prediction Logic (`backend/predict.js`)

Transparent rule-based model — easy to tune and explain in a report. It scores the **average
conditions since planting** against optimal ranges, then scales a base yield by maturity.

```
maturityFactor(d)  = clamp((days - 1) / 34, 0, 1)        # 0 on day 1 → 1.0 at day 35 (harvest window)

moistureScore(m):   1.0 in [60,70]                        # optimal soil moisture 60-70%
                    linear 0→1 from 20%→60% below band
                    linear 1→0 from 70%→95% above band
tempScore(t):       1.0 in [18,22] °C                     # linear falloff outside, 0 at 5°C / 38°C
humidityScore(h):   1.0 in [60,80] %                      # linear falloff outside, 0 at 30% / 100%

conditionIndex = 0.5·moistureScore + 0.25·tempScore + 0.25·humidityScore   # moisture weighted 2×
estimatedYield = BASE_YIELD_G (200 g) × maturityFactor × conditionIndex
category       = High (≥0.85) / Medium (≥0.60) / Low (<0.60)   ← based on conditionIndex
```

Why these weights: moisture is the variable this system actively manages, so it dominates;
temperature and humidity modulate. `BASE_YIELD_G` (≈150–250 g for pechay) is the one
constant to tune against your variety — measure a real harvest and adjust.

Growth stage shown alongside: Days 1–7 Germination, 8–14 Seedling, 15–29 Vegetative,
30–45 Maturity/harvest, >45 Overdue (bolting risk).

Example `/api/predict` output:

```json
{
  "ok": true,
  "days_since_planting": 22,
  "growth_stage": "Vegetative growth",
  "averages": { "moisture_pct": 58.2, "temperature_c": 27.6, "humidity_pct": 71.3, "readings_count": 1214 },
  "scores": { "moisture": 0.96, "temperature": 0.65, "humidity": 1.0 },
  "condition_index": 0.89,
  "base_yield_g": 200,
  "maturity_factor": 0.62,
  "estimated_yield_g_per_plant": 110,
  "yield_category": "High",
  "note": "..."
}
```

[FULL] upgrade path: keep logging readings per crop cycle, record actual harvest grams in a
`harvests` table, then fit a linear regression (moisture, temp, humidity, days) with
`ml-regression` or even Excel — same API shape.

## 8. Implementation Roadmap (Phased)

| Phase | Goal | Effort | What "done" looks like |
|---|---|---|---|
| **1** | Sensors to Serial Monitor [MVP] | ~1 h | Calibration sketch values captured; main .ino prints moisture %, temp, humidity every 5 s over USB |
| **2** | Auto-watering [MVP] | ~1–2 h | Dry soil (unplug probe or hold dry sponge) triggers pump 4 s; second trigger within 10 min is ignored; serial shows each cycle |
| **3** | Backend + dashboard [MVP] | ~2 h | `npm install && npm start`; laptop IP set in .ino; cards populate live; chart draws; planting date saves |
| **4** | Telegram notifications [MVP] | ~30 min | BotFather token + chat ID in .ino; each threshold crossing sends exactly one alert + one recovery message |
| **5** | Yield prediction [MVP] | ~30 min | Prediction panel shows grams + category consistent with the averages and stage |
| **6** | Testing & calibration [MVP] | ~2 h | Full pass of the §9 checklist |

**MVP = end of Phase 6.** Roughly a weekend of work, in this order — each phase builds on the
last and is testable on its own. [FULL] candidates afterwards: light sensor, OTA updates,
NTP timestamps on device, regression model trained on real harvests, data export, HTTPS,
pump moisture-soak delay before "after" reading, multi-plant zones.

## 9. Testing Checklist

**Per component**
- [ ] YL-69: dry = ~0–10%, in water = ~95–100%, wet soil lands 40–80% (re-run calibration if not)
- [ ] DHT11: readings plausible vs. a room thermometer; breathing on it raises humidity fast
- [ ] Relay: clicking sound on switch; **test pump OFF at boot first** (before connecting water)
- [ ] Pump: lifts water in a cup test; tub lines not kinked; no leaks over the electronics

**Logic**
- [ ] Sensor-to-dashboard path: with ONLY the two sensors wired (no pump), `firmware/test_sensors_dashboard/` POSTs readings every 10 s and the dashboard cards/chart update
- [ ] Watering fires below 30% and not at/above 30%
- [ ] Cooldown: force-dry twice within 10 min → exactly one cycle
- [ ] Watering blocked when DHT11 disconnected (air-extreme gate uses valid readings)
- [ ] Telegram: one alert on threshold crossing, one recovery message, silence in between
- [ ] Dashboard: numbers match Serial Monitor; chart advances; watering events appear in log table

**Edge cases**
- [ ] Wi-Fi router rebooted mid-run → ESP32 reconnects within ~30 s, no crash, no double watering
- [ ] Backend laptop closed/asleep → ESP32 keeps watering locally; queued data is simply skipped (acceptable for prototype)
- [ ] DHT11 unplugged → "sensor not responding" alert once, recovers when re-plugged
- [ ] Planting date changed on dashboard → days counter and prediction update on next poll
- [ ] Prediction with <1 h of data → panel shows the "rough guess" note instead of a bogus precise number
- [ ] Pump runs dry / blocked → cooldown still prevents rapid re-triggering (watch the 10-min window)

## 10. Scope Reminder — 50% Prototype vs. Full System

**Required for the MVP (must all work end-to-end):**
1. Calibrated sensor readings visible on the dashboard (Phases 1, 3)
2. Threshold auto-watering with cooldown + event log (Phase 2)
3. Telegram alerts for moisture/temperature/humidity crossings (Phase 4)
4. Planting date → days/stage + rule-based yield estimate on the dashboard (Phase 5)
5. Basic pass of the testing checklist (Phase 6)

**Explicitly out of scope for the MVP (fine to skip):**
- Machine learning / trained models (the rule-based scorer stands in)
- Access from outside the home Wi-Fi, logins, multi-user, HTTPS for the dashboard
- OTA firmware updates, NTP time sync, data export, cloud hosting
- Flow meter / total-water tracking in the prediction, light sensor, camera
- Polished UI (current dashboard is functional, not pretty), mobile app
- Production hardening: cert-verified Telegram calls, corroded-probe mitigation, UPS handling

If the five MVP items above are true, the prototype requirement is met. Everything else is
polish — add only what a demo or report actually needs.
