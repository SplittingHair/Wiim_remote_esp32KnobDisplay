#pragma once
// =====================================================================
//  wiim_webcfg.h - one config web page for BOTH modes:
//   - STA (connected):  http://wiim-knob.local  or the device IP
//   - AP  (offline):    auto-hotspot "WiiM-Knob-Setup", page at
//                       http://192.168.4.1 (captive redirect included)
//  The AP raises automatically whenever WiFi is disconnected and shuts
//  down as soon as the real network connects. STA retries continue in
//  the background the whole time (AP+STA coexistence).
// =====================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "wiim_portal.h"    // portal_save_creds()/portal_load_creds()

extern String wiim_ip_addr;

static WebServer _cfg_server(80);
static DNSServer _cfg_dns;
static volatile bool _cfg_ap_active = false;

static void _cfg_save_wiim_ip(const String &ip) {
  Preferences p; p.begin("knob", false);
  p.putString("wiim_ip", ip);
  p.end();
}

// Scan nearby networks for the dropdown (runs in the webcfg task).
static String _cfg_scan_options() {
  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n && i < 25; i++)
    opts += "<option>" + WiFi.SSID(i) + "</option>";
  WiFi.scanDelete();
  if (!opts.length()) opts = "<option value=''>(no networks found - rescan)</option>";
  return opts;
}

static void _cfg_handle_root() {
  String status = _cfg_ap_active
    ? String("Hotspot mode &mdash; not connected to WiFi yet")
    : ("Connected to <b>" + WiFi.SSID() + "</b> (" + WiFi.localIP().toString()
       + ", " + String(WiFi.RSSI()) + " dBm)");
  String html =
    "<html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;margin:24px;max-width:420px}"
    "input,select{font-size:1.1em;width:100%;padding:8px;margin:4px 0 14px;box-sizing:border-box}"
    "h2{margin-top:0}.s{color:#666;font-size:.9em}"
    "input[type=submit]{background:#1DB954;color:#fff;border:0;border-radius:8px;padding:12px}"
    "</style></head><body>"
    "<h2>WiiM Knob &mdash; Settings</h2>"
    "<p class=s>" + status + "</p>"
    "<form action=/save method=post>"
    "Pick a network:"
    "<select name=ssid>" + _cfg_scan_options() + "</select>"
    "or type one:"
    "<input name=ssid2 placeholder='(optional)'>"
    "WiFi password:"
    "<input name=pass type=password>"
    "WiiM IP address:"
    "<input name=wiim value='" + wiim_ip_addr + "'>"
    "<input type=submit value='Save'>"
    "</form>"
    "<p class=s>Saving a WiFi network reboots the knob to join it. "
    "Changing only the WiiM IP applies instantly.</p>"
    "</body></html>";
  _cfg_server.send(200, "text/html", html);
}

static void _cfg_handle_save() {
  String ssid = _cfg_server.arg("ssid2"); if (!ssid.length()) ssid = _cfg_server.arg("ssid");
  String pass = _cfg_server.arg("pass");
  String wiim = _cfg_server.arg("wiim");

  if (wiim.length() > 0 && wiim != wiim_ip_addr) {
    wiim_ip_addr = wiim;
    _cfg_save_wiim_ip(wiim);
  }
  if (ssid.length() > 0) {
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

static void _cfg_handle_notfound() {
  if (_cfg_ap_active) {   // captive-portal redirect (phones auto-open the page)
    _cfg_server.sendHeader("Location",
      String("http://") + WiFi.softAPIP().toString(), true);
    _cfg_server.send(302, "text/plain", "");
  } else {
    _cfg_server.send(404, "text/plain", "not found");
  }
}

// Call ONCE at boot: the server runs in both AP and STA modes.
static void webcfg_begin() {
  _cfg_server.on("/",     _cfg_handle_root);
  _cfg_server.on("/save", HTTP_POST, _cfg_handle_save);
  _cfg_server.onNotFound(_cfg_handle_notfound);
  _cfg_server.begin();
}

// Call once after the first successful STA connection.
static void webcfg_mdns_start() {
  MDNS.begin("wiim-knob");                 // http://wiim-knob.local
  MDNS.addService("http", "tcp", 80);
}

// Raise / drop the fallback hotspot (STA retries keep running underneath).
static void webcfg_ap_on() {
  if (_cfg_ap_active) return;
  WiFi.disconnect(true);      // stop STA retries: they hop channels and
  WiFi.mode(WIFI_AP);         // break phone joins. Pure AP = stable hotspot.
  WiFi.softAP("WiiM-Knob-Setup");
  _cfg_dns.start(53, "*", WiFi.softAPIP());
  _cfg_ap_active = true;
  Serial.printf("[webcfg] hotspot ON  (config at http://%s)\n",
                WiFi.softAPIP().toString().c_str());
}
static void webcfg_ap_off() {
  if (!_cfg_ap_active) return;
  _cfg_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  _cfg_ap_active = false;
  Serial.println("[webcfg] hotspot OFF (connected)");
}

static void webcfg_task(void *arg) {
  for (;;) {
    _cfg_server.handleClient();
    if (_cfg_ap_active) _cfg_dns.processNextRequest();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
