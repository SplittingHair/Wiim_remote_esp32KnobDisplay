#pragma once
// =====================================================================
//  wiim_portal.h - WiFi captive-portal setup + credential storage (NVS)
//  Join the "WiiM-Knob-Setup" hotspot with a phone; a page lets you pick
//  a network and enter the password. Saved creds survive reboots.
// =====================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

static bool portal_load_creds(String &ssid, String &pass) {
  Preferences p; p.begin("wifi", true);
  ssid = p.getString("ssid", ""); pass = p.getString("pass", "");
  p.end();
  return ssid.length() > 0;
}

static void portal_save_creds(const String &s, const String &pw) {
  Preferences p; p.begin("wifi", false);
  p.putString("ssid", s); p.putString("pass", pw);
  p.end();
}

// Blocking. Runs the AP + portal until creds are saved (device restarts)
// or timeout_ms passes (device restarts back to normal mode).
static void portal_run(uint32_t timeout_ms = 300000) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);              // AP for the portal, STA for scanning
  WiFi.softAP("WiiM-Knob-Setup");
  IPAddress apIP = WiFi.softAPIP();

  DNSServer dns;  dns.start(53, "*", apIP);   // captive: all DNS -> us
  WebServer server(80);

  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n && i < 20; i++)
    opts += "<option>" + WiFi.SSID(i) + "</option>";

  server.on("/", [&]() {
    String html =
      "<html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:sans-serif;margin:24px}input,select{font-size:1.1em;width:100%;"
      "padding:6px;margin:4px 0}</style></head><body>"
      "<h2>WiiM Knob &mdash; WiFi Setup</h2>"
      "<form action=/save method=post>"
      "Pick a network:<select name=ssid>" + opts + "</select>"
      "or type one:<input name=ssid2 placeholder='(optional)'>"
      "Password:<input name=pass type=password>"
      "<br><input type=submit value='Save &amp; Reboot' style='padding:10px'>"
      "</form></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, [&]() {
    String s  = server.arg("ssid2"); if (!s.length()) s = server.arg("ssid");
    String pw = server.arg("pass");
    portal_save_creds(s, pw);
    server.send(200, "text/html", "<h2>Saved. Rebooting&hellip;</h2>");
    delay(800);
    ESP.restart();
  });

  // captive-portal redirect for any other URL
  server.onNotFound([&]() {
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
  }
  ESP.restart();   // timeout -> back to normal boot
}
