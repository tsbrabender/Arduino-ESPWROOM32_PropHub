#include <Arduino.h>
#include <ArduinoJson.h>

#include "PropCore.h"
#include "wifi_config.h"
#include "prop_config.h"
#include "prop_gpio_config.h"

static_assert(GPIO_TRIGGER_PIN_COUNT >= 1 && GPIO_TRIGGER_PIN_COUNT <= 3,
              "prop_gpio supports 1-3 GPIO_TRIGGER_PINS entries (one per toggle)");

// One "toggle-N" trigger event per configured GPIO_TRIGGER_PINS entry -
// unlike other prop types, these are fixed here rather than read from
// prop_config.h's PROP_TRIGGER_EVENTS, since this prop's effects are
// intrinsically tied to how many GPIOs are wired up, not freely nameable.
// All of them fire an effect (pulse a GPIO), so this prop has no
// EventConfigs - see PropCore.h for the TriggerEvents/EventConfigs split.
static const char *TOGGLE_EFFECTS[] = {"toggle-1", "toggle-2", "toggle-3"};

// Tracks an in-progress pulse per pin so loop() - not the request handler -
// brings it back LOW after GPIO_TRIGGER_PULSE_MS. AsyncWebServer invokes
// handleTrigger() from inside the TCP stack's own callback context, and
// blocking there with delay() (even briefly) stalls the HTTP response and
// can time the client out. The handler must do zero blocking work.
bool pulseActive[GPIO_TRIGGER_PIN_COUNT];
unsigned long pulseStartMs[GPIO_TRIGGER_PIN_COUNT];

// Stub trigger handler: finds the requested toggle's index, drives the
// correspondingly-indexed GPIO_TRIGGER_PINS entry HIGH, and responds
// immediately - loop() turns it back LOW once GPIO_TRIGGER_PULSE_MS elapses.
// Swap this out for real hardware (relay, solenoid, servo, etc).
void handleTrigger(AsyncWebServerRequest *request, JsonVariant &json) {
  String effect = TOGGLE_EFFECTS[0];
  if (json.is<JsonObject>() && json["effect"].is<const char *>()) {
    effect = json["effect"].as<String>();
  }

  int toggleIndex = -1;
  for (size_t i = 0; i < GPIO_TRIGGER_PIN_COUNT; i++) {
    if (effect == TOGGLE_EFFECTS[i]) {
      toggleIndex = i;
      break;
    }
  }

  bool triggered = false;
  if (toggleIndex >= 0) {
    digitalWrite(GPIO_TRIGGER_PINS[toggleIndex], HIGH);
    pulseActive[toggleIndex] = true;
    pulseStartMs[toggleIndex] = millis();
    triggered = true;
  } else {
    Serial.printf("Triggered unknown toggle: %s\n", effect.c_str());
  }

  JsonDocument response;
  response["ok"] = true;
  response["effect"] = effect;
  response["triggered"] = triggered;
  String body;
  serializeJson(response, body);
  request->send(200, "application/json", body);
}

void setup() {
  Serial.begin(115200);
  for (size_t i = 0; i < GPIO_TRIGGER_PIN_COUNT; i++) {
    pinMode(GPIO_TRIGGER_PINS[i], OUTPUT);
    digitalWrite(GPIO_TRIGGER_PINS[i], LOW);
  }
  propCoreBegin(PROP_NAME, TOGGLE_EFFECTS, GPIO_TRIGGER_PIN_COUNT,
                nullptr, 0, WIFI_SSID, WIFI_PASSWORD, handleTrigger);
}

void loop() {
  propCoreLoop();

  for (size_t i = 0; i < GPIO_TRIGGER_PIN_COUNT; i++) {
    if (pulseActive[i] && millis() - pulseStartMs[i] >= GPIO_TRIGGER_PULSE_MS) {
      digitalWrite(GPIO_TRIGGER_PINS[i], LOW);
      pulseActive[i] = false;
      propCoreLog("Triggered %s (GPIO%d)", TOGGLE_EFFECTS[i], GPIO_TRIGGER_PINS[i]);
    }
  }
}
