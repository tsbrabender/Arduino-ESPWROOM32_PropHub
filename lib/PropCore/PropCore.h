#pragma once

#include <ESPAsyncWebServer.h>

// Shared behavior for every prop node type: joins the hub's AP, registers
// (and periodically re-registers) with it, and runs the /trigger endpoint.
// Each prop's main.cpp supplies its identity, its two trigger lists, and the
// handler that actually does something with a trigger.
//
// Every trigger name the prop reports falls into one of two categories,
// both fired through the same POST /trigger {"effect": "..."} request -
// onTrigger is responsible for telling them apart by name:
//   - triggerEvents: fires an effect - plays a sound, pulses a GPIO, etc.
//   - eventConfigs: alters/configures prop state instead - volume, standby.
// The hub tracks and displays the two lists separately so the dashboard can
// group "do something" buttons apart from "change how it behaves" ones.
// Pass triggerEventsCount/eventConfigsCount 0 (with a null list) if a prop
// has none of one kind - prop_gpio, for example, has no configs today.
//
// Each triggerEvents[i] also has a user-editable display label (the
// dashboard's "rename" pencil next to each TriggerEvent button), settable
// via POST /trigger-events/rename {"id": "<canonical id>", "label": "..."}
// and persisted on-device (NVS on ESP32, EEPROM on ESP8266) so it survives
// reboots and reflashes - see loadStoredTriggerLabelsJson() in PropCore.cpp.
// The canonical id in triggerEvents[] never changes and is always what's
// sent back in the POST /trigger request; only the label shown on the
// dashboard and reported to the hub changes. EventConfigs aren't renameable.
void propCoreBegin(const char *propName,
                    const char *const *triggerEvents, size_t triggerEventsCount,
                    const char *const *eventConfigs, size_t eventConfigsCount,
                    const char *wifiSsid, const char *wifiPassword,
                    ArJsonRequestHandlerFunction onTrigger);
void propCoreLoop();

// Prints to Serial as usual, and - if the hub is reachable - forwards the
// same message to it (POST /api/nodes/log) so it shows up in the prop's log
// page on the dashboard instead of only being visible over a serial cable.
void propCoreLog(const char *fmt, ...);
