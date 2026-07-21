#pragma once
// =====================================================================
// wiim_client.h  -  WiiM (LinkPlay) local HTTP API client for ESP32-S3
// Target: Waveshare ESP32-S3-Knob-Touch-LCD-1.8
//
// Self-contained. No dependency on the LVGL UI - it just fetches state
// and sends commands. The UI layer reads WiimState and calls the
// wiim_* action functions. Uses ArduinoJson (install via Library Mgr).
//
// WiiM API notes:
//  - Older firmware: http  on port 80
//  - Newer firmware: https on port 443 with a SELF-SIGNED cert
//    -> we use WiFiClientSecure + setInsecure() to skip validation.
//  - We auto-detect: try HTTPS first, fall back to HTTP.
// =====================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ------------------------- CONFIG -----------------------------------
// Set your WiiM's IP. STRONGLY recommend a DHCP reservation on the
// router so this never changes. Find it in the WiiM Home app ->
// device -> info, or your router's client list.
// Runtime WiiM IP: loaded from NVS at boot (default below), editable
// live from the web config page (wiim_webcfg.h).
#ifndef WIIM_IP_DEFAULT
#define WIIM_IP_DEFAULT "192.168.1.93"
#endif
String wiim_ip_addr = WIIM_IP_DEFAULT;

#ifndef WIIM_USE_HTTPS
#define WIIM_USE_HTTPS 1                    // 1 = try https:443 first
#endif

#define WIIM_HTTP_TIMEOUT_MS   1500

// ------------------------- STATE ------------------------------------
static volatile uint32_t _vol_touch_ms = 0;   // millis() of last local volume change

struct WiimState {
  bool     online       = false;   // did the last poll succeed?
  bool     playing      = false;   // transport state == play
  int      volume       = 0;       // 0..100
  bool     muted        = false;
  bool     shuffle      = false;   // loopmode 2 or 3 = shuffle on
  uint32_t curPosMs     = 0;       // current position (ms)
  uint32_t totLenMs     = 0;       // track length (ms)
  String   title        = "";
  String   artist       = "";
  String   album        = "";
  String   albumArtUrl  = "";      // http URL to cover art (if any)
  uint32_t lastUpdateMs = 0;       // millis() of last good poll
};

extern WiimState wiim;

// ------------------------- INTERNAL ---------------------------------
// Build a base URL, honoring the detected scheme/port.
static bool     _wiim_https   = (WIIM_USE_HTTPS != 0);
static bool     _wiim_probed  = false;

static String _wiim_base() {
  if (_wiim_https) return String("https://") + wiim_ip_addr;
  return String("http://") + wiim_ip_addr;
}

// ---- offline backoff -------------------------------------------------
// When the WiiM is off/unreachable, every TLS connect blocks for the full
// timeout. After 2 consecutive failures we fast-fail all requests for 10s,
// then allow one retry, and so on. First success clears the backoff.
static int      _wiim_fail_count  = 0;
static uint32_t _wiim_next_try_ms = 0;

// Perform a GET, return body in `out`. Returns true on HTTP 200.
// PERFORMANCE: the HTTPClient + TLS client are static with setReuse(true),
// so the TCP+TLS connection stays open between requests. The expensive
// TLS handshake (hundreds of ms of crypto) happens ONCE, not per request.
// All callers are serialized by wiim_mutex, so the statics are safe.
static bool _wiim_get(const String& cmd, String& out) {
  // fast-fail while in backoff: no blocking connect to a dead host
  if (_wiim_next_try_ms && millis() < _wiim_next_try_ms) {
    Serial.printf("[wiim] SKIP (backoff %lums left) %s\n",
                  (unsigned long)(_wiim_next_try_ms - millis()), cmd.c_str());
    return false;
  }

  String url = _wiim_base() + "/httpapi.asp?command=" + cmd;
  bool ok = false;

  if (_wiim_https) {
    static WiFiClientSecure sclient;
    static HTTPClient http;
    static bool inited = false;
    if (!inited) {
      sclient.setInsecure();             // WiiM uses a self-signed cert
      sclient.setTimeout(WIIM_HTTP_TIMEOUT_MS / 1000);
      http.setReuse(true);               // keep-alive: reuse the connection
      inited = true;
    }
    if (http.begin(sclient, url)) {
      http.setTimeout(WIIM_HTTP_TIMEOUT_MS);
      int code = http.GET();
      if (code == 200) { out = http.getString(); ok = true; }
      http.end();   // with setReuse(true) the connection stays open
    }
    if (!ok) sclient.stop();   // failed attempt: drop the socket cleanly
  } else {
    static WiFiClient client;
    static HTTPClient http;
    static bool inited = false;
    if (!inited) { http.setReuse(true); inited = true; }
    if (http.begin(client, url)) {
      http.setTimeout(WIIM_HTTP_TIMEOUT_MS);
      int code = http.GET();
      if (code == 200) { out = http.getString(); ok = true; }
      http.end();
    }
  }
  Serial.printf("[wiim] %s %s (https=%d fails=%d)\n",
                ok ? "OK  " : "FAIL", cmd.c_str(), (int)_wiim_https, _wiim_fail_count);
  if (ok) {
    _wiim_fail_count = 0;
    _wiim_next_try_ms = 0;
  } else {
    if (++_wiim_fail_count >= 2)
      _wiim_next_try_ms = millis() + 10000;   // back off for 10s
  }
  return ok;
}

// Try HTTPS first, then HTTP; remember whichever worked.
static bool _wiim_probe() {
  String body;
  _wiim_https = true;
  if (_wiim_get("getStatusEx", body)) { _wiim_probed = true; return true; }
  _wiim_https = false;
  if (_wiim_get("getStatusEx", body)) { _wiim_probed = true; return true; }
  return false;
}

// Fire-and-forget command (we still read body to complete the request).
static bool _wiim_cmd(const String& cmd) {
  if (!_wiim_probed) { if (!_wiim_probe()) return false; }
  String body;
  return _wiim_get(cmd, body);
}

// Hex-encoded fields sometimes appear in getMetaInfo/getPlayerStatus.
// LinkPlay returns Title/Artist/Album as HEX strings in getPlayerStatus.
// getMetaInfo returns plain UTF-8 JSON. We prefer getMetaInfo for text.
static String _hexToStr(const String& hex) {
  if (hex.length() == 0 || (hex.length() % 2) != 0) return hex;
  // crude check: all hex chars?
  for (size_t i = 0; i < hex.length(); i++) {
    char c = hex[i];
    bool h = (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
    if (!h) return hex;   // not hex, return as-is
  }
  String out; out.reserve(hex.length()/2);
  for (size_t i = 0; i + 1 < hex.length(); i += 2) {
    char b[3] = { hex[i], hex[i+1], 0 };
    out += (char) strtol(b, nullptr, 16);
  }
  return out;
}

// ------------------------- PUBLIC API -------------------------------

// Poll transport state + volume + position. Call ~1/sec.
static bool wiim_poll_status() {
  if (WiFi.status() != WL_CONNECTED) { wiim.online = false; return false; }
  if (!_wiim_probed) { if (!_wiim_probe()) { wiim.online = false; return false; } }

  String body;
  if (!_wiim_get("getPlayerStatus", body)) { wiim.online = false; return false; }

  StaticJsonDocument<1536> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) { wiim.online = false; return false; }

  const char* status = doc["status"] | "";          // "play"/"pause"/"stop"
  wiim.playing  = (strcmp(status, "play") == 0);

  // WiiM returns ALL values as quoted strings ("vol":"25"), so read them
  // as strings and convert with atoi/atol rather than as native ints.
  // Don't clobber the volume the user just set with the knob: skip the
  // polled value for a short window after the last local change.
  if (doc.containsKey("vol") && (millis() - _vol_touch_ms > 1200))
    wiim.volume = atoi(doc["vol"] | "0");
  if (doc.containsKey("mute"))   wiim.muted    = (atoi(doc["mute"] | "0") == 1);
  if (doc.containsKey("loop")) { int lm = atoi(doc["loop"] | "4"); wiim.shuffle = (lm==2||lm==3); }
  if (doc.containsKey("curpos")) wiim.curPosMs = (uint32_t)atol(doc["curpos"] | "0");
  if (doc.containsKey("totlen")) wiim.totLenMs = (uint32_t)atol(doc["totlen"] | "0");

  // getPlayerStatus text fields are hex-encoded on LinkPlay
  if (doc.containsKey("Title"))  wiim.title  = _hexToStr(String((const char*)doc["Title"]));
  if (doc.containsKey("Artist")) wiim.artist = _hexToStr(String((const char*)doc["Artist"]));
  if (doc.containsKey("Album"))  wiim.album  = _hexToStr(String((const char*)doc["Album"]));

  wiim.online = true;
  wiim.lastUpdateMs = millis();
  return true;
}

// Richer metadata incl. album-art URL. Call less often (e.g. on track change).
static bool wiim_poll_meta() {
  if (!_wiim_probed) { if (!_wiim_probe()) return false; }
  String body;
  if (!_wiim_get("getMetaInfo", body)) return false;

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, body)) return false;

  JsonObject m = doc["metaData"];
  if (m.isNull()) return false;

  if (m.containsKey("title"))     wiim.title  = String((const char*)m["title"]);
  if (m.containsKey("artist"))    wiim.artist = String((const char*)m["artist"]);
  if (m.containsKey("album"))     wiim.album  = String((const char*)m["album"]);
  if (m.containsKey("albumArtURI"))
    wiim.albumArtUrl = String((const char*)m["albumArtURI"]);
  else if (m.containsKey("albumArtUri"))
    wiim.albumArtUrl = String((const char*)m["albumArtUri"]);
  return true;
}

// ---- Actions -------------------------------------------------------
static void wiim_toggle_play() { _wiim_cmd("setPlayerCmd:onepause"); }
static void wiim_next()        { _wiim_cmd("setPlayerCmd:next"); }
static void wiim_prev()        { _wiim_cmd("setPlayerCmd:prev"); }

static void wiim_set_volume(int v) {
  if (v < 0) v = 0; if (v > 100) v = 100;
  wiim.volume = v;
  _wiim_cmd(String("setPlayerCmd:vol:") + v);
}
static void wiim_volume_delta(int d) { wiim_set_volume(wiim.volume + d); }

static void wiim_seek_sec(uint32_t sec) {
  _wiim_cmd(String("setPlayerCmd:seek:") + sec);
}
// ---- presets -------------------------------------------------------
#define WIIM_MAX_PRESETS 12
struct WiimPreset { int number; char name[48]; };
static WiimPreset wiim_presets[WIIM_MAX_PRESETS];
static int wiim_preset_count = 0;

// Fetch the preset list (names set in the WiiM Home app).
static bool wiim_fetch_presets() {
  String body;
  if (!_wiim_get("getPresetInfo", body)) return false;
  DynamicJsonDocument doc(12288);
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
  wiim_preset_count = 0;
  JsonArray list = doc["preset_list"].as<JsonArray>();
  for (JsonObject p : list) {
    if (wiim_preset_count >= WIIM_MAX_PRESETS) break;
    WiimPreset &dst = wiim_presets[wiim_preset_count];
    dst.number = p["number"] | (wiim_preset_count + 1);
    const char *nm = p["name"] | "";
    if (!nm[0]) snprintf(dst.name, sizeof(dst.name), "Preset %d", dst.number);
    else { strncpy(dst.name, nm, sizeof(dst.name)-1); dst.name[sizeof(dst.name)-1] = 0; }
    wiim_preset_count++;
  }
  return true;
}

// Start preset n (1..12) -- same as pressing the preset button in the app.
static void wiim_play_preset(int n) {
  _wiim_cmd(String("MCUKeyShortClick:") + n);
}

// Sends the CURRENT wiim.muted state to the device. The caller flips
// wiim.muted first (optimistic UI), so this must NOT flip it again.
static void wiim_apply_mute() {
  _wiim_cmd(String("setPlayerCmd:mute:") + (wiim.muted ? 1 : 0));
}
