#pragma once

#include <ESPAsyncWebServer.h>

// Shared behavior for every prop node type: joins the hub's AP, registers
// (and periodically re-registers) with it, and runs the /trigger endpoint.
// Each prop's main.cpp supplies its identity, effect list, and the handler
// that actually does something with a trigger.
void propCoreBegin(const char *propName, const char *const *effects, size_t effectsCount,
                    const char *wifiSsid, const char *wifiPassword,
                    ArJsonRequestHandlerFunction onTrigger);
void propCoreLoop();
