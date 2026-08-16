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
#include <vector>

namespace {

const unsigned long HEARTBEAT_INTERVAL_MS = 5000;
// The hub always runs its softAP on this address (ESP32 default AP gateway IP).
const char *HUB_BASE_URL = "http://192.168.4.1";

// Where the prop's (possibly user-renamed) name and its TriggerEvents'
// (possibly user-renamed) display labels are persisted, so both survive
// reboots and reflashing with new firmware. ESP32 uses NVS (Preferences);
// ESP8266 has no NVS equivalent, so it uses flash-emulated EEPROM instead -
// only a full chip erase clears either.
#if defined(ESP32)
const char *NVS_NAMESPACE = "propcore";
const char *NVS_KEY_NAME = "name";
// Value is a JSON object of only the TriggerEvents whose label differs from
// their canonical id, e.g. {"default":"Crickets"} - see
// persistTriggerEventLabels().
const char *NVS_KEY_TRIGGER_LABELS = "trigLabels";
#elif defined(ESP8266)
// Two length-prefixed slots: byte 0 is the stored name's length (0 means
// "nothing saved yet"), bytes 1..(NAME_SLOT_SIZE-1) hold the name. Byte
// NAME_SLOT_SIZE is the stored trigger-labels JSON's length, the rest of
// that slot holds the JSON itself (same format as the NVS value above).
const int NAME_SLOT_SIZE = 64;
const int TRIGGER_LABELS_SLOT_SIZE = 256;
const int TRIGGER_LABELS_OFFSET = NAME_SLOT_SIZE;
const int EEPROM_SIZE = NAME_SLOT_SIZE + TRIGGER_LABELS_SLOT_SIZE;
#endif

AsyncWebServer server(80);
AsyncCorsMiddleware cors;
unsigned long lastHeartbeatMs = 0;

String g_propName;
const char *const *g_triggerEvents = nullptr;
size_t g_triggerEventsCount = 0;
// Display label per triggerEvents[i], defaulting to the canonical id and
// overridable per-id via POST /trigger-events/rename (see
// handleTriggerEventRename()). The canonical id - never the label - is what
// actually gets sent back in the /trigger request, since that's what a
// prop's onTrigger handler dispatches on.
std::vector<String> g_triggerEventLabels;
const char *const *g_eventConfigs = nullptr;
size_t g_eventConfigsCount = 0;
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
  if (len > 0 && len <= NAME_SLOT_SIZE - 1) {
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
  uint8_t len = nameLen > (size_t)(NAME_SLOT_SIZE - 1) ? (uint8_t)(NAME_SLOT_SIZE - 1) : (uint8_t)nameLen;
  EEPROM.write(0, len);
  for (uint8_t i = 0; i < len; i++) {
    EEPROM.write(1 + i, name[i]);
  }
  EEPROM.commit();
  EEPROM.end();
#endif
}

String loadStoredTriggerLabelsJson() {
#if defined(ESP32)
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
  String stored = prefs.getString(NVS_KEY_TRIGGER_LABELS, "");
  prefs.end();
  return stored;
#elif defined(ESP8266)
  EEPROM.begin(EEPROM_SIZE);
  uint8_t len = EEPROM.read(TRIGGER_LABELS_OFFSET);
  String stored;
  if (len > 0 && len <= TRIGGER_LABELS_SLOT_SIZE - 1) {
    stored.reserve(len);
    for (uint8_t i = 0; i < len; i++) {
      stored += (char)EEPROM.read(TRIGGER_LABELS_OFFSET + 1 + i);
    }
  }
  EEPROM.end();
  return stored;
#endif
}

void saveStoredTriggerLabelsJson(const String &json) {
#if defined(ESP32)
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  prefs.putString(NVS_KEY_TRIGGER_LABELS, json);
  prefs.end();
#elif defined(ESP8266)
  EEPROM.begin(EEPROM_SIZE);
  size_t jsonLen = json.length();
  uint8_t len = jsonLen > (size_t)(TRIGGER_LABELS_SLOT_SIZE - 1) ? (uint8_t)(TRIGGER_LABELS_SLOT_SIZE - 1) : (uint8_t)jsonLen;
  EEPROM.write(TRIGGER_LABELS_OFFSET, len);
  for (uint8_t i = 0; i < len; i++) {
    EEPROM.write(TRIGGER_LABELS_OFFSET + 1 + i, json[i]);
  }
  EEPROM.commit();
  EEPROM.end();
#endif
}

// Index of `id` within g_triggerEvents, or -1 if it's not one of this prop's
// configured TriggerEvents (e.g. an EventConfig, or an unknown/typo'd id).
int findTriggerEventIndex(const String &id) {
  for (size_t i = 0; i < g_triggerEventsCount; i++) {
    if (id == g_triggerEvents[i]) return (int)i;
  }
  return -1;
}

// Loads any persisted label overrides and applies them onto
// g_triggerEventLabels (already seeded 1:1 with the canonical ids by the
// caller). Malformed/missing stored JSON just leaves the canonical ids in
// place.
void applyStoredTriggerEventLabels() {
  String storedJson = loadStoredTriggerLabelsJson();
  if (storedJson.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, storedJson)) return;
  for (JsonPair kv : doc.as<JsonObject>()) {
    int index = findTriggerEventIndex(kv.key().c_str());
    if (index >= 0) {
      g_triggerEventLabels[index] = kv.value().as<String>();
    }
  }
}

// Persists only the TriggerEvents whose label differs from their canonical
// id, so reverting one back to its id also stops persisting it.
void persistTriggerEventLabels() {
  JsonDocument doc;
  for (size_t i = 0; i < g_triggerEventsCount; i++) {
    if (g_triggerEventLabels[i] != String(g_triggerEvents[i])) {
      doc[g_triggerEvents[i]] = g_triggerEventLabels[i];
    }
  }
  String json;
  serializeJson(doc, json);
  saveStoredTriggerLabelsJson(json);
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
  JsonArray triggerEvents = doc["triggerEvents"].to<JsonArray>();
  for (size_t i = 0; i < g_triggerEventsCount; i++) {
    JsonObject event = triggerEvents.add<JsonObject>();
    event["id"] = g_triggerEvents[i];
    event["label"] = g_triggerEventLabels[i];
  }
  JsonArray eventConfigs = doc["eventConfigs"].to<JsonArray>();
  for (size_t i = 0; i < g_eventConfigsCount; i++) {
    eventConfigs.add(g_eventConfigs[i]);
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

// User-facing TriggerEvent rename, POSTed directly to this prop's IP from
// the dashboard (same direct-to-prop pattern as handleRename() above). Only
// TriggerEvents can be renamed - EventConfigs aren't in g_triggerEvents, so
// their ids simply won't be found here. An empty label reverts that
// TriggerEvent back to its canonical id rather than being rejected, since
// there'd otherwise be no way to undo a rename from the dashboard. Persists
// so the label survives reboots and future reflashes, then immediately
// re-registers with the hub so it shows up without waiting for the next
// heartbeat.
void handleTriggerEventRename(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();
  String id = body["id"] | "";
  String label = body["label"] | "";
  label.trim();

  int index = findTriggerEventIndex(id);
  if (index < 0) {
    request->send(404, "application/json", "{\"error\":\"unknown trigger event\"}");
    return;
  }

  g_triggerEventLabels[index] = label.isEmpty() ? String(g_triggerEvents[index]) : label;
  persistTriggerEventLabels();

  registerWithHub();

  JsonDocument response;
  response["ok"] = true;
  response["id"] = id;
  response["label"] = g_triggerEventLabels[index];
  String responseBody;
  serializeJson(response, responseBody);
  request->send(200, "application/json", responseBody);
}

} // namespace

void propCoreBegin(const char *propName,
                    const char *const *triggerEvents, size_t triggerEventsCount,
                    const char *const *eventConfigs, size_t eventConfigsCount,
                    const char *wifiSsid, const char *wifiPassword,
                    ArJsonRequestHandlerFunction onTrigger) {
  String storedName = loadStoredName();
  g_propName = storedName.length() > 0 ? storedName : String(propName);

  g_triggerEvents = triggerEvents;
  g_triggerEventsCount = triggerEventsCount;
  g_triggerEventLabels.clear();
  for (size_t i = 0; i < triggerEventsCount; i++) {
    g_triggerEventLabels.push_back(String(triggerEvents[i]));
  }
  applyStoredTriggerEventLabels();
  g_eventConfigs = eventConfigs;
  g_eventConfigsCount = eventConfigsCount;
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
  server.addHandler(new AsyncCallbackJsonWebHandler("/trigger-events/rename", handleTriggerEventRename));
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
