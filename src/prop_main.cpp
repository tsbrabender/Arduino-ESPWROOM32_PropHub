#include <Arduino.h>
#include <ArduinoJson.h>

#include "PropCore.h"
#include "wifi_config.h"
#include "prop_config.h"

static const int LED_PIN = 2;
static const unsigned long TRIGGER_PULSE_MS = 150;

// Tracks an in-progress LED pulse and a pending log message so loop() - not
// the request handler - does the blocking work. AsyncWebServer invokes
// handleTrigger() from inside the TCP stack's own callback context (see
// prop_gpio_main.cpp), where both delay() and propCoreLog()'s network
// round-trip would stall the HTTP response - the handler must do zero
// blocking work.
bool pulseActive = false;
unsigned long pulseStartMs = 0;
bool pendingLog = false;
String pendingEffect;

// Stub trigger handler: records the requested effect and starts the LED
// pulse; loop() turns the LED back off and sends the log line.
// Swap this out for real hardware (audio playback, relay, servo, etc).
void handleTrigger(AsyncWebServerRequest *request, JsonVariant &json) {
  String effect = PROP_TRIGGER_EVENTS[0];
  if (json.is<JsonObject>() && json["effect"].is<const char *>()) {
    effect = json["effect"].as<String>();
  }

  digitalWrite(LED_PIN, HIGH);
  pulseActive = true;
  pulseStartMs = millis();
  pendingEffect = effect;
  pendingLog = true;

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
  propCoreBegin(PROP_NAME, PROP_TRIGGER_EVENTS, PROP_TRIGGER_EVENTS_COUNT,
                PROP_EVENT_CONFIGS, PROP_EVENT_CONFIGS_COUNT, WIFI_SSID, WIFI_PASSWORD, handleTrigger);
}

void loop() {
  propCoreLoop();

  if (pulseActive && millis() - pulseStartMs >= TRIGGER_PULSE_MS) {
    digitalWrite(LED_PIN, LOW);
    pulseActive = false;
  }

  if (pendingLog) {
    pendingLog = false;
    propCoreLog("Triggered effect: %s", pendingEffect.c_str());
  }
}
