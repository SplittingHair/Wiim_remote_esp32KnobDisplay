#pragma once
// =====================================================================
//  wiim_webcfg.h - config web page served while connected (STA mode).
//  Open http://<esp-ip>/  or  http://wiim-knob.local/  from a phone:
//    - change WiFi SSID/password (saved to NVS, reboots to apply)
//    - change the WiiM IP (saved to NVS, applies immediately)
//  The captive portal (wiim_portal.h) remains the no-network fallback.
// =====================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "wiim_portal.h"    // portal_save_creds()

// runtime WiiM IP (loaded from NVS in the .ino; used by wiim_client.h)
extern String wiim_ip_addr;

static WebServer _cfg_server(80);

static void _cfg_save_wiim_ip(const String &ip) {
  Preferences p; p.begin("knob", false);
  p.putString("wiim_ip", ip);
  p.end();
}

static void _cfg_handle_root() {
  String html =
    "<html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;margin:24px;max-width:420px}"
    "input{font-size:1.1em;width:100%;padding:8px;margin:4px 0 14px;box-sizing:border-box}"
    "h2{margin-top:0}.s{color:#666;font-size:.9em}"
    "input[type=submit]{background:#1DB954;color:#fff;border:0;border-radius:8px;padding:12px}"
    "</style></head><body>"
    "<h2>WiiM Knob &mdash; Settings</h2>"
    "<p class=s>Connected to <b>" + WiFi.SSID() + "</b> (" + WiFi.localIP().toString() +
    ", " + String(WiFi.RSSI()) + " dBm)</p>"
    "<form action=/save method=post>"
    "WiFi network (leave empty to keep current):"
    "<input name=ssid placeholder='" + WiFi.SSID() + "'>"
    "WiFi password:"
    "<input name=pass type=password placeholder='(unchanged)'>"
    "WiiM IP address:"
    "<input name=wiim value='" + wiim_ip_addr + "'>"
    "<input type=submit value='Save'>"
    "</form>"
    "<p class=s>Changing WiFi reboots the knob. Changing only the WiiM IP applies instantly.</p>"
    "</body></html>";
  _cfg_server.send(200, "text/html", html);
}

static void _cfg_handle_save() {
  String ssid = _cfg_server.arg("ssid");
  String pass = _cfg_server.arg("pass");
  String wiim = _cfg_server.arg("wiim");

  bool wifi_changed = ssid.length() > 0;
  if (wiim.length() > 0 && wiim != wiim_ip_addr) {
    wiim_ip_addr = wiim;
    _cfg_save_wiim_ip(wiim);
  }
  if (wifi_changed) {
    portal_save_creds(ssid, pass);
    _cfg_server.send(200, "text/html",
      "<h2>Saved. Rebooting to join '" + ssid + "'&hellip;</h2>");
    delay(800);
    ESP.restart();
  } else {
    _cfg_server.send(200, "text/html",
      "<h2>Saved.</h2><p>WiiM IP is now " + wiim_ip_addr +
      ".</p><p><a href='/'>back</a></p>");
  }
}

// Call once after WiFi connects.
static void webcfg_start() {
  MDNS.begin("wiim-knob");                 // http://wiim-knob.local
  MDNS.addService("http", "tcp", 80);
  _cfg_server.on("/",     _cfg_handle_root);
  _cfg_server.on("/save", HTTP_POST, _cfg_handle_save);
  _cfg_server.begin();
}

// Small task: service web requests.
static void webcfg_task(void *arg) {
  for (;;) {
    _cfg_server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
