#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>

#include "wifi_config.h"

static const char *MDNS_HOSTNAME = "prophub";

AsyncWebServer server(80);

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Access point \"%s\" started, IP address: %s\n", WIFI_SSID, WiFi.softAPIP().toString().c_str());
}

const char *taskStateToString(eTaskState state) {
  switch (state) {
    case eRunning: return "Running";
    case eReady: return "Ready";
    case eBlocked: return "Blocked";
    case eSuspended: return "Suspended";
    case eDeleted: return "Deleted";
    default: return "Invalid";
  }
}

// The stock Arduino-ESP32 core ships FreeRTOS with configUSE_TRACE_FACILITY
// disabled, so uxTaskGetSystemState() (a full task enumeration) isn't
// available. Instead, probe the well-known tasks the core and our libraries
// create by name via xTaskGetHandle(), which works without trace facility.
static const char *kKnownTaskNames[] = {
  "loopTask", "IDLE0",   "IDLE1",  "Tmr Svc",   "wifi",
  "esp_timer", "async_tcp", "ipc0", "ipc1",     "sys_evt",
};

void handleApiStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;

  JsonObject device = doc["device"].to<JsonObject>();
  device["chipModel"] = ESP.getChipModel();
  device["chipRevision"] = ESP.getChipRevision();
  device["cores"] = ESP.getChipCores();
  device["cpuFreqMHz"] = ESP.getCpuFreqMHz();
  device["flashSizeBytes"] = ESP.getFlashChipSize();
  device["sdkVersion"] = ESP.getSdkVersion();
  device["macAddress"] = WiFi.softAPmacAddress();

  JsonObject memory = doc["memory"].to<JsonObject>();
  memory["heapFreeBytes"] = ESP.getFreeHeap();
  memory["heapTotalBytes"] = ESP.getHeapSize();
  memory["heapMinFreeBytes"] = ESP.getMinFreeHeap();
  memory["psramTotalBytes"] = ESP.getPsramSize();
  memory["psramFreeBytes"] = ESP.getFreePsram();

  JsonObject network = doc["network"].to<JsonObject>();
  network["mode"] = "AP";
  network["ssid"] = WIFI_SSID;
  network["ipAddress"] = WiFi.softAPIP().toString();
  network["connectedClients"] = WiFi.softAPgetStationNum();

  doc["uptimeMs"] = millis();

  JsonArray tasks = doc["tasks"].to<JsonArray>();
  for (const char *name : kKnownTaskNames) {
    TaskHandle_t handle = xTaskGetHandle(name);
    if (handle == nullptr) continue;

    JsonObject task = tasks.add<JsonObject>();
    task["name"] = name;
    task["state"] = taskStateToString(eTaskGetState(handle));
    task["priority"] = uxTaskPriorityGet(handle);
    task["coreId"] = xTaskGetAffinity(handle);
    task["stackHighWaterMark"] = uxTaskGetStackHighWaterMark(handle) * sizeof(StackType_t);
  }

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  serializeJson(doc, *response);
  request->send(response);
}

// A prop node is considered offline if it hasn't re-registered within this window.
// Prop nodes re-register every PROP_HEARTBEAT_MS (see prop_main.cpp), well under this.
static const unsigned long NODE_STALE_MS = 15000;

// How many of each node's most recent status messages (see PropCore::propCoreLog)
// to keep. Oldest entries are dropped once this cap is hit.
static const size_t MAX_LOG_ENTRIES = 100;

struct LogEntry {
  unsigned long ts;
  String message;
};

// id is the canonical trigger name a prop's onTrigger handler dispatches on
// (what POST /trigger actually sends) and never changes; label is the
// possibly user-renamed display text (see PropCore.h's
// POST /trigger-events/rename) shown on the dashboard instead.
struct TriggerEvent {
  String id;
  String label;
};

struct PropNode {
  String id;
  String name;
  String ip;
  // triggerEvents fire an effect (play a sound, pulse a GPIO) and can be
  // renamed for display; eventConfigs alter/configure prop state instead
  // (volume, standby) and can't be renamed - see PropCore.h.
  std::vector<TriggerEvent> triggerEvents;
  std::vector<String> eventConfigs;
  unsigned long lastSeenMs;
  std::vector<LogEntry> logs;
};

std::vector<PropNode> propNodes;

PropNode *findPropNode(const String &id) {
  for (auto &node : propNodes) {
    if (node.id == id) return &node;
  }
  return nullptr;
}

void handleRegisterNode(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();
  String id = body["id"] | "";
  if (id.isEmpty()) {
    request->send(400, "application/json", "{\"error\":\"missing id\"}");
    return;
  }

  PropNode *node = findPropNode(id);
  if (node == nullptr) {
    propNodes.push_back(PropNode{});
    node = &propNodes.back();
    node->id = id;
    Serial.printf("Prop node registered: %s\n", id.c_str());
  }

  node->name = body["name"] | id;
  node->ip = body["ip"] | "";
  node->triggerEvents.clear();
  if (body["triggerEvents"].is<JsonArray>()) {
    for (JsonVariant event : body["triggerEvents"].as<JsonArray>()) {
      JsonObject eventObj = event.as<JsonObject>();
      String eventId = eventObj["id"] | "";
      if (eventId.isEmpty()) continue;
      node->triggerEvents.push_back(TriggerEvent{eventId, eventObj["label"] | eventId});
    }
  }
  node->eventConfigs.clear();
  if (body["eventConfigs"].is<JsonArray>()) {
    for (JsonVariant config : body["eventConfigs"].as<JsonArray>()) {
      node->eventConfigs.push_back(config.as<String>());
    }
  }
  node->lastSeenMs = millis();

  request->send(200, "application/json", "{\"ok\":true}");
}

void handleGetNodes(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray nodes = doc["nodes"].to<JsonArray>();
  unsigned long now = millis();

  for (auto &n : propNodes) {
    JsonObject node = nodes.add<JsonObject>();
    node["id"] = n.id;
    node["name"] = n.name;
    node["ip"] = n.ip;
    node["online"] = (now - n.lastSeenMs) < NODE_STALE_MS;
    JsonArray triggerEvents = node["triggerEvents"].to<JsonArray>();
    for (auto &e : n.triggerEvents) {
      JsonObject event = triggerEvents.add<JsonObject>();
      event["id"] = e.id;
      event["label"] = e.label;
    }
    JsonArray eventConfigs = node["eventConfigs"].to<JsonArray>();
    for (auto &c : n.eventConfigs) {
      eventConfigs.add(c);
    }
  }

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  serializeJson(doc, *response);
  request->send(response);
}

// Status messages a prop sends via PropCore::propCoreLog, kept per-node so
// they're visible on the dashboard without a serial cable.
void handleNodeLog(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();
  String id = body["id"] | "";
  String message = body["message"] | "";
  if (id.isEmpty() || message.isEmpty()) {
    request->send(400, "application/json", "{\"error\":\"missing id or message\"}");
    return;
  }

  PropNode *node = findPropNode(id);
  if (node == nullptr) {
    request->send(404, "application/json", "{\"error\":\"unknown node\"}");
    return;
  }

  node->logs.push_back(LogEntry{millis(), message});
  if (node->logs.size() > MAX_LOG_ENTRIES) {
    node->logs.erase(node->logs.begin());
  }

  request->send(200, "application/json", "{\"ok\":true}");
}

void handleGetNodeLogs(AsyncWebServerRequest *request) {
  if (!request->hasParam("id")) {
    request->send(400, "application/json", "{\"error\":\"missing id\"}");
    return;
  }

  PropNode *node = findPropNode(request->getParam("id")->value());
  if (node == nullptr) {
    request->send(404, "application/json", "{\"error\":\"unknown node\"}");
    return;
  }

  JsonDocument doc;
  JsonArray logs = doc["logs"].to<JsonArray>();
  for (auto &entry : node->logs) {
    JsonObject e = logs.add<JsonObject>();
    e["ts"] = entry.ts;
    e["message"] = entry.message;
  }

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  serializeJson(doc, *response);
  request->send(response);
}

void startWebServer() {
  if (!LittleFS.begin(true)) {
    Serial.println("Failed to mount LittleFS");
    return;
  }

  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.printf("mDNS responder started: http://%s.local\n", MDNS_HOSTNAME);
  }

  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/nodes", HTTP_GET, handleGetNodes);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/nodes/register", handleRegisterNode));
  server.on("/api/nodes/logs", HTTP_GET, handleGetNodeLogs);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/nodes/log", handleNodeLog));
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.begin();
  Serial.println("Web server started");
}

void setup() {
  Serial.begin(115200);
  startAccessPoint();
  startWebServer();
}

void loop() {
}
