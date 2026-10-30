# Solar + 3-Pot Wiring Guide

Plain-language wiring for the upgraded setup: solar by day, battery by night, 3 pechay pots
(1 probe each), a 12V pump, and a 16x2 LCD on the ESP32. The web dashboard keeps working
exactly as before — it just shows 3 pots now.

> **This file replaces the wiring table in IMPLEMENTATION_PLAN.md §2.** Everything else in
> the plan (backend, dashboard, prediction, Telegram) still applies.

## Your parts

| Part | Role |
|---|---|
| Solar panel 12W 18V (Voc 24.3V) | Charges the battery in daylight |
| Charge controller 12V/10A | Protects the battery while charging (specs to check: SOLAR_SETUP.md) |
| 3S2P 18650 pack 12V with BMS | Stores energy, powers everything at night |
| DC-DC buck converter | Steps 12V down to 5V for the electronics |
| ESP32 | Brain: reads sensors, decides watering, talks to Wi-Fi |
| 5V 1-channel relay | On/off switch for the pump |
| 12V brushless pump | Moves water to the pots (your pipe design) |
| DHT11 | Air temperature + humidity |
| 3x YL-69 | Soil moisture, one per pot |
| 16x2 LCD | Shows readings right at the plant, no phone needed |

## The big picture

```
 Solar panel (18V)
      │
      ▼
 Charge controller ──────► 12V Li-Ion battery pack
                                │
                 ┌──────────────┴──────────────┐
                 ▼                             ▼
          Buck converter                 Relay (COM/NO)
          12V → 5V                            │
                 │                            ▼
      ┌──────────┼──────────┐            12V pump
      ▼          ▼          ▼            (your pipes → 3 pots)
    ESP32     Relay VCC   DHT11 VCC
      │
      ├── 3.3V pin ──► 3x YL-69 VCC
      ├── GPIO34/35/39 ──► YL-69 AO (pot 1/2/3)
      ├── GPIO33 ──► DHT11 data
      ├── GPIO26 ──► Relay IN
      └── GPIO21/22 ──► LCD (SDA/SCL)
```

**One rule to remember: every GND is connected together** — battery (−), buck output GND,
ESP32 GND, relay GND, DHT11 GND, all 3 YL-69 GNDs, pump (−). Without a common ground
nothing works reliably.

---

## Step 1 — Set the buck converter to 5.0V (do this FIRST, before anything else)

1. Connect buck input to the battery (or any 12V source). Do NOT connect the ESP32 yet.
2. Multimeter on DC volts, probes on the buck's OUT + and OUT −.
3. Turn the little blue screw on the buck until the meter reads **5.0V** (4.9–5.1 is fine).
4. Power off. From now on the buck's 5V output feeds the ESP32 and relay.

⚠️ Never connect the ESP32 to an unadjusted buck — if it's set to 9V or 12V you can fry the board.

## Step 2 — Solar → charge controller → battery

Wire in this order: **battery first, then solar panel** (most cheap controllers get confused
or damaged if the panel goes in first).

| Charge controller terminal | Connect to |
|---|---|
| Battery + / − | Battery pack + / − |
| Solar + / − | Panel + / − (only in daylight is fine too) |

If your controller has a battery-type menu, choose **LI / Li-ion**. If it doesn't have that
option, leave the default — good enough for a prototype.

## Step 3 — Battery → buck → 5V rail

| From | To |
|---|---|
| Battery + | Buck IN+ |
| Battery − | Buck IN− |
| Buck OUT+ (5.0V) | ESP32 **5V pin**, Relay **VCC**, DHT11 **VCC** |
| Buck OUT− | ESP32 **GND**, Relay **GND**, DHT11 **GND** (common ground) |

The ESP32's onboard regulator turns that 5V into the 3.3V the chip needs.

## Step 4 — The 3 soil moisture probes

Power them from the ESP32's **3.3V pin** (not 5V) so their analog output stays inside the
ESP32's safe 0–3.3V measuring range.

| YL-69 pin | Connect to |
|---|---|
| VCC (all 3) | ESP32 **3.3V** |
| GND (all 3) | ESP32 **GND** |
| AO of probe 1 | **GPIO34** |
| AO of probe 2 | **GPIO35** |
| AO of probe 3 | **GPIO39** |

Why 34/35/39: they're input-only pins built for analog readings, and they're in the ADC1
group — **the only analog pins that work while Wi-Fi is on**. (On some boards they're labeled
IO34/IO35/VN — VN is GPIO39. GPIO36/VP works too if your board has it, just move the wire
and change the number in the sketch.) Don't use the DO pin on the probes; we read the analog AO.

## Step 5 — DHT11 and relay (unchanged from before)

| DHT11 pin | Connect to |
|---|---|
| VCC | 5V rail (buck OUT+) |
| GND | common GND |
| DATA | **GPIO33** (module has the pull-up built in) |

GPIO33 is the pin your working sensor test already used, so leave the wire where it is.
If you move it, change `PIN_DHT` at the top of the sketch to match.

| Relay pin | Connect to |
|---|---|
| VCC | 5V rail |
| GND | common GND |
| IN | **GPIO26** |

## Step 6 — 12V pump through the relay

The pump runs straight off the battery — the relay just switches it on and off.

| From | To |
|---|---|
| Battery + | Relay **COM** |
| Relay **NO** | Pump + |
| Pump − | Battery − (common ground) |

Most relay boards are **active-LOW**: the firmware already treats LOW = pump ON. If your pump
runs backwards (on at boot, off when watering), swap `RELAY_PUMP_ON`/`RELAY_PUMP_OFF` in the sketch.

## Step 7 — 16x2 LCD (I2C version)

This assumes the LCD has the little black "backpack" board soldered on the back with only
4 pins (GND, VCC, SDA, SCL). That's the version sold in almost every kit.

| LCD pin | Connect to |
|---|---|
| GND | common GND |
| VCC | 5V rail |
| SDA | **GPIO21** |
| SCL | **GPIO22** |

Two things beginners always hit:
- **Blank screen?** Turn the small blue trimmer on the backpack — that's contrast, and
  out-of-the-box it's usually set so nothing is visible.
- If it's still blank after that, the I2C address might be `0x3F` instead of `0x27` —
  change it in the sketch (`LiquidCrystal_I2C lcd(0x3F, 16, 2);`).

> If your LCD has 16 pins and NO backpack board, tell me — the wiring and library differ
> and I'll adjust the sketch.

Before the sketch will compile, install the LCD library once: Arduino IDE → Tools → Manage
Libraries → search **"LiquidCrystal I2C"** (by Frank de Brabander) → Install.

---

## Pin summary (tape this inside your enclosure)

| ESP32 pin | Device |
|---|---|
| 5V | buck 5V in |
| GND | common ground |
| 3.3V | 3x YL-69 VCC |
| GPIO34 | YL-69 pot 1 (AO) |
| GPIO35 | YL-69 pot 2 (AO) |
| GPIO39 | YL-69 pot 3 (AO) — labeled VN on many boards |
| GPIO33 | DHT11 data |
| GPIO26 | relay IN |
| GPIO21 | LCD SDA |
| GPIO22 | LCD SCL |

## Will the battery last? (short answer: yes, easily)

- ESP32 + LCD + sensors together sip about **0.6–0.8W** — roughly **15–19Wh per day**.
- The pump only runs a few seconds at a time: about **1Wh per day**, negligible.
- Your 12W panel in decent sun makes **~35–45Wh per day** — 2x what you use.
- The 3S2P pack really holds **~70Wh** (despite the 12,800mAh label), so even with zero
  sun it runs **~4 days**, and one sunny day refills it.

You have comfortable margin. No need for deep-sleep tricks or power management.

## Wi-Fi setup mode (no code editing anymore)

The main sketch no longer stores your Wi-Fi password or the laptop's IP. Instead:

1. **First boot after uploading:** the ESP32 opens a hotspot called **Pechay-Setup**.
2. Join it from your phone — a setup page pops up by itself (if it doesn't, open
   `http://192.168.4.1` in the browser).
3. Pick your home Wi-Fi from the list, type its password, and type your laptop's IP
   (run `ipconfig` on the laptop — the IPv4 Address, e.g. `192.168.1.6`).
4. Tap **Save & restart**. The ESP32 restarts, joins your Wi-Fi, and remembers these
   settings permanently (stored on the chip, surviving power loss).

**To change the settings later** (new Wi-Fi, different laptop IP): hold the **BOOT**
button (GPIO0) for ~4 seconds — the hotspot opens again. Note: press and hold it a few
seconds *after* the ESP32 has already booted; holding it while powering on puts the board
into flashing mode instead.

## First power-on test order (do it in this sequence)

1. **Buck alone** — meter reads 5.0V (Step 1).
2. **ESP32 + LCD only** — upload the main sketch. On first boot the LCD shows
   "SETUP MODE / Join Pechay-Set": do the phone setup above, and the LCD switches to
   readings once Wi-Fi connects. No sensors yet is fine (values will look wrong).
3. **Add probes** — upload `calibrate_moisture` sketch. AIR value = probe held dry in air;
   WATER value = probe in that pot's soil **right after a thorough watering** (not a cup of
   water - wet soil conducts better, and a cup-based value pins every soil reading at 100%).
   Put the numbers into the main sketch's `MOISTURE_RAW_AIR` / `MOISTURE_RAW_WATER`
   arrays (one value per pot — they differ a little probe to probe).
4. **Sensors → dashboard** — upload `test_sensors_dashboard`; watch all 3 pots appear on
   `http://localhost:3000` every 10 s.
5. **Add relay + pump LAST** — dry-run first: with the pump in a bucket of water (not in the
   pipes), force a low reading (pull a probe out of soil) and confirm the pump runs ~4 s,
   stops, and won't re-run for 10 minutes (cooldown).
6. Connect your pipe design and do a real watering test on all 3 pots.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| LCD blank | Contrast trimmer (turn it!) or wrong I2C address (try 0x3F) |
| ESP32 won't boot / restarts | Buck above 5.1V, or grounds not common, or probe wired to a wrong pin |
| One pot reads 0% or 100% always | AO wire loose, or that pot's calibration values not set. 100% in soil usually means the WATER value came from a cup of water — recalibrate in wet soil instead |
| Readings jump around | Normal for YL-69 — the firmware averages 8 samples; keep probe wires away from pump wires |
| Pump never runs | Relay polarity (try swapping RELAY_PUMP_ON/OFF), or moisture above threshold, or cooldown still active |
| Pump runs nonstop | Active-LOW polarity mismatch, or NO/NC swapped on the relay (use NO) |
| Dashboard shows no data | Join Pechay-Setup (hold BOOT ~4 s) and re-check Wi-Fi password + laptop IP, or backend not running on the laptop |

## Safety notes (quick)

- Never short the battery terminals — 18650 packs deliver huge current. Tape exposed metal.
- Double-check polarity before first power-on; reverse polarity is the #1 board killer.
- Keep the panel wiring away from water; the enclosure for ESP32/relay should be splash-proof
  or indoors with only the pump/probes outside.
- If the battery has no protection board (BMS), don't let the controller drain it fully —
  most 3S packs include one, but check.
- [Optional, recommended] A 2A inline fuse on the battery → pump line protects everything
  if the pump ever jams.

## What changed in the code (already updated for you)

- Firmware now reads **3 probes** (GPIO34/35/36), shows them on the **LCD**, and waters when
  the **driest pot** drops below 30% — one pump feeds all pots through your pipes.
- Telegram alerts name which pot is dry.
- Dashboard shows **3 moisture cards**, a chart with all 3 pots, and the watering log records
  which pot triggered each cycle.
- Yield prediction averages moisture across all 3 pots — same formula, same weights.
- Old `pechay.db` data is migrated automatically on first run of the new backend (old
  readings get copied into all 3 pot columns). If you'd rather start clean, stop the server
  and delete `backend/pechay.db`.
