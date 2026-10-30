#include <Preferences.h>

Preferences prefs;

void setup() {
  Serial.begin(115200);
  delay(500);

  if (!prefs.begin("pechay", false)) {
    Serial.println("Could not open the Pechay preferences.");
    return;
  }

  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();

  Serial.println("Saved Wi-Fi name and password erased.");
  Serial.println("Upload the pechay_watering sketch to set up Wi-Fi again.");
}

void loop() {
}
