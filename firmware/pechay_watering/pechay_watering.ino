// IoT Pechay Watering and Yield Prediction - ESP32 firmware
// Solar / battery powered, 3 pechay pots (1 soil probe each), 12V pump via relay, 16x2 I2C LCD.
// Board: ESP32 Dev Module (Arduino IDE)
// Wiring: see SOLAR_WIRING.md
//
// Libraries to install (Tools -> Manage Libraries):
//   - "DHT sensor library" by Adafruit  (it will offer "Adafruit Unified Sensor" too - install both)
//   - "LiquidCrystal I2C" by Frank de Brabander  (may show as marcoschwartz)
//
// Flow: read sensors -> show on LCD -> evaluate alerts -> auto-water with cooldown
//       -> poll dashboard for manual overrides -> POST to backend -> Telegram alerts
//
// The auto-watering decision happens ON THE ESP32, so the plants get water even if the laptop,
// the Wi-Fi, or the backend is down. The server only stores and displays.
// Manual override is the one exception: the ESP32 asks the server for queued dashboard commands
// every few seconds, then runs the pump itself even if the soil is above the threshold.

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Preferences.h>   // built in: remembers your settings on the chip
#include <WebServer.h>     // built in: serves the setup page
#include <DNSServer.h>     // built in: makes the setup page pop up by itself

// ---------------- WIFI / SERVER CONFIG - set from your phone, not here ----------------
// No more editing this file to change networks. On first boot the ESP32 opens a hotspot
// called "Pechay-Setup"; join it from your phone, pick your WiFi, type the password and
// your laptop's IP (from `ipconfig`). The ESP32 remembers them forever (stored on the chip).
// To change them later, hold the BOOT button for ~4 seconds - the hotspot opens again.
// The port stays fixed; the portal asks only for the IP.
#define SERVER_PORT 3000

// Telegram bot credentials (see IMPLEMENTATION_PLAN.md section 6 for setup steps).
// Leave these as-is until you have created the bot - alerts are simply skipped.
#define BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID "YOUR_TELEGRAM_CHAT_ID"

// ---------------- WIFI SETUP PORTAL ----------------
#define AP_SSID "Pechay-Setup"     // the hotspot your phone joins
#define SETUP_BUTTON_PIN 0         // the BOOT button on the DevKit
#define SETUP_HOLD_MS 4000         // hold BOOT this long to reopen the hotspot

// ---------------- PINS (see SOLAR_WIRING.md pin summary) ----------------
#define NUM_POTS 3
const int PIN_MOIST[NUM_POTS] = { 34, 35, 39 };  // YL-69 AO for pot 1/2/3. MUST be ADC1 pins
                                                 // (GPIO 32-39) - ADC2 fails when Wi-Fi is on.
#define PIN_DHT 33        // DHT11 DATA pin (module already has the 10k pull-up)
#define PIN_RELAY 26      // Relay IN pin
#define PIN_LCD_SDA 21    // LCD SDA
#define PIN_LCD_SCL 22    // LCD SCL
#define LCD_ADDR 0x27     // LCD stays blank after adjusting contrast? Try 0x3F here.

// ---------------- YL-69 CALIBRATION (one pair per pot, from calibrate_moisture sketch) -------
// AIR   = raw value with the probe held dry in open air.
// WATER = the WETTEST raw value measured in that pot's soil (low raw = wet). Use the
//         lowest value seen right after watering, not a cup-of-water value.
// Probes differ a little from each other, so calibrate all three separately.
const int MOISTURE_RAW_AIR[NUM_POTS]   = { 4095, 4095, 4095 };  // dry air
const int MOISTURE_RAW_WATER[NUM_POTS] = { 1017, 1176, 1050 };  // wettest soil measured
// If a pot's percent reads backwards (100% in air), swap that pot's two values.

// ---------------- THRESHOLDS ----------------
const int MOISTURE_WATERING_BELOW = 30;   // % - water when the DRIEST pot drops below this
const int MOISTURE_ALERT_BELOW = 30;      // % - notify when any pot is below this
const int MOISTURE_ALERT_CLEAR = 35;      // % - alert clears once ALL pots are above this
const float TEMP_ALERT_ABOVE = 35.0;      // deg C - notify when above
const float TEMP_ALERT_CLEAR = 33.0;      // deg C
const float HUMIDITY_ALERT_BELOW = 40.0;  // % - notify when below
const float HUMIDITY_ALERT_CLEAR = 45.0;  // %

// Block watering only in truly extreme air conditions (tune freely).
const float TEMP_WATER_BLOCK = 40.0;
const float HUMIDITY_WATER_BLOCK_LOW = 30.0;
const float HUMIDITY_WATER_BLOCK_HIGH = 95.0;

// ---------------- WATERING TIMING ----------------
// One pump feeds all 3 pots through your pipes, so a cycle has to be long enough for the
// driest pot. Start at 4 s and raise it (6000-8000) if the pots are not wet enough after a cycle.
const unsigned long PUMP_DURATION_MS = 4000;                    // pump runs 4 seconds per cycle
const unsigned long WATER_COOLDOWN_MS = 10UL * 60UL * 1000UL;   // min 10 minutes between cycles
const unsigned long POST_WATERING_SETTLE_MS = 15000;            // let water soak in before measuring

// ---------------- SAMPLE / UPLOAD INTERVALS ----------------
const unsigned long SENSOR_INTERVAL_MS = 5000;    // read sensors + refresh LCD every 5 s
const unsigned long SEND_INTERVAL_MS = 30000;     // POST to backend every 30 s
const unsigned long COMMAND_POLL_MS = 5000;       // ask the backend for manual pump commands

// ---------------- RELAY POLARITY ----------------
// Most 1-channel relay boards are ACTIVE-LOW: writing LOW turns the pump ON.
// If your pump runs when it should be off, swap these two.
#define RELAY_PUMP_ON LOW
#define RELAY_PUMP_OFF HIGH

#define DHTTYPE DHT11
DHT dht(PIN_DHT, DHTTYPE);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// ---------------- STATE ----------------
unsigned long lastSensorReadMs = 0;
unsigned long lastSendMs = 0;
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastCommandPollMs = 0;
unsigned long lastWateringMs = 0;   // start of last cycle; watering enabled after cooldown
unsigned long pumpStartMs = 0;
unsigned long pumpStoppedMs = 0;
unsigned long cycleDurationMs = PUMP_DURATION_MS;  // this cycle's run time (auto or manual)
bool pumpRunning = false;
bool wateringAfterPending = false;
bool manualCycle = false;           // true while the current/last cycle came from the dashboard

long pendingCommandId = 0;          // command picked up but not started yet
unsigned long pendingCommandDuration = 0;
long activeCommandId = 0;           // manual command being executed now

int moisturePct[NUM_POTS] = { 0, 0, 0 };
int moistureBeforeWatering = 0;
int triggeredPot = 0;               // 1-based pot that caused the current/last watering cycle
float temperatureC = 0.0;           // last valid readings; reused if a DHT read fails
float humidityPct = 0.0;
bool dhtEverOk = false;

int dhtFailCount = 0;
bool dhtFailAlerted = false;

// alert latches - one Telegram message per event, not one per sample
bool alertMoistureActive = false;
bool alertTempActive = false;
bool alertHumidityActive = false;

// ---------------- HELPERS ----------------
int readMoistureRaw(int pot) {
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(PIN_MOIST[pot]);
    delay(20);
  }
  return sum / 8;
}

int rawToPercent(int pot, int raw) {
  // YL-69 resistance rises roughly exponentially as the soil dries, so a linear
  // map bunches most of the usable soil range near 100%. Interpolate in log space.
  float dry = MOISTURE_RAW_AIR[pot];
  float wet = MOISTURE_RAW_WATER[pot];
  float r = constrain((float)raw, wet, dry);
  float pct = 100.0f * (log(dry) - log(r)) / (log(dry) - log(wet));
  return constrain((int)(pct + 0.5f), 0, 100);
}

// 0-based index of the pot with the lowest moisture.
int driestPot() {
  int driest = 0;
  for (int p = 1; p < NUM_POTS; p++) {
    if (moisturePct[p] < moisturePct[driest]) driest = p;
  }
  return driest;
}

String allPotsText() {
  return String(moisturePct[0]) + "% / " + String(moisturePct[1]) + "% / " + String(moisturePct[2]) + "%";
}

// Print a line without leaving leftovers from the previous, longer text (no flicker).
void lcdLine(int row, const String& text) {
  String padded = text + "                ";
  lcd.setCursor(0, row);
  lcd.print(padded.substring(0, 16));
}

void updateLcd() {

  // Line 0: air temp, humidity, and pump state, e.g. "T30.5C H73% ON"
  String temp = dhtEverOk ? String(temperatureC, 1) : "--.-";
  String hum = dhtEverOk ? String(humidityPct, 0) : "--";
  lcdLine(0, "T" + temp + "C H" + hum + "% ");

  // Line 1: the three pot moistures, e.g. "M:45/60/55"
  lcdLine(1, "M:" + String(moisturePct[0]) + "/" + String(moisturePct[1]) + "/" + String(moisturePct[2])  + " -- " + (pumpRunning ? "WATER" : "ok"));

  
}

// Minimal URL-encoding for Telegram text (spaces, newlines, &, %, etc.)
String urlEncode(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

void sendTelegram(const String& message) {
  if (String(BOT_TOKEN).startsWith("YOUR_")) {
    Serial.println("Telegram not configured yet - message skipped: " + message);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();  // prototype shortcut: skips TLS cert checks. Fine for a hobby build.
  HTTPClient https;
  https.setTimeout(8000);
  if (!https.begin(client, "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage")) return;
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "chat_id=" + String(CHAT_ID) + "&text=" + urlEncode(message);
  int code = https.POST(body);
  Serial.printf("Telegram send -> HTTP %d\n", code);
  https.end();
}

// ---------------- WIFI SETUP PORTAL (config is stored on the chip, not in this file) ----------------
struct DeviceConfig { String ssid, pass, host; };
DeviceConfig cfg;                 // filled from storage at boot
Preferences prefs;
WebServer portal(80);
DNSServer dns;
int scanCount = 0;                // WiFi networks found, fills the portal dropdown

void loadConfig() {
  prefs.begin("pechay", true);    // read-only
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.host = prefs.getString("host", "");
  prefs.end();
}

void saveConfig() {
  prefs.begin("pechay", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("host", cfg.host);
  prefs.end();
}

String serverUrl() { return "http://" + cfg.host + ":" + String(SERVER_PORT); }

String portalPage() {
  String s = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Pechay Setup</title></head>"
               "<body style='font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 14px'>"
               "<h2>Pechay Watering Setup</h2>"
               "<p>Pick your home WiFi, type its password, then type your laptop's IP address "
               "(on the laptop run <b>ipconfig</b> - the IPv4 Address, e.g. 192.168.1.6).</p>"
               "<form method='post' action='/save'>"
               "WiFi network:<br><select name='ssid' style='width:100%;padding:8px'>");
  for (int i = 0; i < scanCount; i++) {
    s += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + WiFi.RSSI(i) + ")</option>";
  }
  s += F("</select><br><br>"
         "Or type SSID manually:<br><input name='ssid2' style='width:100%;padding:8px'><br><br>"
         "WiFi password:<br><input name='pass' type='password' style='width:100%;padding:8px'><br><br>"
         "Laptop IP (server):<br><input name='host' style='width:100%;padding:8px' placeholder='192.168.1.6'><br><br>"
         "<button type='submit' style='width:100%;padding:10px;font-size:1rem'>Save &amp; restart</button>"
         "</form></body></html>");
  return s;
}

void handlePortalSave() {
  String ssid = portal.arg("ssid2").length() ? portal.arg("ssid2") : portal.arg("ssid");
  String host = portal.arg("host");
  ssid.trim(); host.trim();
  if (ssid.length() == 0 || host.length() == 0) {
    portal.send(200, "text/html", "<p>WiFi network and laptop IP are both required.</p><p><a href='/'>Back</a></p>");
    return;
  }
  cfg.ssid = ssid;
  cfg.pass = portal.arg("pass");
  cfg.host = host;
  saveConfig();
  portal.send(200, "text/html", "<p><b>Saved.</b> The ESP32 is restarting and will join your WiFi. "
                                "This page is safe to close.</p>");
  delay(800);
  ESP.restart();
}

// Opens the setup hotspot and NEVER returns (saves + restarts when the user submits,
// or the device is simply power-cycled to cancel).
void runSetupPortal() {
  digitalWrite(PIN_RELAY, RELAY_PUMP_OFF);   // safety: never pump while configuring
  pumpRunning = false;
  pendingCommandId = 0;
  activeCommandId = 0;
  manualCycle = false;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  IPAddress apIp = WiFi.softAPIP();

  dns.start(53, "*", apIp);                  // any web address -> this page, so phones pop it up

  portal.on("/", HTTP_GET, []() { portal.send(200, "text/html", portalPage()); });
  portal.on("/save", HTTP_POST, handlePortalSave);
  portal.onNotFound([&]() {
    portal.sendHeader("Location", "http://" + apIp.toString() + "/");
    portal.send(302, "text/plain", "");
  });
  portal.begin();

  Serial.printf("\n>>> SETUP MODE <<<\nJoin WiFi '%s' from your phone, then open http://%s\n",
                AP_SSID, apIp.toString().c_str());
  lcdLine(0, "SETUP MODE");
  lcdLine(1, "Join Pechay-Set");

  scanCount = WiFi.scanNetworks();           // fills the dropdown on the page

  while (true) {
    portal.handleClient();
    dns.processNextRequest();
    delay(2);
  }
}

// Hold BOOT ~4 s at any time to reopen the setup hotspot.
void checkSetupButton() {
  static unsigned long heldSince = 0;
  bool pressed = digitalRead(SETUP_BUTTON_PIN) == LOW;
  if (pressed && heldSince == 0) heldSince = millis();
  if (!pressed) heldSince = 0;
  if (heldSince && millis() - heldSince >= SETUP_HOLD_MS) runSetupPortal();
}

// allowPortal = true only for the boot-time attempt: a rejected password means the
// stored credentials are wrong, so open the hotspot. Later retries (router rebooting,
// out of range) must NOT yank the device into setup mode - they just try again.
void connectWiFi(bool allowPortal) {
  if (cfg.ssid.length() == 0) {
    Serial.println("No WiFi saved yet - opening the setup hotspot.");
    runSetupPortal();                       // never returns
  }
  Serial.printf("Connecting to WiFi '%s' ...\n", cfg.ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else if (allowPortal && WiFi.status() == WL_CONNECT_FAILED) {
    Serial.println("Password rejected - opening the setup hotspot. Re-enter it there.");
    runSetupPortal();                       // never returns
  } else {
    Serial.println("WiFi not connected (router down or out of range?) - will retry later. "
                   "Sensors/watering keep running.");
  }
}

void postJSON(const String& url, const String& json, uint16_t timeoutMs = 8000) {
  if (WiFi.status() != WL_CONNECTED || cfg.host.length() == 0) return;
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  Serial.printf("POST %s -> HTTP %d\n", url.c_str(), code);
  http.end();
}

// ---------------- MANUAL OVERRIDE (dashboard command poll) ----------------
void postCommandResult(long id, const char* status, const String& reason = "") {
  String json = "{\"id\":" + String(id) + ",\"status\":\"" + String(status) + "\"";
  if (reason.length()) json += ",\"reason\":\"" + reason + "\"";
  json += "}";
  postJSON(serverUrl() + "/api/pump/command", json, 3000);
}

// The backend replies "cmdId durationMs" (plain text) or 204 when nothing is queued.
void pollManualCommand() {
  if (WiFi.status() != WL_CONNECTED || cfg.host.length() == 0) return;
  if (pendingCommandId != 0) return;              // one queued command is enough

  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(serverUrl() + "/api/pump/command")) return;
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    long id = 0;
    unsigned long duration = 0;
    if (sscanf(body.c_str(), "%ld %lu", &id, &duration) == 2 && id > 0) {
      pendingCommandId = id;
      pendingCommandDuration = constrain(duration, 1000UL, 30000UL);
      Serial.printf("Manual command #%ld queued (%lu ms)\n", pendingCommandId, pendingCommandDuration);
    }
  } else if (code != 204) {
    Serial.printf("Manual command poll -> HTTP %d\n", code);
  }
  http.end();
}

// ---------------- SENSORS ----------------
void readSensors() {
  for (int p = 0; p < NUM_POTS; p++) {
    moisturePct[p] = rawToPercent(p, readMoistureRaw(p));
  }

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    dhtFailCount++;
    if (dhtFailCount >= 10 && !dhtFailAlerted) {
      sendTelegram("Pechay system alert: DHT11 temperature/humidity sensor is not responding. Check wiring.");
      dhtFailAlerted = true;
    }
    Serial.println("DHT11 read failed");
  } else {
    dhtFailCount = 0;
    if (dhtFailAlerted) {
      sendTelegram("Pechay system: DHT11 sensor is reading again.");
      dhtFailAlerted = false;
    }
    humidityPct = h;
    temperatureC = t;
    dhtEverOk = true;
  }

  Serial.printf("M1=%d%% M2=%d%% M3=%d%%  temp=%.1fC  humidity=%.1f%%  pump=%s\n",
                moisturePct[0], moisturePct[1], moisturePct[2],
                temperatureC, humidityPct, pumpRunning ? "ON" : "off");

  updateLcd();
}

// ---------------- ALERTS (with hysteresis so the phone doesn't get spammed) ----------------
void evaluateAlerts() {
  int driest = driestPot();

  if (!alertMoistureActive && moisturePct[driest] < MOISTURE_ALERT_BELOW) {
    alertMoistureActive = true;
    sendTelegram("Pechay alert: pot " + String(driest + 1) + " soil moisture is "
                 + String(moisturePct[driest]) + "% (below " + String(MOISTURE_ALERT_BELOW) + "%).\n"
                 + "All pots: " + allPotsText());
  } else if (alertMoistureActive) {
    // Clear only once every pot has climbed back above the clear threshold.
    bool allRecovered = true;
    for (int p = 0; p < NUM_POTS; p++) {
      if (moisturePct[p] < MOISTURE_ALERT_CLEAR) allRecovered = false;
    }
    if (allRecovered) {
      alertMoistureActive = false;
      sendTelegram("Pechay update: all pots recovered - " + allPotsText());
    }
  }

  if (dhtEverOk) {
    if (!alertTempActive && temperatureC > TEMP_ALERT_ABOVE) {
      alertTempActive = true;
      sendTelegram("Pechay alert: temperature is " + String(temperatureC, 1) + "C (above " + String(TEMP_ALERT_ABOVE, 1) + "C).");
    } else if (alertTempActive && temperatureC < TEMP_ALERT_CLEAR) {
      alertTempActive = false;
      sendTelegram("Pechay update: temperature back to " + String(temperatureC, 1) + "C.");
    }

    if (!alertHumidityActive && humidityPct < HUMIDITY_ALERT_BELOW) {
      alertHumidityActive = true;
      sendTelegram("Pechay alert: humidity is " + String(humidityPct, 1) + "% (below " + String(HUMIDITY_ALERT_BELOW, 1) + "%).");
    } else if (alertHumidityActive && humidityPct >= HUMIDITY_ALERT_CLEAR) {
      alertHumidityActive = false;
      sendTelegram("Pechay update: humidity recovered to " + String(humidityPct, 1) + "%.");
    }
  }
}

// ---------------- AUTO-WATERING (non-blocking pump state machine) ----------------
// One pump waters all 3 pots through your pipes. A cycle is triggered by whichever pot is
// driest, and the cooldown is shared - so the pots are never watered back-to-back.
void updateWatering() {
  unsigned long now = millis();

  if (pumpRunning) {
    if (now - pumpStartMs >= cycleDurationMs) {
      digitalWrite(PIN_RELAY, RELAY_PUMP_OFF);
      pumpRunning = false;
      lastWateringMs = now;
      pumpStoppedMs = now;
      wateringAfterPending = true;
      updateLcd();
    }
    return;
  }

  if (wateringAfterPending) {
    if (now - pumpStoppedMs >= POST_WATERING_SETTLE_MS) {
      int pot = triggeredPot - 1;
      moisturePct[pot] = rawToPercent(pot, readMoistureRaw(pot));
      int moistureAfter = moisturePct[pot];
      Serial.printf("Watering done (pot %d triggered). Moisture %d%% -> %d%%\n",
                    triggeredPot, moistureBeforeWatering, moistureAfter);

      String json = "{\"duration_ms\":" + String(cycleDurationMs)
                  + ",\"moisture_before\":" + String(moistureBeforeWatering)
                  + ",\"moisture_after\":" + String(moistureAfter)
                  + ",\"pot_triggered\":" + String(triggeredPot)
                  + ",\"manual\":" + (manualCycle ? "true" : "false");
      if (manualCycle && activeCommandId != 0) json += ",\"command_id\":" + String(activeCommandId);
      json += "}";
      postJSON(serverUrl() + "/api/watering", json);
      wateringAfterPending = false;
      manualCycle = false;
      activeCommandId = 0;
      updateLcd();
    }
    return;
  }

  // Manual override from the dashboard: skip the moisture threshold and cooldown, but keep
  // the extreme-air lock so a tap cannot pump in 40 C heat or saturated/dust-dry air.
  if (pendingCommandId != 0) {
    bool airOk = !dhtEverOk || (temperatureC < TEMP_WATER_BLOCK
                 && humidityPct > HUMIDITY_WATER_BLOCK_LOW
                 && humidityPct < HUMIDITY_WATER_BLOCK_HIGH);
    if (!airOk) {
      String reason = temperatureC >= TEMP_WATER_BLOCK
        ? "Blocked: air is " + String(temperatureC, 1) + " C (40 C limit)."
        : "Blocked: humidity is outside the safe 30-95% range.";
      Serial.println("Manual command blocked by the air safety lock");
      postCommandResult(pendingCommandId, "blocked", reason);
      pendingCommandId = 0;
      return;
    }

    int driest = driestPot();               // reference pot for the before/after log
    triggeredPot = driest + 1;
    moistureBeforeWatering = moisturePct[driest];
    pumpStartMs = now;
    cycleDurationMs = pendingCommandDuration;
    activeCommandId = pendingCommandId;
    pendingCommandId = 0;
    manualCycle = true;
    pumpRunning = true;
    digitalWrite(PIN_RELAY, RELAY_PUMP_ON);
    Serial.printf("Manual watering START: pot %d reference, %lu ms (command #%ld)\n",
                  triggeredPot, cycleDurationMs, activeCommandId);
    postCommandResult(activeCommandId, "started");
    updateLcd();
    return;
  }

  bool cooldownOk = (lastWateringMs == 0) || (now - lastWateringMs >= WATER_COOLDOWN_MS);
  bool airOk = !dhtEverOk || (temperatureC < TEMP_WATER_BLOCK
               && humidityPct > HUMIDITY_WATER_BLOCK_LOW
               && humidityPct < HUMIDITY_WATER_BLOCK_HIGH);
  // If the DHT11 is dead we still water - dry soil is the bigger risk than hot air.

  int driest = driestPot();
  if (moisturePct[driest] < MOISTURE_WATERING_BELOW && cooldownOk && airOk) {
    triggeredPot = driest + 1;
    moistureBeforeWatering = moisturePct[driest];
    pumpStartMs = now;
    cycleDurationMs = PUMP_DURATION_MS;
    manualCycle = false;
    activeCommandId = 0;
    pumpRunning = true;
    digitalWrite(PIN_RELAY, RELAY_PUMP_ON);
    Serial.printf("Watering START: pot %d at %d%% (below %d%%)\n",
                  triggeredPot, moistureBeforeWatering, MOISTURE_WATERING_BELOW);
    updateLcd();
  }
}

// ---------------- BACKEND UPLOAD ----------------
void sendSensorData() {
  char json[256];
  snprintf(json, sizeof(json),
           "{\"moisture1\":%d,\"moisture2\":%d,\"moisture3\":%d,"
           "\"temperature\":%.1f,\"humidity\":%.1f,\"pumpOn\":%s}",
           moisturePct[0], moisturePct[1], moisturePct[2],
           temperatureC, humidityPct, pumpRunning ? "true" : "false");
  postJSON(serverUrl() + "/api/sensor-data", String(json));
}

// ---------------- SETUP / LOOP ----------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Pechay watering system booting (3 pots + LCD)");

  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_PUMP_OFF);

  Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcdLine(0, "Pechay Watering");
  lcdLine(1, "Booting...");

  dht.begin();

  loadConfig();
  connectWiFi(true);
  lastSensorReadMs = millis() - SENSOR_INTERVAL_MS;  // read on first loop
}

void loop() {
  unsigned long now = millis();

  checkSetupButton();

  if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = now;
    readSensors();
    evaluateAlerts();
  }

  updateWatering();

  // Never poll mid-cycle: a slow HTTP GET would block the call that stops the pump.
  if (!pumpRunning && !wateringAfterPending && now - lastCommandPollMs >= COMMAND_POLL_MS) {
    lastCommandPollMs = now;
    pollManualCommand();
  }

  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    sendSensorData();
  }

  // Reconnect Wi-Fi at most every 30 s if it drops (sensors/watering keep working offline).
  // Never opens the setup hotspot from here - only the boot attempt and the BOOT button do.
  if (WiFi.status() != WL_CONNECTED && now - lastWiFiAttemptMs >= 30000) {
    lastWiFiAttemptMs = now;
    connectWiFi(false);
  }
}
