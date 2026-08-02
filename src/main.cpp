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

struct PropNode {
  String id;
  String name;
  String ip;
  std::vector<String> effects;
  unsigned long lastSeenMs;
};

std::vector<PropNode> propNodes;

void handleRegisterNode(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();
  String id = body["id"] | "";
  if (id.isEmpty()) {
    request->send(400, "application/json", "{\"error\":\"missing id\"}");
    return;
  }

  PropNode *node = nullptr;
  for (auto &n : propNodes) {
    if (n.id == id) {
      node = &n;
      break;
    }
  }
  if (node == nullptr) {
    propNodes.push_back(PropNode{});
    node = &propNodes.back();
    node->id = id;
    Serial.printf("Prop node registered: %s\n", id.c_str());
  }

  node->name = body["name"] | id;
  node->ip = body["ip"] | "";
  node->effects.clear();
  if (body["effects"].is<JsonArray>()) {
    for (JsonVariant effect : body["effects"].as<JsonArray>()) {
      node->effects.push_back(effect.as<String>());
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
    JsonArray effects = node["effects"].to<JsonArray>();
    for (auto &e : n.effects) {
      effects.add(e);
    }
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
