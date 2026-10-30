// Rule-based pechay yield prediction.
// Scores the average growing conditions since planting against known optimal ranges,
// then scales a base yield by growth maturity. Transparent on purpose - no black box.

const BASE_YIELD_G = 200; // typical harvestable pechay per plant (30-45 days); tune for your variety

function clamp01(v) {
  return Math.min(1, Math.max(0, v));
}

// Ramps 0 at day 1 -> 1.0 at day 35 (start of the maturity/harvest window), stays 1.0 after.
function maturityFactor(days) {
  if (days == null) return null;
  return clamp01((days - 1) / 34);
}

// Optimal soil moisture: 60-70%. Score falls off linearly outside the band.
function moistureScore(avg) {
  if (avg >= 60 && avg <= 70) return 1;
  if (avg < 60) return clamp01((avg - 20) / 40);
  return clamp01(1 - (avg - 70) / 25);
}

// Optimal temperature: 18-22 C.
function tempScore(avg) {
  if (avg >= 18 && avg <= 22) return 1;
  if (avg < 18) return clamp01((avg - 5) / 13);
  return clamp01(1 - (avg - 22) / 16);
}

// Optimal humidity: 60-80%.
function humidityScore(avg) {
  if (avg >= 60 && avg <= 80) return 1;
  if (avg < 60) return clamp01((avg - 30) / 30);
  return clamp01(1 - (avg - 80) / 20);
}

function growthStage(days) {
  if (days == null) return 'Unknown (set planting date)';
  if (days <= 7) return 'Germination';
  if (days <= 14) return 'Seedling';
  if (days <= 29) return 'Vegetative growth';
  if (days <= 45) return 'Maturity / harvest window';
  return 'Past harvest window (bolting risk)';
}

// db: node:sqlite DatabaseSync. days: days since planting (or null). sinceMs: planting date as epoch ms (or null).
function predictYield(db, days, sinceMs) {
  let windowStart = sinceMs;
  let windowNote = 'Averages cover the full period since planting.';
  if (windowStart == null) {
    windowStart = Date.now() - 7 * 86400000;
    windowNote = 'No planting date set - averages cover only the last 7 days. Set the planting date for a full-period estimate.';
  }

  // Moisture is averaged across the 3 pots, then across the whole prediction window.
  const row = db.prepare(
    'SELECT AVG((moisture1 + moisture2 + moisture3) / 3.0) AS m, AVG(temperature) AS t, AVG(humidity) AS h, COUNT(*) AS n FROM sensor_readings WHERE ts >= ?'
  ).get(windowStart);

  if (!row || row.n === 0) {
    return { ok: false, error: 'No sensor data in the prediction window yet.', note: windowNote };
  }

  const scores = {
    moisture: Math.round(moistureScore(row.m) * 100) / 100,
    temperature: Math.round(tempScore(row.t) * 100) / 100,
    humidity: Math.round(humidityScore(row.h) * 100) / 100,
  };

  // Moisture is weighted highest - it is what the whole watering system manages.
  const conditionIndex = Math.round(
    (0.5 * scores.moisture + 0.25 * scores.temperature + 0.25 * scores.humidity) * 100
  ) / 100;

  const mat = maturityFactor(days);
  const estimatedYieldG = mat == null ? null : Math.max(0, Math.round(BASE_YIELD_G * mat * conditionIndex));

  const category = conditionIndex >= 0.85 ? 'High' : conditionIndex >= 0.6 ? 'Medium' : 'Low';

  let note = windowNote;
  if (row.n < 50) note += ' Fewer than 50 readings in the window - treat this as a rough guess.';
  if (days != null && days < 15) note += ' Plant is very young; estimates this early are unreliable.';

  return {
    ok: true,
    days_since_planting: days,
    growth_stage: growthStage(days),
    averages: {
      moisture_pct: Math.round(row.m * 10) / 10,
      temperature_c: Math.round(row.t * 10) / 10,
      humidity_pct: Math.round(row.h * 10) / 10,
      readings_count: row.n,
    },
    scores,
    condition_index: conditionIndex,
    base_yield_g: BASE_YIELD_G,
    maturity_factor: mat,
    estimated_yield_g_per_plant: estimatedYieldG,
    yield_category: category,
    note,
  };
}

module.exports = { predictYield, growthStage };
