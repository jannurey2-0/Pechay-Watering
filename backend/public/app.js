// Polls the backend and renders the dashboard.
// All state classes/labels live here so the visual language stays in one place.

const POLL_LATEST_MS = 5000;    // live cards
const POLL_LOG_MS = 5000;       // watering events
const POLL_SLOW_MS = 10000;     // chart and prediction

const POTS = [1, 2, 3];
const POT_PINS = { 1: 'GPIO34', 2: 'GPIO35', 3: 'GPIO36' };

// Mirrors growthStage() in backend/predict.js — day spans drive the timeline rail too.
const STAGES = [
  { key: 'germination', label: 'Germination', from: 1, to: 7 },
  { key: 'seedling', label: 'Seedling', from: 8, to: 14 },
  { key: 'vegetative', label: 'Vegetative growth', from: 15, to: 29 },
  { key: 'maturity', label: 'Maturity / harvest window', from: 30, to: 45 },
];
const CYCLE_DAYS = 45;

const SERIES = [
  { key: 'moisture1', label: 'Pot 1 moisture', color: '#4ec96f', unit: '%' },
  { key: 'moisture2', label: 'Pot 2 moisture', color: '#a3d46a', unit: '%' },
  { key: 'moisture3', label: 'Pot 3 moisture', color: '#3fbfa8', unit: '%' },
  { key: 'temperature', label: 'Temperature', color: '#ef9f3c', unit: '°C' },
  { key: 'humidity', label: 'Humidity', color: '#5aa9e0', unit: '%' },
];

let chart;
let lastDataAt = null;

const $ = (id) => document.getElementById(id);

function fmtTime(ts) {
  return ts ? new Date(ts).toLocaleString() : '--';
}

function relTime(ts) {
  if (!ts) return '';
  const s = Math.max(0, Math.round((Date.now() - ts) / 1000));
  if (s < 60) return s + 's ago';
  const m = Math.round(s / 60);
  if (m < 60) return m + ' min ago';
  const h = Math.round(m / 60);
  if (h < 24) return h + ' h ago';
  return Math.round(h / 24) + ' d ago';
}

// Reads can be null before the first POST arrives; every renderer handles that.
function num(v) {
  return typeof v === 'number' && Number.isFinite(v) ? v : null;
}

function setChip(el, text, state) {
  el.textContent = text;
  el.dataset.state = state;
}

function setConn(ok, text) {
  $('conn-status').textContent = text;
  $('conn-pill').dataset.state = ok ? 'ok' : 'error';
}

// ------------------------------------------------------------------ states
function moistureState(v) {
  if (v == null) return ['nodata', 'no data'];
  if (v < 30) return ['thirsty', 'thirsty'];
  if (v < 45) return ['dry', 'getting dry'];
  if (v < 60) return ['okay', 'okay'];
  if (v <= 70) return ['ideal', 'ideal'];
  if (v <= 84) return ['wet', 'wet'];
  return ['soaked', 'waterlogged'];
}

// TEMP_WATER_BLOCK = 40 C in the firmware: above it the ESP32 will not run the pump.
function tempState(t) {
  if (t == null) return ['nodata', 'no data', 'Waiting for the DHT11 to report.'];
  if (t >= 40) return ['alert', 'pump locked', '<b>40 °C or hotter</b> — the ESP32 skips watering to protect the roots.'];
  if (t > 35) return ['alert', 'heat alert', '<b>Above 35 °C</b> — this is what triggers a Telegram alert.'];
  if (t >= 18 && t <= 22) return ['good', 'ideal', 'Inside the ideal 18–22 °C band for pechay.'];
  if (t < 18) return ['info', 'cool', 'Cooler than ideal — growth slows, but the plant is safe.'];
  return ['warn', 'warm', 'Warmer than pechay likes. Watch for wilting around midday.'];
}

// HUMIDITY_WATER_BLOCK_LOW = 30 %, HUMIDITY_WATER_BLOCK_HIGH = 95 % in the firmware.
function humState(h) {
  if (h == null) return ['nodata', 'no data', 'Waiting for the DHT11 to report.'];
  if (h > 95) return ['alert', 'pump locked', '<b>Above 95 %</b> — the air is saturated, so watering is skipped.'];
  if (h < 30) return ['alert', 'pump locked', '<b>Below 30 %</b> — the ESP32 skips watering in air this dry.'];
  if (h < 40) return ['alert', 'dry air', '<b>Below 40 %</b> — this is what triggers a Telegram alert.'];
  if (h >= 60 && h <= 80) return ['good', 'ideal', 'Inside the ideal 60–80 % band for pechay.'];
  if (h < 60) return ['warn', 'low', 'Drier than ideal — seedlings lose water fast in dry air.'];
  return ['warn', 'muggy', 'Wetter than ideal — good air flow helps prevent rot.'];
}

// ------------------------------------------------------------------ latest
async function refreshLatest() {
  let d;
  try {
    d = await (await fetch('/api/latest')).json();
  } catch (e) {
    setConn(false, 'backend unreachable');
    $('main').dataset.live = 'false';
    return;
  }

  const r = d.reading;
  $('main').dataset.live = r ? 'true' : 'false';
  // Age of the newest reading, not of the last poll: this climbs if the ESP32 stops posting.
  lastDataAt = r ? r.ts : null;

  if (r) {
    setConn(true, 'live');
    for (const pot of POTS) {
      const v = num(r['moisture' + pot]);
      const [state, label] = moistureState(v);
      const card = $('pot-card-' + pot);
      card.dataset.state = state;
      card.dataset.pump = r.pump_on ? 'on' : 'off';
      setChip($('pot' + pot + '-state'), label, state);
      $('moisture' + pot).textContent = v == null ? '--' : Math.round(v);
      $('moisture' + pot + '-fill').style.height = (v == null ? 0 : Math.max(0, Math.min(100, v))) + '%';
    }

    const t = num(r.temperature);
    const h = num(r.humidity);
    const [tState, tLabel, tNote] = tempState(t);
    $('temp-card').dataset.state = tState;
    setChip($('temp-state'), tLabel, tState);
    $('temperature').textContent = t == null ? '--' : t.toFixed(1);
    $('temp-mark').style.left = (t == null ? 0 : Math.max(0, Math.min(100, (t / 50) * 100))) + '%';
    $('temp-note').innerHTML = tNote;

    const [hState, hLabel, hNote] = humState(h);
    $('hum-card').dataset.state = hState;
    setChip($('hum-state'), hLabel, hState);
    $('humidity').textContent = h == null ? '--' : h.toFixed(1);
    $('hum-mark').style.left = (h == null ? 0 : Math.max(0, Math.min(100, h))) + '%';
    $('hum-note').innerHTML = hNote;

    const pumping = !!r.pump_on;
    const manualRunning = d.manual_command && d.manual_command.status === 'started';
    $('pump-pill').dataset.state = pumping ? 'run' : 'idle';
    $('pump-pill-text').textContent = pumping ? (manualRunning ? 'manual watering' : 'pump running') : 'pump idle';
    $('pump-card').dataset.state = pumping ? 'run' : 'idle';
    setChip($('pump-chip'), pumping ? 'watering now' : 'idle', pumping ? 'info' : 'nodata');
  } else {
    setConn(false, 'waiting for ESP32 data…');
    for (const pot of POTS) {
      const card = $('pot-card-' + pot);
      card.dataset.state = 'nodata';
      card.dataset.pump = 'off';
      setChip($('pot' + pot + '-state'), 'no data', 'nodata');
    }
    $('temp-card').dataset.state = 'nodata';
    setChip($('temp-state'), 'no data', 'nodata');
    $('hum-card').dataset.state = 'nodata';
    setChip($('hum-state'), 'no data', 'nodata');
    $('pump-pill').dataset.state = 'idle';
    $('pump-pill-text').textContent = 'pump idle';
    $('pump-card').dataset.state = 'idle';
    setChip($('pump-chip'), 'idle', 'nodata');
  }

  renderManualCommand(d.manual_command, r ? !!r.pump_on : false);
  renderWateringEvent(d.last_watering);
  renderGrowth(d.days_since_planting);
  $('planting-date').value = d.planting_date || '';
}

// -------------------------------------------------------- manual override
// The backend keeps the latest command for 5 minutes, so this renders the whole
// queue -> picked -> running -> done/blocked lifecycle from the 5 s /api/latest poll.
function renderManualCommand(cmd, pumpOn) {
  const btn = $('manual-water');
  const chip = $('manual-chip');
  const note = $('manual-note');

  if (!cmd) {
    setChip(chip, 'ready', 'good');
    btn.disabled = !!pumpOn;
    note.textContent = pumpOn
      ? 'Pump is running — manual override unlocks when it stops.'
      : 'Skips the moisture check and cooldown. Extreme heat or humidity still blocks the pump.';
    return;
  }

  const dur = Math.round(cmd.duration_ms / 1000) + ' s';
  if (cmd.status === 'pending') {
    setChip(chip, 'queued', 'info');
    note.textContent = 'Command queued — waiting for the ESP32 to pick it up…';
    btn.disabled = true;
  } else if (cmd.status === 'picked') {
    setChip(chip, 'received', 'info');
    note.textContent = 'ESP32 received the command — pump starting soon.';
    btn.disabled = true;
  } else if (cmd.status === 'started') {
    setChip(chip, 'running', 'info');
    note.textContent = 'Manual cycle running for ' + dur + '…';
    btn.disabled = true;
  } else if (cmd.status === 'done') {
    setChip(chip, 'completed', 'good');
    note.textContent = 'Manual ' + dur + ' cycle finished — see the watering log.';
    btn.disabled = !!pumpOn;
  } else if (cmd.status === 'blocked') {
    setChip(chip, 'blocked', 'alert');
    note.textContent = cmd.result || 'Blocked by the ESP32 air safety lock.';
    btn.disabled = false;
  } else if (cmd.status === 'expired') {
    setChip(chip, 'no response', 'alert');
    note.textContent = 'ESP32 did not respond — check its power and Wi-Fi.';
    btn.disabled = false;
  } else {
    setChip(chip, 'failed', 'warn');
    note.textContent = cmd.result || 'The ESP32 did not finish the cycle.';
    btn.disabled = false;
  }
}

function renderWateringEvent(ev) {
  if (!ev) {
    $('last-watering').textContent = 'never';
    $('last-watering-detail').textContent = 'no pump cycle recorded yet';
    $('lw-before').textContent = '--';
    $('lw-after').textContent = '--';
    $('lw-gain').textContent = '';
    return;
  }
  const before = Math.round(ev.moisture_before);
  const after = Math.round(ev.moisture_after);
  const gain = after - before;
  $('last-watering').textContent = relTime(ev.ts);
  $('last-watering-detail').textContent =
    (ev.manual ? 'Manual · ' : '') + 'Pot ' + (ev.pot_triggered || '?') + ' · ' +
    (ev.duration_ms / 1000) + ' s pump · ' + fmtTime(ev.ts);
  $('lw-before').textContent = before + '%';
  $('lw-after').textContent = after + '%';
  $('lw-gain').textContent = (gain >= 0 ? '+' : '') + gain + '%';
  $('lw-gain').className = 'gain' + (gain < 0 ? ' neg' : '');
}

// ------------------------------------------------------------------ growth
function renderGrowth(days) {
  const stageEls = Array.from($('stages').children);

  if (days == null) {
    $('days').textContent = '--';
    $('stage').textContent = 'set the planting date';
    $('harvest-count').textContent = 'the countdown starts once the date is saved';
    $('rail-fill').style.width = '0%';
    $('rail-marker').style.left = '0%';
    stageEls.forEach((li) => li.classList.remove('active', 'done'));
    return;
  }

  const pct = Math.max(0, Math.min(100, (days / CYCLE_DAYS) * 100));
  $('rail-fill').style.width = pct + '%';
  $('rail-marker').style.left = pct + '%';
  $('days').textContent = days;

  const active = STAGES.find((s) => days >= s.from && days <= s.to);
  stageEls.forEach((li, i) => {
    const st = STAGES[i];
    li.classList.toggle('active', !!active && st.key === active.key);
    li.classList.toggle('done', days > st.to);
  });
  $('stage').textContent = active
    ? active.label
    : (days > CYCLE_DAYS ? 'Past harvest window (bolting risk)' : STAGES[0].label);

  if (days < 30) {
    $('harvest-count').innerHTML = '≈ <b>' + (30 - days) + '</b> days to the harvest window';
  } else if (days <= CYCLE_DAYS) {
    $('harvest-count').innerHTML = '<b>In the harvest window</b> — day ' + days + ' of ' + CYCLE_DAYS;
  } else {
    $('harvest-count').innerHTML = '<b>Past day ' + CYCLE_DAYS + '</b> — harvest now, bolting risk';
  }
}

// ----------------------------------------------------------------- history
async function refreshHistory() {
  const hours = parseInt($('history-range').value, 10) || 6;
  let rows = [];
  try {
    rows = (await (await fetch('/api/history?hours=' + hours)).json()).rows || [];
  } catch (e) {
    return;
  }

  const labels = rows.map((r) => new Date(r.ts).toLocaleTimeString());
  const datasets = SERIES.map((s) => ({
    label: s.label,
    data: rows.map((r) => r[s.key]),
    borderColor: s.color,
    backgroundColor: s.color,
    pointRadius: 0,
    pointHoverRadius: 4,
    borderWidth: 2,
    tension: 0.35,
  }));

  if (chart) {
    chart.data.labels = labels;
    chart.data.datasets = datasets;
    chart.update('none');
  } else {
    chart = new Chart($('history-chart'), {
      type: 'line',
      data: { labels, datasets },
      options: {
        animation: false,
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        scales: {
          // One shared 0-100 axis works because moisture/humidity are % and temp sits in 15-40 C.
          y: {
            min: 0, max: 100,
            grid: { color: 'rgba(150,205,165,.08)' },
            border: { display: false },
            ticks: { color: '#67826f', font: { size: 10 }, padding: 6 },
            title: { display: true, text: '%   /   °C', color: '#67826f', font: { size: 10 } },
          },
          x: {
            grid: { display: false },
            border: { color: 'rgba(150,205,165,.14)' },
            ticks: { color: '#67826f', font: { size: 10 }, maxTicksLimit: 10, maxRotation: 0 },
          },
        },
        plugins: {
          legend: {
            position: 'bottom',
            labels: { color: '#8ba793', usePointStyle: true, pointStyle: 'circle', boxWidth: 7, boxHeight: 7, padding: 16, font: { size: 11 } },
          },
          tooltip: {
            backgroundColor: '#16241a', borderColor: 'rgba(150,205,165,.22)', borderWidth: 1,
            titleColor: '#e9f4eb', bodyColor: '#8ba793', padding: 10, cornerRadius: 8, displayColors: true,
            callbacks: {
              label: (ctx) => {
                const s = SERIES[ctx.datasetIndex];
                const v = ctx.parsed.y;
                return ' ' + s.label + ': ' + (v == null ? '--' : Math.round(v * 10) / 10 + s.unit);
              },
            },
          },
        },
      },
    });
  }

  renderTrends(rows, hours);
}

// Cheap "which way is each pot moving" readout, over whatever window is loaded.
function renderTrends(rows, hours) {
  for (const pot of POTS) {
    const el = $('pot' + pot + '-trend');
    if (rows.length < 2) { el.textContent = ''; continue; }
    const first = num(rows[0]['moisture' + pot]);
    const last = num(rows[rows.length - 1]['moisture' + pot]);
    if (first == null || last == null) { el.textContent = ''; continue; }
    const delta = Math.round(last - first);
    el.textContent = (delta > 0 ? '▲ +' : delta < 0 ? '▼ ' : '· ') + delta + '% / ' + hours + 'h';
    el.className = 'trend ' + (delta > 0 ? 'up' : delta < 0 ? 'down' : '');
  }
}

// ------------------------------------------------------------- watering log
async function refreshWateringLog() {
  let rows = [];
  try {
    rows = await (await fetch('/api/watering-log')).json();
  } catch (e) {
    return;
  }
  const tbody = $('watering-log');
  $('log-count').textContent = rows.length + (rows.length === 1 ? ' event' : ' events');
  if (!rows.length) {
    tbody.innerHTML = '<tr class="empty"><td colspan="6">No watering events yet.</td></tr>';
    return;
  }
  tbody.innerHTML = rows.map((r) => {
    const before = Math.round(r.moisture_before);
    const after = Math.round(r.moisture_after);
    const gain = after - before;
    const pot = r.pot_triggered || 0;
    return '<tr>' +
      '<td class="when">' + new Date(r.ts).toLocaleTimeString() + '<small>' +
        new Date(r.ts).toLocaleDateString() + ' · ' + relTime(r.ts) + '</small></td>' +
      '<td>' + (pot ? '<span class="pot-chip p' + pot + '">' + pot + '</span>' : '—') +
        (r.manual ? '<span class="tag-manual">manual</span>' : '') + '</td>' +
      '<td><span class="dur-chip">' + (r.duration_ms / 1000) + ' s</span></td>' +
      '<td>' + before + '%</td>' +
      '<td>' + after + '%</td>' +
      '<td class="gain-cell' + (gain < 0 ? ' neg' : '') + '">' + (gain >= 0 ? '+' : '') + gain + '%</td>' +
      '</tr>';
  }).join('');
}

// --------------------------------------------------------------- prediction
async function refreshPrediction() {
  let d;
  try {
    d = await (await fetch('/api/predict')).json();
  } catch (e) {
    return;
  }

  const ring = $('yield-ring');
  const badge = $('yield-category');

  if (!d.ok) {
    ring.style.setProperty('--pct', 0);
    ring.style.setProperty('--ring', '#6b8673');
    $('condition-index').textContent = '--';
    $('yield-g').textContent = '--';
    $('yield-stage').textContent = 'no estimate yet';
    $('yield-maturity').textContent = '';
    setChip(badge, '—', 'nodata');
    $('prediction-note').textContent = d.error || '';
    for (const id of ['avg-moisture', 'avg-temp', 'avg-humidity']) $(id).textContent = '--';
    for (const id of ['score-moisture', 'score-temp', 'score-humidity']) $(id).style.width = '0%';
    for (const id of ['score-moisture-num', 'score-temp-num', 'score-humidity-num']) $(id).textContent = '--';
    return;
  }

  const pct = Math.round(d.condition_index * 100);
  const ringColor = d.yield_category === 'High' ? '#3fc46a' : d.yield_category === 'Medium' ? '#ef9f3c' : '#e2564f';
  ring.style.setProperty('--pct', pct);
  ring.style.setProperty('--ring', ringColor);
  $('condition-index').textContent = pct;
  setChip(badge, d.yield_category, d.yield_category === 'High' ? 'good' : d.yield_category === 'Medium' ? 'warn' : 'alert');

  const y = d.estimated_yield_g_per_plant;
  $('yield-g').textContent = y == null ? '--' : y;
  $('yield-stage').textContent = d.growth_stage;
  $('yield-maturity').textContent = d.maturity_factor == null
    ? 'set the planting date for a full estimate'
    : 'growth ' + Math.round(d.maturity_factor * 100) + '% complete · ' + d.averages.readings_count + ' readings';
  $('prediction-note').textContent = d.note;

  // Scores are shown as % of optimal — the raw 0-1 value confused people, and the
  // bars read the same way as the moisture vessels above.
  const factor = (name, score, avgId, avg) => {
    $('score-' + name).style.width = Math.round(score * 100) + '%';
    $('score-' + name + '-num').textContent = Math.round(score * 100) + '%';
    $(avgId).textContent = avg;
  };
  factor('moisture', d.scores.moisture, 'avg-moisture', d.averages.moisture_pct);
  factor('temp', d.scores.temperature, 'avg-temp', d.averages.temperature_c);
  factor('humidity', d.scores.humidity, 'avg-humidity', d.averages.humidity_pct);
}

// ------------------------------------------------------------------- wiring
$('manual-water').addEventListener('click', async () => {
  const btn = $('manual-water');
  const duration_ms = parseInt($('manual-duration').value, 10) || 4000;
  const seconds = Math.round(duration_ms / 1000);
  if (!confirm('Start the pump now for ' + seconds + ' s?')) return;

  btn.disabled = true;
  setChip($('manual-chip'), 'sending', 'info');
  $('manual-note').textContent = 'Sending the command to the backend…';
  try {
    const res = await fetch('/api/pump/manual', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ duration_ms }),
    });
    const d = await res.json();
    if (!res.ok) {
      setChip($('manual-chip'), 'rejected', 'warn');
      $('manual-note').textContent = d.error || 'The command was rejected.';
      btn.disabled = false;
      return;
    }
    renderManualCommand(d.command, false);
  } catch (e) {
    setChip($('manual-chip'), 'offline', 'alert');
    $('manual-note').textContent = 'Backend unreachable — command not sent.';
    btn.disabled = false;
  }
});

$('planting-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const planting_date = $('planting-date').value;
  await fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ planting_date }),
  });
  refreshLatest();
  refreshPrediction();
});

// The export always mirrors the chart range so the file matches what is on screen.
function syncExportLink() {
  const hours = parseInt($('history-range').value, 10) || 6;
  $('export-csv').href = '/api/history.csv?hours=' + hours;
}

$('history-range').addEventListener('change', () => {
  syncExportLink();
  refreshHistory();
});
syncExportLink();

function tickClock() {
  $('clock').textContent = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  $('ago').textContent = lastDataAt ? '· data ' + relTime(lastDataAt) : '';
}

refreshLatest();
refreshHistory();
refreshWateringLog();
refreshPrediction();
tickClock();
setInterval(refreshLatest, POLL_LATEST_MS);
setInterval(refreshWateringLog, POLL_LOG_MS);
setInterval(() => { refreshHistory(); refreshPrediction(); }, POLL_SLOW_MS);
setInterval(tickClock, 1000);
