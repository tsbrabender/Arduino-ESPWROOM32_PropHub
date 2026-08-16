# Arduino-ESPWROOM32_PropHub

A Halloween prop control system: one **hub** board runs its own Wi-Fi access point and a web dashboard; any number of **prop** boards join that network, self-register with the hub, and expose a trigger endpoint the dashboard can fire.

## Trigger types: TriggerEvents vs EventConfigs

Every trigger name a prop reports to the hub falls into one of two categories, both fired through the same `POST /trigger {"effect": "name"}` request — a prop's `handleTrigger()` tells them apart by name, not by request shape:

- **TriggerEvents** — fires an effect: play a sound, pulse a GPIO, etc. (`default`/`alternate`/`alternate2`/`alternate3` on the soundbox prop, `toggle-N` on the GPIO prop).
- **EventConfigs** — alters/configures the prop's state instead of firing an effect (`standby`, `volume-up`, `volume-down` on the soundbox prop).

Each prop passes both lists to `propCoreBegin()` (see `lib/PropCore/PropCore.h`) and reports them to the hub as separate `triggerEvents`/`eventConfigs` arrays on registration. The dashboard's Props panel renders them as two visually distinct button groups per prop, with EventConfigs set off by a dashed divider, so at a glance you can tell "do something" buttons apart from "change how it behaves" ones. A prop with no configs (like the GPIO prop) just reports an empty EventConfigs list.

## Wi-Fi credentials

Copy `include/wifi_config.h.example` to `include/wifi_config.h` and fill in the SSID/password for the network the hub will broadcast (props join this same network). `wifi_config.h` is gitignored.

## Building the hub

The hub serves a React dashboard (device info, memory, network, running FreeRTOS tasks, and a Props panel) from LittleFS at `http://prophub.local/` (or `http://192.168.4.1/`), backed by `/api/status` and `/api/nodes` JSON endpoints.

```
cd web
npm install
npm run build                            # builds into ../data
cd ..
pio run -e prop_hub --target uploadfs    # flashes the filesystem image
pio run -e prop_hub --target upload      # flashes the hub firmware
```

For local frontend development, `npm run dev` in `web/` proxies `/api` requests to `http://prophub.local`.

## Building a prop node

Each physical prop needs its own identity. Copy `include/prop_config.h.example` to `include/prop_config.h` and set a unique `PROP_NAME` (and `PROP_TRIGGER_EVENTS`/`PROP_EVENT_CONFIGS` lists — see [Trigger types](#trigger-types-triggerevents-vs-eventconfigs) above) before flashing. `prop_config.h` is gitignored — re-edit it before flashing each additional prop.

```
pio run -e prop_node --target upload
```

A prop node joins the hub's Wi-Fi network, registers itself (name, IP, trigger events, event configs) with the hub every 5s, and exposes `POST /trigger` (currently a stub: logs the effect and blinks the onboard LED — swap in real audio/relay/servo hardware in `handleTrigger()` in `src/prop_main.cpp`). The dashboard's Props panel calls each node's `/trigger` endpoint directly over the local network, so triggering isn't relayed through the hub.

`wifi_config.h` is shared by every environment, hub included — if you change it, reflash the hub too, or props won't be able to join its (now different) network.

Every prop can also be renamed after the fact from the dashboard (click its name in the Props panel) — the new name is persisted on the device itself (NVS on ESP32, EEPROM on ESP8266) and survives reboots and future reflashes.

## Building a GPIO trigger prop

A stub prop for ESP8266MOD-based boards (`esp12e` pinout) that exposes up to three momentary "toggle" TriggerEvents — pressing one briefly pulses a GPIO HIGH then LOW, a starting point for relay, solenoid, or similar hardware. Copy `include/prop_gpio_config.h.example` to `include/prop_gpio_config.h` and set `GPIO_TRIGGER_PINS` (1-3 entries) to match your wiring; each entry becomes one `toggle-N` button on the dashboard. Unlike other prop types, `PROP_TRIGGER_EVENTS`/`PROP_EVENT_CONFIGS` in `prop_config.h` aren't used here — only `PROP_NAME` is. This prop has no EventConfigs.

```
pio run -e prop_gpio --target upload
```

Everything else (hub join/registration, logging, rename) works the same as any other prop via the shared `PropCore` library — swap the stub pulse logic in `handleTrigger()` in `src/prop_gpio_main.cpp` for real hardware.

## Troubleshooting

**`pio` isn't recognized as a command** — PlatformIO's CLI often isn't on `PATH` outside the IDE extension's own terminal. Either call it by full path (e.g. `C:\Users\<you>\.platformio\penv\Scripts\pio.exe run -e prop_node --target upload`) or add that `Scripts` folder to your user `PATH` once.

**Upload fails with `Wrong boot mode detected (0x13)`** — the board didn't auto-enter the bootloader (common when the auto-reset circuit isn't wired/working). Hold **BOOT**, tap **EN/RESET** while still holding BOOT, then release BOOT, and retry the upload.

**Confirming a prop actually registered with the hub** — join the hub's Wi-Fi network from your PC/phone and open `http://192.168.4.1/` (or `http://prophub.local/`); the Props panel lists every registered node with an online/offline dot. Alternatively, watch the prop's own serial output (`pio device monitor -p <port> -b 115200`) for `Registered with hub (HTTP 200)`; `Hub registration failed: ...` means it joined Wi-Fi but couldn't reach the hub's `/api/nodes/register` endpoint.
