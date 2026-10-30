// Test sketch: 3x YL-69 + DHT11 -> Wi-Fi -> backend dashboard
// Purpose: verify sensor wiring AND that the web dashboard captures readings from all 3 pots.
// No relay, no pump, no LCD, no Telegram - just the data path.
//
// How to use:
//   1. Start the backend first:  cd backend && npm start
//   2. Set the MOISTURE_RAW_AIR / MOISTURE_RAW_WATER values from your calibration run.
//   3. Upload, open Serial Monitor (115200). On first boot the ESP32 opens a "Pechay-Setup"
//      hotspot - join it from your phone, pick your WiFi and enter the laptop IP
//      (run `ipconfig` -> "IPv4 Address"). Settings are saved on the chip and are SHARED
//      with pechay_watering.ino - configure once, both sketches just work.
//      Need to change something? Hold the BOOT button ~4 s to reopen the hotspot.
//   4. Open http://localhost:3000 (or http://<laptop-IP>:3000 from your phone on same Wi-Fi).
//   5. Success = all three moisture cards and the chart update every ~10 seconds.
//      The watering log stays empty (no pump in this test) - that's expected.

#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <Preferences.h>   // built in: remembers your settings on the chip
#include <WebServer.h>     // built in: serves the setup page
#include <DNSServer.h>     // built in: makes the setup page pop up by itself

// ---------------- WIFI / SERVER CONFIG - set from your phone, not here ----------------
// Same setup portal and the same saved settings as pechay_watering.ino.
#define SERVER_PORT 3000
#define AP_SSID "Pechay-Setup"     // the hotspot your phone joins
#define SETUP_BUTTON_PIN 0         // the BOOT button on the DevKit
#define SETUP_HOLD_MS 4000         // hold BOOT this long to reopen the hotspot

// ---------------- PINS (same as pechay_watering.ino) ----------------
#define NUM_POTS 3
const int PIN_MOIST[NUM_POTS] = { 34, 35, 39 };  // YL-69 AO, one per pot. MUST be ADC1 pins
                                                 // (GPIO 32-39) - ADC2 fails when Wi-Fi is on.
#define PIN_DHT 33         // DHT11 DATA pin

// ---------------- YL-69 CALIBRATION (from calibrate_moisture sketch) ----------------
// One pair per pot: AIR = probe held dry in open air; WATER = the WETTEST raw value
// measured in that pot's soil (low raw = wet). Use the lowest value seen right after
// watering, not a cup-of-water value.
const int MOISTURE_RAW_AIR[NUM_POTS]   = { 4095, 4095, 4095 };  // dry air
const int MOISTURE_RAW_WATER[NUM_POTS] = { 1017, 1176, 1050 };  // wettest soil measured
// If a pot's percent reads backwards (100% in air), swap that pot's two values.

const unsigned long SEND_INTERVAL_MS = 10000;  // read + upload every 10 s

#define DHTTYPE DHT11
DHT dht(PIN_DHT, DHTTYPE);

unsigned long lastSendMs = 0;
unsigned long lastWiFiAttemptMs = 0;

float temperatureC = 0.0;  // last valid readings; reused if a DHT read fails
float humidityPct = 0.0;
bool dhtEverOk = false;

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

// ---------------- WIFI SETUP PORTAL (config is stored on the chip, not in this file) ----------------
// Storage is shared with pechay_watering.ino ("pechay" namespace) - set it up once,
// in whichever sketch you flash first.
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
                   "Nothing is uploaded until Wi-Fi is up.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Sensor -> dashboard test starting (3 pots)");
  dht.begin();
  loadConfig();
  connectWiFi(true);
  lastSendMs = millis() - SEND_INTERVAL_MS;  // first read happens immediately
}

void loop() {
  unsigned long now = millis();

  checkSetupButton();

  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;

    int m[NUM_POTS];
    for (int p = 0; p < NUM_POTS; p++) {
      m[p] = rawToPercent(p, readMoistureRaw(p));
    }

    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (isnan(h) || isnan(t)) {
      Serial.println("DHT11 read failed - reusing last valid values (check DATA pin wiring)");
    } else {
      humidityPct = h;
      temperatureC = t;
      dhtEverOk = true;
    }

    Serial.printf("M1=%d%%  M2=%d%%  M3=%d%%  temp=%.1fC  humidity=%.1f%%  |  ",
                  m[0], m[1], m[2], temperatureC, humidityPct);
    if (!dhtEverOk) Serial.print("(DHT11 never read OK - temp/humidity are placeholder 0.0) ");

    if (WiFi.status() == WL_CONNECTED) {
      char json[256];
      snprintf(json, sizeof(json),
               "{\"moisture1\":%d,\"moisture2\":%d,\"moisture3\":%d,"
               "\"temperature\":%.1f,\"humidity\":%.1f,\"pumpOn\":false}",
               m[0], m[1], m[2], temperatureC, humidityPct);
      HTTPClient http;
      http.setTimeout(8000);
      http.begin(serverUrl() + "/api/sensor-data");
      http.addHeader("Content-Type", "application/json");
      int code = http.POST(json);
      Serial.printf("POST -> HTTP %d  (%s)\n", code, code == 200 ? "dashboard should update" : "check server / IP");
      http.end();
    } else {
      Serial.println("Wi-Fi down - POST skipped");
    }
  }

  // Never opens the setup hotspot from here - only the boot attempt and the BOOT button do.
  if (WiFi.status() != WL_CONNECTED && now - lastWiFiAttemptMs >= 30000) {
    lastWiFiAttemptMs = now;
    connectWiFi(false);
  }
}
