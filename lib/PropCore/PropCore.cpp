#include "PropCore.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <stdarg.h>

namespace {

const unsigned long HEARTBEAT_INTERVAL_MS = 5000;
// The hub always runs its softAP on this address (ESP32 default AP gateway IP).
const char *HUB_BASE_URL = "http://192.168.4.1";

// NVS namespace/key the prop's (possibly user-renamed) name is persisted
// under, so it survives reboots and reflashing with new firmware - only a
// full chip erase clears NVS.
const char *NVS_NAMESPACE = "propcore";
const char *NVS_KEY_NAME = "name";

AsyncWebServer server(80);
AsyncCorsMiddleware cors;
unsigned long lastHeartbeatMs = 0;

String g_propName;
const char *const *g_effects = nullptr;
size_t g_effectsCount = 0;
const char *g_wifiSsid = nullptr;
const char *g_wifiPassword = nullptr;

void joinHub() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_wifiSsid, g_wifiPassword);

  Serial.printf("Joining %s", g_wifiSsid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nJoined, IP address: %s\n", WiFi.localIP().toString().c_str());
}

void registerWithHub() {
  JsonDocument doc;
  doc["id"] = WiFi.macAddress();
  doc["name"] = g_propName;
  doc["ip"] = WiFi.localIP().toString();
  JsonArray effects = doc["effects"].to<JsonArray>();
  for (size_t i = 0; i < g_effectsCount; i++) {
    effects.add(g_effects[i]);
  }
  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(String(HUB_BASE_URL) + "/api/nodes/register");
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(body);
  if (status <= 0) {
    Serial.printf("Hub registration failed: %s\n", http.errorToString(status).c_str());
  } else {
    Serial.printf("Registered with hub (HTTP %d)\n", status);
  }
  http.end();
}

// User-facing rename, POSTed directly to this prop's IP from the dashboard
// (see PropLogPage/App.jsx pattern for /trigger). Persists to NVS so the
// name survives reboots and future reflashes, then immediately re-registers
// with the hub so the new name shows up without waiting for the next
// heartbeat.
void handleRename(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();
  String newName = body["name"] | "";
  newName.trim();
  if (newName.isEmpty()) {
    request->send(400, "application/json", "{\"error\":\"missing name\"}");
    return;
  }

  g_propName = newName;

  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  prefs.putString(NVS_KEY_NAME, g_propName);
  prefs.end();

  registerWithHub();

  JsonDocument response;
  response["ok"] = true;
  response["name"] = g_propName;
  String responseBody;
  serializeJson(response, responseBody);
  request->send(200, "application/json", responseBody);
}

} // namespace

void propCoreBegin(const char *propName, const char *const *effects, size_t effectsCount,
                    const char *wifiSsid, const char *wifiPassword,
                    ArJsonRequestHandlerFunction onTrigger) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
  String storedName = prefs.getString(NVS_KEY_NAME, "");
  prefs.end();
  g_propName = storedName.length() > 0 ? storedName : String(propName);

  g_effects = effects;
  g_effectsCount = effectsCount;
  g_wifiSsid = wifiSsid;
  g_wifiPassword = wifiPassword;

  joinHub();

  cors.setOrigin("*");
  cors.setMethods("POST, OPTIONS");
  cors.setHeaders("Content-Type");
  cors.setAllowCredentials(false);
  server.addMiddleware(&cors);

  server.addHandler(new AsyncCallbackJsonWebHandler("/trigger", onTrigger));
  server.addHandler(new AsyncCallbackJsonWebHandler("/rename", handleRename));
  server.begin();
  Serial.println("Trigger server started");

  registerWithHub();
  lastHeartbeatMs = millis();
}

void propCoreLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    joinHub();
  }
  if (millis() - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
    registerWithHub();
    lastHeartbeatMs = millis();
  }
}

void propCoreLog(const char *fmt, ...) {
  char buffer[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  Serial.println(buffer);

  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument doc;
  doc["id"] = WiFi.macAddress();
  doc["message"] = buffer;
  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(String(HUB_BASE_URL) + "/api/nodes/log");
  http.addHeader("Content-Type", "application/json");
  http.POST(body);
  http.end();
}
