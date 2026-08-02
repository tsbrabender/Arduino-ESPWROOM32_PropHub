#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>

#include "wifi_config.h"
#include "prop_config.h"

static const int LED_PIN = 2;
static const unsigned long HEARTBEAT_INTERVAL_MS = 5000;
// The hub always runs its softAP on this address (ESP32 default AP gateway IP).
static const char *HUB_BASE_URL = "http://192.168.4.1";

AsyncWebServer server(80);
AsyncCorsMiddleware cors;
unsigned long lastHeartbeatMs = 0;

void joinHub() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Joining %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nJoined, IP address: %s\n", WiFi.localIP().toString().c_str());
}

void registerWithHub() {
  JsonDocument doc;
  doc["id"] = WiFi.macAddress();
  doc["name"] = PROP_NAME;
  doc["ip"] = WiFi.localIP().toString();
  JsonArray effects = doc["effects"].to<JsonArray>();
  for (size_t i = 0; i < PROP_EFFECTS_COUNT; i++) {
    effects.add(PROP_EFFECTS[i]);
  }
  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(String(HUB_BASE_URL) + "/api/nodes/register");
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(body);
  if (status <= 0) {
    Serial.printf("Hub registration failed: %s\n", http.errorToString(status).c_str());
  }
  http.end();
}

// Stub trigger handler: logs the requested effect and blinks the onboard LED.
// Swap this out for real hardware (audio playback, relay, servo, etc).
void handleTrigger(AsyncWebServerRequest *request, JsonVariant &json) {
  String effect = PROP_EFFECTS[0];
  if (json.is<JsonObject>() && json["effect"].is<const char *>()) {
    effect = json["effect"].as<String>();
  }

  Serial.printf("Triggered effect: %s\n", effect.c_str());
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

void startTriggerServer() {
  cors.setOrigin("*");
  cors.setMethods("POST, OPTIONS");
  cors.setHeaders("Content-Type");
  cors.setAllowCredentials(false);
  server.addMiddleware(&cors);

  server.addHandler(new AsyncCallbackJsonWebHandler("/trigger", handleTrigger));
  server.begin();
  Serial.println("Trigger server started");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  joinHub();
  startTriggerServer();
  registerWithHub();
  lastHeartbeatMs = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    joinHub();
  }
  if (millis() - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
    registerWithHub();
    lastHeartbeatMs = millis();
  }
}
