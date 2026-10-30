// YL-69 calibration helper - 3 probes (one per pechay pot)
// Upload this first. Open Serial Monitor (115200 baud).
//
// For EACH of the 3 probes:
//   1. Hold it DRY in open air for ~10 s -> note its raw value = that pot's AIR value.
//   2. Water the pots thoroughly, wait ~1 min, then push the probe into the WET SOIL of
//      its pot -> note its raw value = WATER value.
//      IMPORTANT: wet soil, not a cup of water! Fertilized soil conducts electricity
//      better than plain water, so a cup-based value makes every soil reading pin at
//      100% the moment the probe enters the pot.
//   3. Type both numbers into pechay_watering.ino AND test_sensors_dashboard.ino, in the
//      MOISTURE_RAW_AIR / MOISTURE_RAW_WATER arrays (one pair per pot).
//
// Probes differ a little from each other, so calibrate all three separately - otherwise
// one pot can read 20% higher than its neighbour even in the same soil.
//
// Wiring (see SOLAR_WIRING.md step 4):
//   each YL-69 VCC -> ESP32 3.3V, GND -> ESP32 GND
//   AO of pot 1 -> GPIO34, pot 2 -> GPIO35, pot 3 -> GPIO39
//   Do NOT use the probe's DO pin for this project - we read the analog AO.

#define NUM_POTS 3
const int PIN_MOIST[NUM_POTS] = { 34, 35, 39 };

int readRaw(int pin) {
  long sum = 0;
  for (int i = 0; i < 8; i++) {   // average 8 samples to smooth out the noise
    sum += analogRead(pin);
    delay(20);
  }
  return sum / 8;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("YL-69 calibration starting (3 pots)...");
  Serial.println("Leave all probes in dry air for 10 seconds, then watch the raw values.");
}

void loop() {
  Serial.print("P1 raw = "); Serial.print(readRaw(PIN_MOIST[0]));
  Serial.print("   |   P2 raw = "); Serial.print(readRaw(PIN_MOIST[1]));
  Serial.print("   |   P3 raw = "); Serial.print(readRaw(PIN_MOIST[2]));
  Serial.println("     (0-4095 scale: high = dry air, low = water)");
  delay(1000);
}
