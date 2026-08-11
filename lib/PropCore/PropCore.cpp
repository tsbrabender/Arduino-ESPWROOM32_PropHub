#include "PropCore.h"

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>
#else
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#endif

#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <stdarg.h>

namespace {

const unsigned long HEARTBEAT_INTERVAL_MS = 5000;
// The hub always runs its softAP on this address (ESP32 default AP gateway IP).
const char *HUB_BASE_URL = "http://192.168.4.1";

// Where the prop's (possibly user-renamed) name is persisted, so it survives
// reboots and reflashing with new firmware. ESP32 uses NVS (Preferences);
// ESP8266 has no NVS equivalent, so it uses flash-emulated EEPROM instead -
// only a full chip erase clears either.
#if defined(ESP32)
const char *NVS_NAMESPACE = "propcore";
const char *NVS_KEY_NAME = "name";
#elif defined(ESP8266)
// Byte 0 is the stored name's length (0 or 0xFF/255 means "nothing saved
// yet"), bytes 1..EEPROM_SIZE-1 hold the name itself.
const int EEPROM_SIZE = 64;
#endif

AsyncWebServer server(80);
AsyncCorsMiddleware cors;
unsigned long lastHeartbeatMs = 0;

String g_propName;
const char *const *g_effects = nullptr;
size_t g_effectsCount = 0;
const char *g_wifiSsid = nullptr;
const char *g_wifiPassword = nullptr;

// ESP8266's HTTPClient requires an explicit WiFiClient; ESP32's doesn't.
void beginHttp(HTTPClient &http, const String &url) {
#if defined(ESP8266)
  static WiFiClient wifiClient;
  http.begin(wifiClient, url);
#else
  http.begin(url);
#endif
}

String loadStoredName() {
#if defined(ESP32)
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
  String stored = prefs.getString(NVS_KEY_NAME, "");
  prefs.end();
  return stored;
#elif defined(ESP8266)
  EEPROM.begin(EEPROM_SIZE);
  uint8_t len = EEPROM.read(0);
  String stored;
  if (len > 0 && len <= EEPROM_SIZE - 1) {
    stored.reserve(len);
    for (uint8_t i = 0; i < len; i++) {
      stored += (char)EEPROM.read(1 + i);
    }
  }
  EEPROM.end();
  return stored;
#endif
}

void saveStoredName(const String &name) {
#if defined(ESP32)
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  prefs.putString(NVS_KEY_NAME, name);
  prefs.end();
#elif defined(ESP8266)
  EEPROM.begin(EEPROM_SIZE);
  size_t nameLen = name.length();
  uint8_t len = nameLen > (size_t)(EEPROM_SIZE - 1) ? (uint8_t)(EEPROM_SIZE - 1) : (uint8_t)nameLen;
  EEPROM.write(0, len);
  for (uint8_t i = 0; i < len; i++) {
    EEPROM.write(1 + i, name[i]);
  }
  EEPROM.commit();
  EEPROM.end();
#endif
}

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
  beginHttp(http, String(HUB_BASE_URL) + "/api/nodes/register");
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
// (see PropsPanel in App.jsx for the /trigger-style direct-to-prop pattern).
// Persists so the name survives reboots and future reflashes, then
// immediately re-registers with the hub so the new name shows up without
// waiting for the next heartbeat.
void handleRename(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();
  String newName = body["name"] | "";
  newName.trim();
  if (newName.isEmpty()) {
    request->send(400, "application/json", "{\"error\":\"missing name\"}");
    return;
  }

  g_propName = newName;
  saveStoredName(g_propName);

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
  String storedName = loadStoredName();
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
  beginHttp(http, String(HUB_BASE_URL) + "/api/nodes/log");
  http.addHeader("Content-Type", "application/json");
  http.POST(body);
  http.end();
}
