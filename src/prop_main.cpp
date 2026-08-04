#include <Arduino.h>
#include <ArduinoJson.h>

#include "PropCore.h"
#include "wifi_config.h"
#include "prop_config.h"

static const int LED_PIN = 2;

// Stub trigger handler: logs the requested effect and blinks the onboard LED.
// Swap this out for real hardware (audio playback, relay, servo, etc).
void handleTrigger(AsyncWebServerRequest *request, JsonVariant &json) {
  String effect = PROP_EFFECTS[0];
  if (json.is<JsonObject>() && json["effect"].is<const char *>()) {
    effect = json["effect"].as<String>();
  }

  propCoreLog("Triggered effect: %s", effect.c_str());
  digitalWrite(LED_PIN, HIGH);
  delay(150);
  digitalWrite(LED_PIN, LOW);

  JsonDocument response;
  response["ok"] = true;
  response["effect"] = effect;
  String body;
  serializeJson(response, body);
  request->send(200, "application/json", body);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  propCoreBegin(PROP_NAME, PROP_EFFECTS, PROP_EFFECTS_COUNT, WIFI_SSID, WIFI_PASSWORD, handleTrigger);
}

void loop() {
  propCoreLoop();
}
