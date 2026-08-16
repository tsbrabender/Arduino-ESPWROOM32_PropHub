#include <Arduino.h>
#include <ArduinoJson.h>
#include <DFRobotDFPlayerMini.h>
#include <string.h>

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

// The module's BUSY pin, active low while a track is playing. Driven
// directly by the chip - unlike the UART "finished" notification, it doesn't
// depend on the (unreliable, on this hardware) serial link.
static const int DFPLAYER_BUSY_PIN = 15;

DFRobotDFPlayerMini dfPlayer;
bool dfPlayerReady = false;
int currentVolume = SOUNDBOX_DEFAULT_VOLUME;

// Effective per-folder track counts used for random selection. Seeded from
// the configured constants and, if detectTrackCounts() finds the DFPlayer
// reports a different (valid) count at boot, overwritten with that instead
// - see detectTrackCounts() below.
int defaultTrackCount = SOUNDBOX_DEFAULT_TRACK_COUNT;
int alternateTrackCount = SOUNDBOX_ALTERNATE_TRACK_COUNT;
int alternate2TrackCount = SOUNDBOX_ALTERNATE2_TRACK_COUNT;
int alternate3TrackCount = SOUNDBOX_ALTERNATE3_TRACK_COUNT;

// MODE_DEFAULT/MODE_ALTERNATE/MODE_ALTERNATE2/MODE_ALTERNATE3 keep picking
// new random tracks from their folder as each one finishes (see loop()),
// until "standby" stops them.
enum SoundboxMode { MODE_STANDBY, MODE_DEFAULT, MODE_ALTERNATE, MODE_ALTERNATE2, MODE_ALTERNATE3 };
SoundboxMode currentMode = MODE_STANDBY;
unsigned long trackStartMs = 0;
unsigned long busyIdleSinceMs = 0; // 0 = not currently observed idle

// AsyncWebServer invokes handleTrigger() from inside the TCP stack's own
// callback context (see prop_gpio_main.cpp), where propCoreLog()'s network
// round-trip would stall the HTTP response - the handler must do zero
// blocking work. playRandomFromFolder()/setVolume()/handleTrigger() run from
// both that context and from loop() (track-finished advance), so they always
// queue through here instead of calling propCoreLog() directly; loop() sends
// the queued message.
char pendingLogMessage[192];
bool pendingLogPresent = false;

void queueLog(const char *message) {
  strncpy(pendingLogMessage, message, sizeof(pendingLogMessage) - 1);
  pendingLogMessage[sizeof(pendingLogMessage) - 1] = '\0';
  pendingLogPresent = true;
}

// Picks a random file (1..trackCount) from a folder and plays it. Folder
// playback requires the SD card to have a "01".."99" style directory with
// sequentially numbered "001.mp3".."NNN.mp3" files - see prop_soundbox_config.h.
void playRandomFromFolder(int folder, int trackCount) {
  if (!dfPlayerReady) {
    queueLog("DFPlayer not ready - skipping playback");
    return;
  }
  int track = random(1, trackCount + 1);
  char message[64];
  snprintf(message, sizeof(message), "Playing folder %d track %d", folder, track);
  queueLog(message);
  dfPlayer.playFolder(folder, track);
  trackStartMs = millis();
  busyIdleSinceMs = 0;
}

// Plays one random track for the given mode. Called both when a trigger
// switches into that mode and, from loop(), every time the DFPlayer reports
// the current track finished - that's what makes default/alternate keep
// playing random tracks back-to-back instead of stopping after one.
void playForMode(SoundboxMode mode) {
  if (mode == MODE_DEFAULT) {
    playRandomFromFolder(SOUNDBOX_DEFAULT_FOLDER, defaultTrackCount);
  } else if (mode == MODE_ALTERNATE) {
    playRandomFromFolder(SOUNDBOX_ALTERNATE_FOLDER, alternateTrackCount);
  } else if (mode == MODE_ALTERNATE2) {
    playRandomFromFolder(SOUNDBOX_ALTERNATE2_FOLDER, alternate2TrackCount);
  } else if (mode == MODE_ALTERNATE3) {
    playRandomFromFolder(SOUNDBOX_ALTERNATE3_FOLDER, alternate3TrackCount);
  }
}

void setVolume(int volume) {
  currentVolume = constrain(volume, 0, MAX_VOLUME);
  if (dfPlayerReady) {
    dfPlayer.volume(currentVolume);
  }
  char message[32];
  snprintf(message, sizeof(message), "Volume set to %d", currentVolume);
  queueLog(message);
}

// Effect names are interpreted here (see prop_config.h.example for the full
// list): "default"/"alternate"/"alternate2"/"alternate3" play a random track
// from their configured folder, "standby" stops playback, "volume-up"/
// "volume-down" step the volume. Anything else falls back to "default".
void handleTrigger(AsyncWebServerRequest *request, JsonVariant &json) {
  String effect = "default";
  if (json.is<JsonObject>() && json["effect"].is<const char *>()) {
    effect = json["effect"].as<String>();
  }

  if (effect == "standby") {
    queueLog("Standby: stopping playback");
    currentMode = MODE_STANDBY;
    if (dfPlayerReady) dfPlayer.stop();
  } else if (effect == "volume-up") {
    setVolume(currentVolume + SOUNDBOX_VOLUME_STEP);
  } else if (effect == "volume-down") {
    setVolume(currentVolume - SOUNDBOX_VOLUME_STEP);
  } else if (effect == "alternate") {
    currentMode = MODE_ALTERNATE;
    playForMode(currentMode);
  } else if (effect == "alternate2") {
    currentMode = MODE_ALTERNATE2;
    playForMode(currentMode);
  } else if (effect == "alternate3") {
    currentMode = MODE_ALTERNATE3;
    playForMode(currentMode);
  } else {
    effect = "default";
    currentMode = MODE_DEFAULT;
    playForMode(currentMode);
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

// Queries the DFPlayer for how many files it sees in `folder` (blocking
// UART round-trip, ~500ms timeout per the library default) - only safe to
// call from setup(), never from handleTrigger(). Falls back to
// `configuredCount` if the module doesn't answer or reports nothing, since
// the UART link on this hardware is already known to be unreliable (see the
// init retry loop and the BUSY-pin-over-UART-notification comments below).
int detectTrackCount(int folder, int configuredCount, const char *label) {
  int detected = dfPlayer.readFileCountsInFolder(folder);
  if (detected <= 0) {
    propCoreLog("Folder %d (%s): count query failed, using configured %d", folder, label, configuredCount);
    return configuredCount;
  }
  if (detected != configuredCount) {
    propCoreLog("Folder %d (%s): detected %d tracks (configured %d), using detected", folder, label, detected, configuredCount);
  } else {
    propCoreLog("Folder %d (%s): detected %d tracks, matches configured", folder, label, detected);
  }
  return detected;
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  pinMode(DFPLAYER_BUSY_PIN, INPUT_PULLUP);

  // Join the hub and register first, before the (potentially slow, up to 5
  // minutes below) DFPlayer handshake - so the prop shows up on the
  // dashboard immediately regardless of how long DFPlayer init takes, and so
  // propCoreLog() can already reach the hub while DFPlayer init is running.
  propCoreBegin(PROP_NAME, PROP_EFFECTS, PROP_EFFECTS_COUNT, WIFI_SSID, WIFI_PASSWORD, handleTrigger);

  Serial2.begin(9600, SERIAL_8N1, DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
  delay(1500); // DFPlayer Mini needs time to boot before it'll ACK the init handshake

  static const int DFPLAYER_BEGIN_ATTEMPTS = 300; // 300 attempts at 1s each = 5 minutes to wait for the DFPlayer to boot
  for (int attempt = 1; attempt <= DFPLAYER_BEGIN_ATTEMPTS && !dfPlayerReady; attempt++) {
    dfPlayerReady = dfPlayer.begin(Serial2, /*isACK=*/true, /*doReset=*/true);
    if (!dfPlayerReady) {
      propCoreLog("DFPlayer init attempt %d/%d failed", attempt, DFPLAYER_BEGIN_ATTEMPTS);
      delay(1000);
    }
  }
  // TEMP DIAGNOSTIC: some DFPlayer Mini clones (GD3200B chip) never ACK the
  // handshake above even when wired correctly, but do respond to playback
  // commands. Fall back to a non-ACK begin() so we can test actual audio.
  if (!dfPlayerReady) {
    propCoreLog("Falling back to non-ACK init for a live audio test");
    dfPlayerReady = dfPlayer.begin(Serial2, /*isACK=*/false, /*doReset=*/true);
  }
  if (dfPlayerReady) {
    dfPlayer.volume(currentVolume);
    propCoreLog("DFPlayer Mini ready");

    defaultTrackCount = detectTrackCount(SOUNDBOX_DEFAULT_FOLDER, SOUNDBOX_DEFAULT_TRACK_COUNT, "default");
    alternateTrackCount = detectTrackCount(SOUNDBOX_ALTERNATE_FOLDER, SOUNDBOX_ALTERNATE_TRACK_COUNT, "alternate");
    alternate2TrackCount = detectTrackCount(SOUNDBOX_ALTERNATE2_FOLDER, SOUNDBOX_ALTERNATE2_TRACK_COUNT, "alternate2");
    alternate3TrackCount = detectTrackCount(SOUNDBOX_ALTERNATE3_FOLDER, SOUNDBOX_ALTERNATE3_TRACK_COUNT, "alternate3");
  } else {
    propCoreLog("DFPlayer Mini not detected - continuing without audio playback");
  }
}

void loop() {
  propCoreLoop();

  if (pendingLogPresent) {
    pendingLogPresent = false;
    propCoreLog("%s", pendingLogMessage);
  }

  bool trackFinished = false;
  bool canAdvance = dfPlayerReady && currentMode != MODE_STANDBY;

  // Primary "finished" signal: the module's BUSY pin, driven directly by the
  // chip (active low while playing) rather than a UART message that can be
  // dropped. The grace period after a track starts avoids racing BUSY before
  // it's had a chance to drop low. A single HIGH read isn't trusted on its
  // own - BUSY has to keep reading idle for SOUNDBOX_BUSY_CONFIRM_MS before
  // it's treated as the track actually finishing, to guard against a brief
  // glitchy read.
  if (canAdvance && millis() - trackStartMs > SOUNDBOX_MIN_PLAY_MS) {
    if (digitalRead(DFPLAYER_BUSY_PIN) == HIGH) {
      if (busyIdleSinceMs == 0) {
        busyIdleSinceMs = millis();
      } else if (millis() - busyIdleSinceMs >= SOUNDBOX_BUSY_CONFIRM_MS) {
        propCoreLog("BUSY pin reports idle - advancing");
        trackFinished = true;
      }
    } else {
      busyIdleSinceMs = 0;
    }
  }

  // Secondary "finished" signal: the DFPlayer's own UART notification. Kept
  // as a backup alongside BUSY rather than relied on alone, since it's the
  // signal that proved unreliable on this hardware in the first place - but
  // it can still catch a finish BUSY misses (or vice versa). Polled
  // whenever the player is ready (not just while actively playing) so the
  // module's messages keep draining instead of piling up in the UART buffer.
  if (dfPlayerReady && dfPlayer.available()) {
    uint8_t type = dfPlayer.readType();
    dfPlayer.read(); // clears the parameter tied to this message
    if (type == DFPlayerPlayFinished && canAdvance) {
      propCoreLog("UART 'finished' notification received - advancing");
      trackFinished = true;
    }
  }

  if (trackFinished) {
    playForMode(currentMode);
  }
}
