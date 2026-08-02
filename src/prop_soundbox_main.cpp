#include <Arduino.h>
#include <ArduinoJson.h>
#include <DFRobotDFPlayerMini.h>

#include "PropCore.h"
#include "wifi_config.h"
#include "prop_config.h"
#include "prop_soundbox_config.h"

// DFPlayer Mini talks over UART at 9600 baud. Wired to the ESP32's second
// hardware UART: ESP32 TX2 (GPIO17) -> DFPlayer RX (through a ~1k resistor,
// since the module's RX isn't 5V tolerant), ESP32 RX2 (GPIO16) <- DFPlayer TX.
static const int DFPLAYER_RX_PIN = 16;
static const int DFPLAYER_TX_PIN = 17;
static const int MAX_VOLUME = 30;

DFRobotDFPlayerMini dfPlayer;
bool dfPlayerReady = false;
int currentVolume = SOUNDBOX_DEFAULT_VOLUME;

// Picks a random file (1..trackCount) from a folder and plays it. Folder
// playback requires the SD card to have a "01".."99" style directory with
// sequentially numbered "001.mp3".."NNN.mp3" files - see prop_soundbox_config.h.
void playRandomFromFolder(int folder, int trackCount) {
  if (!dfPlayerReady) {
    Serial.println("DFPlayer not ready - skipping playback");
    return;
  }
  int track = random(1, trackCount + 1);
  Serial.printf("Playing folder %d track %d\n", folder, track);
  dfPlayer.playFolder(folder, track);
}

void setVolume(int volume) {
  currentVolume = constrain(volume, 0, MAX_VOLUME);
  if (dfPlayerReady) {
    dfPlayer.volume(currentVolume);
  }
  Serial.printf("Volume set to %d\n", currentVolume);
}

// Effect names are interpreted here (see prop_config.h.example for the full
// list): "default"/"alternate" play a random track from their configured
// folder, "standby" stops playback, "volume-up"/"volume-down" step the
// volume. Anything else falls back to "default".
void handleTrigger(AsyncWebServerRequest *request, JsonVariant &json) {
  String effect = "default";
  if (json.is<JsonObject>() && json["effect"].is<const char *>()) {
    effect = json["effect"].as<String>();
  }

  if (effect == "standby") {
    Serial.println("Standby: stopping playback");
    if (dfPlayerReady) dfPlayer.stop();
  } else if (effect == "volume-up") {
    setVolume(currentVolume + SOUNDBOX_VOLUME_STEP);
  } else if (effect == "volume-down") {
    setVolume(currentVolume - SOUNDBOX_VOLUME_STEP);
  } else if (effect == "alternate") {
    playRandomFromFolder(SOUNDBOX_ALTERNATE_FOLDER, SOUNDBOX_ALTERNATE_TRACK_COUNT);
  } else {
    effect = "default";
    playRandomFromFolder(SOUNDBOX_DEFAULT_FOLDER, SOUNDBOX_DEFAULT_TRACK_COUNT);
  }

  JsonDocument response;
  response["ok"] = true;
  response["effect"] = effect;
  response["playing"] = dfPlayerReady;
  response["volume"] = currentVolume;
  String body;
  serializeJson(response, body);
  request->send(200, "application/json", body);
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  Serial2.begin(9600, SERIAL_8N1, DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
  dfPlayerReady = dfPlayer.begin(Serial2, /*isACK=*/true, /*doReset=*/true);
  if (dfPlayerReady) {
    dfPlayer.volume(currentVolume);
    Serial.println("DFPlayer Mini ready");
  } else {
    Serial.println("DFPlayer Mini not detected - continuing without audio playback");
  }

  propCoreBegin(PROP_NAME, PROP_EFFECTS, PROP_EFFECTS_COUNT, WIFI_SSID, WIFI_PASSWORD, handleTrigger);
}

void loop() {
  propCoreLoop();
}
