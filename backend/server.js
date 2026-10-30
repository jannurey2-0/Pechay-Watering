// Pechay watering backend: Express + node:sqlite (built into Node 22.12+, zero native deps).
// Run: npm install, then npm start. Dashboard: http://localhost:3000

const express = require('express');
const path = require('path');
const { DatabaseSync } = require('node:sqlite');
const { predictYield } = require('./predict');

const PORT = process.env.PORT || 3000;
const DAY_MS = 86400000;

const app = express();
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ---------------------------------------------------------------- database
const db = new DatabaseSync(path.join(__dirname, 'pechay.db'));
db.exec(`
CREATE TABLE IF NOT EXISTS sensor_readings (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,
  moisture1 REAL NOT NULL,
  moisture2 REAL NOT NULL,
  moisture3 REAL NOT NULL,
  temperature REAL NOT NULL,
  humidity REAL NOT NULL,
  pump_on INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS watering_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,
  duration_ms INTEGER NOT NULL,
  moisture_before REAL NOT NULL,
  moisture_after REAL NOT NULL,
  pot_triggered INTEGER NOT NULL DEFAULT 0,
  manual INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS pump_commands (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,
  duration_ms INTEGER NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  picked_ts INTEGER,
  finished_ts INTEGER,
  result TEXT
);
CREATE TABLE IF NOT EXISTS config (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);`);

// Upgrade an existing single-probe database to the 3-pot schema. Runs once, on first start
// after the upgrade; a fresh database already has the new columns and skips this.
function columnExists(table, column) {
  return db.prepare(`PRAGMA table_info(${table})`).all().some((r) => r.name === column);
}

if (!columnExists('sensor_readings', 'moisture1')) {
  db.exec(`
    ALTER TABLE sensor_readings ADD COLUMN moisture1 REAL NOT NULL DEFAULT 0;
    ALTER TABLE sensor_readings ADD COLUMN moisture2 REAL NOT NULL DEFAULT 0;
    ALTER TABLE sensor_readings ADD COLUMN moisture3 REAL NOT NULL DEFAULT 0;`);
  db.exec('UPDATE sensor_readings SET moisture1 = moisture, moisture2 = moisture, moisture3 = moisture');
  // The old single-probe column is NOT NULL, so it has to go or new inserts would fail.
  db.exec('ALTER TABLE sensor_readings DROP COLUMN moisture');
  console.log('Database upgraded: old single-probe readings copied into all 3 pot columns.');
}

if (!columnExists('watering_events', 'pot_triggered')) {
  db.exec('ALTER TABLE watering_events ADD COLUMN pot_triggered INTEGER NOT NULL DEFAULT 0');
  console.log('Database upgraded: added pot_triggered to watering events.');
}

if (!columnExists('watering_events', 'manual')) {
  db.exec('ALTER TABLE watering_events ADD COLUMN manual INTEGER NOT NULL DEFAULT 0');
  console.log('Database upgraded: added manual flag to watering events.');
}

const COMMAND_TTL_MS = 60000;             // ESP32 polls every 5 s; a minute offline means it missed it
const COMMAND_STARTED_TTL_MS = 2 * 60000; // longest cycle is 30 s, plus the 15 s settle

const stmts = {
  insertReading: db.prepare('INSERT INTO sensor_readings (ts, moisture1, moisture2, moisture3, temperature, humidity, pump_on) VALUES (?, ?, ?, ?, ?, ?, ?)'),
  insertWatering: db.prepare('INSERT INTO watering_events (ts, duration_ms, moisture_before, moisture_after, pot_triggered, manual) VALUES (?, ?, ?, ?, ?, ?)'),
  getConfig: db.prepare('SELECT value FROM config WHERE key = ?'),
  setConfig: db.prepare('INSERT INTO config (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value'),
  latestReading: db.prepare('SELECT * FROM sensor_readings ORDER BY ts DESC LIMIT 1'),
  latestWatering: db.prepare('SELECT * FROM watering_events ORDER BY ts DESC LIMIT 1'),
  history: db.prepare('SELECT ts, moisture1, moisture2, moisture3, temperature, humidity, pump_on FROM sensor_readings WHERE ts >= ? ORDER BY ts ASC LIMIT 5000'),
  // No LIMIT: the chart caps at 5000 points, but an export for the panel must be complete.
  historyAll: db.prepare('SELECT id, ts, moisture1, moisture2, moisture3, temperature, humidity, pump_on FROM sensor_readings WHERE ts >= ? ORDER BY ts ASC'),
  wateringLog: db.prepare('SELECT * FROM watering_events ORDER BY ts DESC LIMIT 50'),
  insertCommand: db.prepare("INSERT INTO pump_commands (ts, duration_ms, status) VALUES (?, ?, 'pending')"),
  commandById: db.prepare('SELECT * FROM pump_commands WHERE id = ?'),
  nextPendingCommand: db.prepare("SELECT * FROM pump_commands WHERE status = 'pending' ORDER BY id ASC LIMIT 1"),
  markCommandPicked: db.prepare("UPDATE pump_commands SET status = 'picked', picked_ts = ? WHERE id = ? AND status = 'pending'"),
  startCommand: db.prepare("UPDATE pump_commands SET status = 'started' WHERE id = ? AND status IN ('pending', 'picked')"),
  finishCommand: db.prepare("UPDATE pump_commands SET status = ?, finished_ts = ?, result = ? WHERE id = ? AND status NOT IN ('done', 'blocked', 'failed', 'expired')"),
  activeCommand: db.prepare("SELECT * FROM pump_commands WHERE status IN ('pending', 'picked', 'started') ORDER BY id DESC LIMIT 1"),
  recentCommand: db.prepare('SELECT * FROM pump_commands WHERE ts >= ? ORDER BY id DESC LIMIT 1'),
  expirePending: db.prepare("UPDATE pump_commands SET status = 'expired', finished_ts = ?, result = 'ESP32 did not pick up the command in time.' WHERE status = 'pending' AND ts < ?"),
  expirePicked: db.prepare("UPDATE pump_commands SET status = 'expired', finished_ts = ?, result = 'ESP32 did not confirm the command in time.' WHERE status = 'picked' AND picked_ts < ?"),
  expireStarted: db.prepare("UPDATE pump_commands SET status = 'failed', finished_ts = ?, result = 'No completion report arrived from the ESP32.' WHERE status = 'started' AND picked_ts < ?"),
};

function expireCommands() {
  const now = Date.now();
  stmts.expirePending.run(now, now - COMMAND_TTL_MS);
  stmts.expirePicked.run(now, now - COMMAND_TTL_MS);
  stmts.expireStarted.run(now, now - COMMAND_STARTED_TTL_MS);
}

function getConfig(key) {
  const r = stmts.getConfig.get(key);
  return r ? r.value : null;
}

// Day 1 = the planting day itself.
function plantingInfo() {
  const pd = getConfig('planting_date');
  if (!pd) return { planting_date: null, days_since_planting: null, planting_ts: null };
  const start = new Date(pd + 'T00:00:00');
  if (isNaN(start.getTime())) return { planting_date: null, days_since_planting: null, planting_ts: null };
  return {
    planting_date: pd,
    days_since_planting: Math.max(1, Math.floor((Date.now() - start.getTime()) / DAY_MS) + 1),
    planting_ts: start.getTime(),
  };
}

function isNum(v) {
  return typeof v === 'number' && Number.isFinite(v);
}

function clampPct(v) {
  return Math.min(100, Math.max(0, v));
}

// "YYYY-MM-DD HH:mm:ss" in the server's local time - Excel-friendly and free of commas.
function localTime(ts) {
  const d = new Date(ts);
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

// ---------------------------------------------------------------- API
// ESP32 posts readings here: one moisture value per pot, plus shared air temp/humidity.
app.post('/api/sensor-data', (req, res) => {
  const { moisture1, moisture2, moisture3, temperature, humidity, pumpOn } = req.body || {};
  if (![moisture1, moisture2, moisture3, temperature, humidity].every(isNum)) {
    return res.status(400).json({ error: 'moisture1, moisture2, moisture3, temperature, humidity must be numbers' });
  }
  stmts.insertReading.run(
    Date.now(),
    clampPct(moisture1),
    clampPct(moisture2),
    clampPct(moisture3),
    temperature,
    humidity,
    pumpOn ? 1 : 0
  );
  res.json({ ok: true, ...plantingInfo() });
});

// Dashboard: newest reading + last watering + planting info.
app.get('/api/latest', (req, res) => {
  expireCommands();
  res.json({
    reading: stmts.latestReading.get() || null,
    last_watering: stmts.latestWatering.get() || null,
    manual_command: stmts.recentCommand.get(Date.now() - 5 * 60000) || null,
    ...plantingInfo(),
    server_time: Date.now(),
  });
});

// Dashboard: history rows for the chart, last N hours.
app.get('/api/history', (req, res) => {
  const hours = Math.min(168, Math.max(1, parseInt(req.query.hours, 10) || 6));
  res.json({ hours, rows: stmts.history.all(Date.now() - hours * 3600000) });
});

// Dashboard: CSV export of every reading in the selected range.
app.get('/api/history.csv', (req, res) => {
  const hours = Math.min(168, Math.max(1, parseInt(req.query.hours, 10) || 6));
  const rows = stmts.historyAll.all(Date.now() - hours * 3600000);

  const lines = [
    'reading_id,timestamp_local,timestamp_iso_utc,epoch_ms,pot1_moisture_pct,pot2_moisture_pct,pot3_moisture_pct,average_moisture_pct,temperature_c,humidity_pct,pump_state,pump_on',
  ];
  for (const r of rows) {
    const avg = (r.moisture1 + r.moisture2 + r.moisture3) / 3;
    lines.push([
      r.id,
      localTime(r.ts),
      new Date(r.ts).toISOString(),
      r.ts,
      r.moisture1.toFixed(1),
      r.moisture2.toFixed(1),
      r.moisture3.toFixed(1),
      avg.toFixed(1),
      r.temperature.toFixed(1),
      r.humidity.toFixed(1),
      r.pump_on ? 'ON' : 'OFF',
      r.pump_on,
    ].join(','));
  }

  const now = new Date();
  const p = (n) => String(n).padStart(2, '0');
  const filename = `pechay-sensor-history-${hours}h-${now.getFullYear()}${p(now.getMonth() + 1)}${p(now.getDate())}-${p(now.getHours())}${p(now.getMinutes())}.csv`;
  res.setHeader('Content-Type', 'text/csv; charset=utf-8');
  res.setHeader('Content-Disposition', `attachment; filename="${filename}"`);
  res.send(lines.join('\r\n') + '\r\n');
});

// ESP32 posts a watering event when a pump cycle finishes.
app.post('/api/watering', (req, res) => {
  const { duration_ms, moisture_before, moisture_after, pot_triggered, manual, command_id } = req.body || {};
  if (![duration_ms, moisture_before, moisture_after, pot_triggered].every(isNum)) {
    return res.status(400).json({ error: 'duration_ms, moisture_before, moisture_after, pot_triggered must be numbers' });
  }
  stmts.insertWatering.run(Date.now(), duration_ms, moisture_before, moisture_after, pot_triggered, manual ? 1 : 0);
  if (isNum(command_id)) {
    stmts.finishCommand.run('done', Date.now(), 'Manual cycle completed.', command_id);
  }
  res.json({ ok: true });
});

// Dashboard: queue a manual pump cycle. Bypasses soil moisture and cooldown on the ESP32,
// which still refuses in extreme air. One command at a time keeps double-taps harmless.
app.post('/api/pump/manual', (req, res) => {
  expireCommands();
  const raw = req.body ? Number(req.body.duration_ms) : NaN;
  const duration = Math.min(30000, Math.max(1000, Math.round(raw) || 4000));

  const active = stmts.activeCommand.get();
  if (active) {
    return res.status(409).json({ error: 'A manual watering command is already in progress.', command: active });
  }

  const info = stmts.insertCommand.run(Date.now(), duration);
  res.status(201).json({ ok: true, command: stmts.commandById.get(info.lastInsertRowid) });
});

// ESP32 polls this every 5 s. Plain text keeps the firmware free of a JSON library:
// "id duration_ms", or 204 when nothing is waiting.
app.get('/api/pump/command', (req, res) => {
  expireCommands();
  const cmd = stmts.nextPendingCommand.get();
  if (!cmd) return res.status(204).end();
  if (!stmts.markCommandPicked.run(Date.now(), cmd.id).changes) return res.status(204).end();
  res.type('text/plain').send(`${cmd.id} ${cmd.duration_ms}`);
});

// ESP32 reports whether the manual cycle started or was blocked by the air safety lock.
app.post('/api/pump/command', (req, res) => {
  const { id, status, reason } = req.body || {};
  if (!isNum(id) || !['started', 'blocked', 'failed'].includes(status)) {
    return res.status(400).json({ error: 'id must be a number and status one of started, blocked, failed' });
  }
  const changed = status === 'started'
    ? stmts.startCommand.run(id)
    : stmts.finishCommand.run(status, Date.now(), typeof reason === 'string' ? reason.slice(0, 200) : null, id);
  if (!changed.changes) return res.status(404).json({ error: 'unknown command id' });
  res.json({ ok: true, command: stmts.commandById.get(id) });
});

// Dashboard: recent watering events, newest first.
app.get('/api/watering-log', (req, res) => {
  res.json(stmts.wateringLog.all());
});

// Dashboard: set the planting date (YYYY-MM-DD).
app.post('/api/config', (req, res) => {
  const { planting_date } = req.body || {};
  if (typeof planting_date !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(planting_date) || isNaN(new Date(planting_date))) {
    return res.status(400).json({ error: 'planting_date must be a valid YYYY-MM-DD date' });
  }
  stmts.setConfig.run('planting_date', planting_date);
  res.json({ ok: true, ...plantingInfo() });
});

// Dashboard: rule-based yield prediction.
app.get('/api/predict', (req, res) => {
  const info = plantingInfo();
  res.json(predictYield(db, info.days_since_planting, info.planting_ts));
});

app.listen(PORT, () => {
  console.log(`Pechay dashboard:  http://localhost:${PORT}`);
  console.log(`On your phone (same Wi-Fi):  http://<this PC's LAN IP>:${PORT}`);
});
