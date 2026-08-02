# Arduino-ESPWROOM32_SoundBox

A Halloween prop control system: one **hub** board runs its own Wi-Fi access point and a web dashboard; any number of **prop** boards join that network, self-register with the hub, and expose a trigger endpoint the dashboard can fire.

## Wi-Fi credentials

Copy `include/wifi_config.h.example` to `include/wifi_config.h` and fill in the SSID/password for the network the hub will broadcast (props join this same network). `wifi_config.h` is gitignored.

## Building the hub

The hub serves a React dashboard (device info, memory, network, running FreeRTOS tasks, and a Props panel) from LittleFS at `http://soundbox.local/` (or `http://192.168.4.1/`), backed by `/api/status` and `/api/nodes` JSON endpoints.

```
cd web
npm install
npm run build                            # builds into ../data
cd ..
pio run -e esp32dev --target uploadfs    # flashes the filesystem image
pio run -e esp32dev --target upload      # flashes the hub firmware
```

For local frontend development, `npm run dev` in `web/` proxies `/api` requests to `http://soundbox.local`.

## Building a prop node

Each physical prop needs its own identity. Copy `include/prop_config.h.example` to `include/prop_config.h` and set a unique `PROP_NAME` (and effect list) before flashing. `prop_config.h` is gitignored — re-edit it before flashing each additional prop.

```
pio run -e prop_node --target upload
```

A prop node joins the hub's Wi-Fi network, registers itself (name, IP, effects) with the hub every 5s, and exposes `POST /trigger` (currently a stub: logs the effect and blinks the onboard LED — swap in real audio/relay/servo hardware in `handleTrigger()` in `src/prop_main.cpp`). The dashboard's Props panel calls each node's `/trigger` endpoint directly over the local network, so triggering isn't relayed through the hub.
