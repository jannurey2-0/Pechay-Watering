# Solar Power Setup — Your Parts + The Charger You Need

This file lists your **actual** panel and battery numbers and the exact specs to look for
in a solar charge controller. The step-by-step wiring is still in **SOLAR_WIRING.md** —
nothing there changes.

---

## 1. Your solar panel (measured specs)

| Spec | Value | What it means for you |
|---|---|---|
| Max power | 12 W | How much it makes in full sun |
| Voltage at max power (Vmp) | 18 V | Normal working voltage — needs a 12V controller to step it down |
| Current at max power (Imp) | 0.6 A | Normal working current |
| Open-circuit voltage (Voc) | 24.3 V | Voltage with nothing attached — **this is what your controller must survive** |
| Short-circuit current (Isc) | 0.7 A | Absolute max current it can produce |

## 2. Your battery

**3S2P 18650 pack, 12V, labeled 12,800 mAh, with BMS.**

- A "12V" 3-cell li-ion pack is actually **12.6 V when full** and **~9 V when empty**.
- One honest note: 12,800 mAh is the marketing number. Real 18650 cells in a 3S2P pack
  hold roughly **6,500–7,000 mAh ≈ 70 Wh**. Plan around 70 Wh and you'll never be surprised.
- The **BMS** (protection board) stops over-discharge and shorts — keep it, it's doing
  a real job.

## 3. The solar charger (charge controller) you need

If you already have a 12V/10A controller, check it against this list — it almost certainly
passes and you don't need to buy anything. If you're buying one, these are the specs:

| Spec to check | You need | Why |
|---|---|---|
| System voltage | **12 V** (or a 12V/24V auto-detect model) | Matches your battery |
| Type | **PWM** is fine | Your panel is small; MPPT costs more for almost no gain at 12 W |
| Charge current | **5 A is enough**; 10 A is the common cheap size | Your panel only produces 0.6 A |
| Max solar input voltage | **≥ 25 V, ideally 30 V+** | Panel Voc is 24.3 V and goes *higher* on cold mornings — a controller rated only to 20 V could die |
| Battery type setting | **Li-ion / LI** option | Tells it the right charge voltage for a 3S pack |
| Load output terminals | Nice bonus, not required | Some controllers cut the load when the battery gets low — free extra protection |

**Don't pay extra for:** MPPT, Bluetooth apps, 20A+ ratings, 48V support. None of it helps here.

### Your actual controller (already owned — confirmed OK)

PWM, 12V/24V, max solar input 24V (12V mode), 10A discharge, supports
Lead-acid / Li-ion / LiFePO4. Settings to use: **12V**, battery type **LI (Li-ion)**,
charging current **10A**. The load output (10A) can carry the buck converter and pump,
which also gives you automatic low-battery cut-off.

One flag: its max solar input is 24 V while your panel's open-circuit is 24.3 V — slightly
over on paper, fine in practice for a PWM unit (its internal switch is rated well above
that), and panel voltage only climbs in cold weather. If it ever refuses to charge in
strong sun, that's the one spec to upgrade (look for 30 V+ input).

If shopping, the search term is: *"PWM solar charge controller 12V 10A lithium"* —
it's a very common, cheap part.

## 4. Buck converter (12V → 5V for the ESP32)

| Spec to check | You need |
|---|---|
| Output voltage | Adjustable — you set it to **5.0 V** before connecting anything |
| Output current | **2 A or more** |
| Input range | Must cover **9–14 V** (most wide-input bucks handle 6–24 V+) |

## 5. Why this comfortably runs the system

| | Per day |
|---|---|
| ESP32 running 24/7 | ~10 Wh |
| LCD backlight | ~5 Wh |
| Sensors + relay idle | ~1 Wh |
| Pump (few seconds at a time) | under 0.5 Wh |
| **Total use** | **~16 Wh** |
| **Panel harvest on a normal sunny day** | **~35–45 Wh** |
| **Battery backup, zero sun** | **~4 days** (real ~70 Wh) |

A sunny day runs everything and refills the battery. A fully overcast day roughly
breaks even. Several cloudy days in a row are covered by the battery.

## 6. Assembly order (same as SOLAR_WIRING.md)

1. **Set the buck converter to 5.0 V first** — before the ESP32 touches it.
2. Wire **battery → controller**, then **panel → controller** (battery first matters).
3. Wire **battery → buck → 5V rail** (ESP32, relay, DHT11, LCD).
4. Follow SOLAR_WIRING.md steps 4–7 for the probes, DHT11, relay + pump, and LCD.

## 7. Two things worth checking on day one

- On a sunny noon, most 12V lithium controllers show ~12.3–12.6 V on the battery while
  charging — that's the panel doing its job.
- After dark, the battery alone should sit around 11–12 V. If it drops near 9 V with no
  sun for days, the controller's low-voltage cut-off (or the BMS) will protect the pack —
  the system just rests until the sun returns.
