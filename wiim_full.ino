// =====================================================================
//  WiiM Knob Controller - FULL build
//  Board: ESP32-S3-Knob-Touch-LCD-1.8  (16MB Flash / 8MB PSRAM)
//
//  >>> REQUIRED ARDUINO IDE SETTINGS <<<
//    Board: ESP32S3 Dev Module | Flash: 16MB (128Mb)
//    Partition: 16M Flash (3MB APP/9.9MB FATFS) | PSRAM: OPI PSRAM
//    USB CDC On Boot: Enabled
//  >>> REQUIRES lv_conf.h: LV_FONT_MONTSERRAT_20 = 1, _28 = 1 <<<
//
//  Main screen:  knob=volume(accel)  tap=play/pause
//                swipe L/R=next/prev  swipe DOWN=settings
//  Settings:     network status, sleep timer, WiFi captive portal
//                swipe UP = back
//  WiFi creds:   NVS (portal) -> fallback to hardcoded -> auto-portal
// =====================================================================

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <WiFi.h>
#include <Preferences.h>
#include "esp_task_wdt.h"

#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "bidi_switch_knob.h"

#include "wiim_client.h"
#include "wiim_ui.h"
#include "wiim_battery.h"
#include "wiim_portal.h"
#include "wiim_webcfg.h"

// ------------------------- USER CONFIG ------------------------------
// Optional hardcoded fallback network. Set SSID to "" to disable --
// the device then relies purely on saved credentials + the portal.
static const char* WIFI_SSID_FALLBACK = "";
static const char* WIFI_PASS_FALLBACK = "";
#define SCREEN_ON_DUTY   255      // brightness when awake (0..255)

// ------------------------- KNOB -------------------------------------
#define ENCODER_ECA_PIN    8
#define ENCODER_ECB_PIN    7
static knob_handle_t s_knob = 0;

static volatile int32_t _knob_delta = 0;
static portMUX_TYPE _knob_mux = portMUX_INITIALIZER_UNLOCKED;

// ------------------------- SHARED STATE -----------------------------
WiimState wiim;
static SemaphoreHandle_t wiim_mutex = NULL;
static Preferences prefs;

struct UiSnap { int vol; bool playing; bool online; char title[192]; char artist[192]; };
static UiSnap _ui_snap;

static void _ui_apply_cb(void *p) {
  wiim.volume  = _ui_snap.vol;
  wiim.playing = _ui_snap.playing;
  wiim.online  = _ui_snap.online;
  wiim.title   = _ui_snap.title;
  wiim.artist  = _ui_snap.artist;
  wiim_ui_refresh();
}

static void ui_request_refresh() {
  _ui_snap.vol     = wiim.volume;
  _ui_snap.playing = wiim.playing;
  _ui_snap.online  = wiim.online;
  strncpy((char*)_ui_snap.title,  wiim.title.c_str(),  sizeof(_ui_snap.title)-1);
  strncpy((char*)_ui_snap.artist, wiim.artist.c_str(), sizeof(_ui_snap.artist)-1);
  _ui_snap.title[sizeof(_ui_snap.title)-1]  = 0;
  _ui_snap.artist[sizeof(_ui_snap.artist)-1]= 0;
  // network status snapshot for the settings screen
  strncpy(_ui_net_ssid, WiFi.SSID().c_str(), sizeof(_ui_net_ssid)-1);
  _ui_net_ssid[sizeof(_ui_net_ssid)-1] = 0;
  strncpy(_ui_net_ip, WiFi.localIP().toString().c_str(), sizeof(_ui_net_ip)-1);
  _ui_net_ip[sizeof(_ui_net_ip)-1] = 0;
  _ui_net_rssi = WiFi.RSSI();
  lv_async_call(_ui_apply_cb, NULL);
}

static void _ui_vol_cb(void *p) { wiim_ui_set_volume((int)(intptr_t)p); }
// Runs in LVGL ctx. Pauses ALL rendering while the panel sleeps, so
// nothing ever flushes to a dead panel (which degraded the pipeline).
static void _panel_sleep_cb(void *p) {
  bool s = (bool)(intptr_t)p;
  lv_disp_t *d = lv_disp_get_default();
  if (s) {
    lcd_panel_sleep(true);                              // panel off first
    if (d && d->refr_timer) lv_timer_pause(d->refr_timer);   // then stop rendering
  } else {
    if (d && d->refr_timer) lv_timer_resume(d->refr_timer);  // rendering back on
    lcd_panel_sleep(false);                             // then wake the panel
  }
}
static void ui_request_volume(int v) { lv_async_call(_ui_vol_cb, (void*)(intptr_t)v); }


// ------------------------- WiiM thread-safe wrappers ----------------
static void safe_set_volume(int v) {
  if (xSemaphoreTake(wiim_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
    wiim_set_volume(v); xSemaphoreGive(wiim_mutex);
  }
}
static void safe_poll_status() {
  if (xSemaphoreTake(wiim_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
    wiim_poll_status(); xSemaphoreGive(wiim_mutex);
  }
}
static void safe_toggle_play() {
  if (xSemaphoreTake(wiim_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
    wiim_toggle_play(); xSemaphoreGive(wiim_mutex);
  }
}
static void safe_next() {
  if (xSemaphoreTake(wiim_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
    wiim_next(); xSemaphoreGive(wiim_mutex);
  }
}
static void safe_prev() {
  if (xSemaphoreTake(wiim_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
    wiim_prev(); xSemaphoreGive(wiim_mutex);
  }
}
static void safe_toggle_mute() {
  if (xSemaphoreTake(wiim_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
    wiim_apply_mute(); xSemaphoreGive(wiim_mutex);
  }
}

// ------------------------- KNOB CALLBACKS ---------------------------
static void _knob_left_cb (void *a, void *d){
  portENTER_CRITICAL(&_knob_mux); _knob_delta--; portEXIT_CRITICAL(&_knob_mux);
  _last_activity_ms = millis();
}
static void _knob_right_cb(void *a, void *d){
  portENTER_CRITICAL(&_knob_mux); _knob_delta++; portEXIT_CRITICAL(&_knob_mux);
  _last_activity_ms = millis();
}

// ------------------------- WIFI -------------------------------------
static bool wifi_connect(const char* ssid, const char* pass, uint32_t ms) {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);   // recover from any later disconnect
  WiFi.setSleep(true);   // modem sleep: major battery saving
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-t0 < ms){ delay(300); }
  return WiFi.status()==WL_CONNECTED;
}

// ------------------------- TASKS ------------------------------------
static void knob_task(void *arg) {
  int pending_vol = -1;
  uint32_t last_send = 0;
  for (;;) {
    portENTER_CRITICAL(&_knob_mux);
    int d = _knob_delta; _knob_delta = 0;
    portEXIT_CRITICAL(&_knob_mux);

    if (d != 0 && !_screen_asleep) {
      int mag = (d < 0) ? -d : d;
      int mult = (mag >= 6) ? 5 : (mag >= 3) ? 3 : 1;   // accel ~1.75x faster
      int v = wiim.volume + d * mult;
      if (v < 0) v = 0; if (v > 100) v = 100;
      wiim.volume = v;
      ui_request_volume(v);
      pending_vol = v;
    }

    if (pending_vol >= 0 && millis() - last_send >= 200) {
      int v = pending_vol; pending_vol = -1;
      last_send = millis();
      safe_set_volume(v);
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// Dedicated FAST action task: drains user actions every 20ms.
// Never does routine polling, so a tap/swipe never waits behind a poll.
static void action_task(void *arg) {
  for (;;) {
    if (_pending_action != WA_NONE) {
      WiimAction a = _pending_action;
      _pending_action = WA_NONE;

      // optimistic UI: flip the play state instantly for perceived speed
      if (a == WA_TOGGLE) { wiim.playing = !wiim.playing; ui_request_refresh(); }
      if (a == WA_MUTE)   { wiim.muted   = !wiim.muted;   ui_request_refresh(); }

      switch (a) {
        case WA_TOGGLE:        safe_toggle_play(); break;
        case WA_NEXT:          safe_next();        break;
        case WA_PREV:          safe_prev();        break;
        case WA_MUTE:          safe_toggle_mute(); break;
        case WA_START_PORTAL:  vTaskDelay(pdMS_TO_TICKS(600));
                               portal_run();       break;   // restarts device
        default: break;
      }
      // confirm real state right after the command
      if (WiFi.status()==WL_CONNECTED) { safe_poll_status(); ui_request_refresh(); }
    }

    // settings persistence (rare, cheap)
    if (_sleep_sel_changed) {
      _sleep_sel_changed = false;
      prefs.begin("knob", false);
      prefs.putInt("sleep_min", _ui_sleep_minutes);
      prefs.end();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Pure periodic poll: keeps track info fresh. Nothing else lives here.
static void _ui_show_off_cb (void *p) { wiim_ui_nav(SCR_OFFLINE); }
static void _ui_show_main2_cb(void *p) { wiim_ui_nav(SCR_MAIN); }

static void status_task(void *arg) {
  uint32_t last_reconnect = 0;
  uint32_t offline_since  = 0;
  bool     web_started    = false;
  for (;;) {
    if (WiFi.status()==WL_CONNECTED) {
      if (!web_started) {              // first successful connection
        web_started = true;
        webcfg_start();
        xTaskCreatePinnedToCore(webcfg_task, "webcfg_task", 6144, NULL, 1, NULL, 0);
      }
      safe_poll_status();
      if (!_screen_asleep) ui_request_refresh();   // never render to a sleeping panel
    } else {
      // WiFi dropped: say so, mark WiiM offline, and try to get back on
      if (millis() - last_reconnect > 10000) {
        last_reconnect = millis();
        Serial.printf("[wifi] disconnected (status=%d), reconnecting...\n",
                      (int)WiFi.status());
        WiFi.reconnect();
      }
      wiim.online = false;
      if (!_screen_asleep) ui_request_refresh();   // show the red dot
    }
    // ---- offline screen auto-switch (only between MAIN and OFFLINE) ----
    bool offline = (WiFi.status()!=WL_CONNECTED) || !wiim.online;
    if (offline) { if (!offline_since) offline_since = millis(); }
    else offline_since = 0;
    if (!_screen_asleep) {
      if (_ui_screen == SCR_MAIN && offline_since &&
          millis() - offline_since > 5000)
        lv_async_call(_ui_show_off_cb, NULL);
      else if (_ui_screen == SCR_OFFLINE && !offline)
        lv_async_call(_ui_show_main2_cb, NULL);
    }

    // battery: poll rarely while the screen is asleep
    vTaskDelay(pdMS_TO_TICKS(_screen_asleep ? 10000 : 1500));
  }
}

static void battery_task(void *arg) {
  battery_init();
  for (int i = 0; i < 5; i++) { battery_sample(); vTaskDelay(pdMS_TO_TICKS(200)); }
  for (;;) {
    battery_sample();
    _ui_batt_pct      = battery_percent();
    _ui_batt_charging = battery_charging();
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// display sleep: TRUE panel power-down after N idle minutes.
// Sleep: DCS display-off + sleep-in, CPU down to 80MHz.
// Wake (touch/knob): CPU back to 240MHz, panel sleep-out + display-on.
static void sleep_task(void *arg) {
  uint32_t last_dbg = 0;
  for (;;) {
    int m = _ui_sleep_minutes;
    uint32_t idle = millis() - _last_activity_ms;

    if (millis() - last_dbg > 5000) {
      last_dbg = millis();
      Serial.printf("[sleep] m=%d idle=%lus asleep=%d wifi=%d\n",
                    m, (unsigned long)(idle/1000), (int)_screen_asleep,
                    (int)(WiFi.status()==WL_CONNECTED));
    }

    if (!_screen_asleep && m > 0 && idle > (uint32_t)m * 60000UL) {
      Serial.println("[sleep] -> sleeping");
      _screen_asleep = true;                       // stop rendering first
      vTaskDelay(pdMS_TO_TICKS(100));              // let in-flight refresh finish
      lv_async_call(_panel_sleep_cb, (void*)1);    // panel off, in LVGL ctx
      vTaskDelay(pdMS_TO_TICKS(150));              // let the panel commands land
      setUpdutySubdivide(0);                       // cut GPIO47 rail: kills the glow
    } else if (_screen_asleep && idle < 2000) {
      Serial.println("[sleep] -> waking");
      setUpdutySubdivide(255);                     // rail back on FIRST
      vTaskDelay(pdMS_TO_TICKS(30));               // let it stabilize
      lv_async_call(_panel_sleep_cb, (void*)0);    // panel on, in LVGL ctx
      vTaskDelay(pdMS_TO_TICKS(250));              // let the panel wake finish
      _screen_asleep = false;                      // rendering resumes
      ui_request_refresh();                        // repaint fresh state
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ------------------------- SETUP / LOOP -----------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("[boot] reset reason: %d\n", (int)esp_reset_reason());

  // TLS handshakes are CPU-bound and can starve the idle task on core 0
  // during knob-volume bursts. Stop the task watchdog from rebooting us
  // over that: longer window, no idle-task watching, no panic.
  {
    esp_task_wdt_config_t wdt_cfg = {
      .timeout_ms     = 30000,
      .idle_core_mask = 0,      // don't watch the idle tasks
      .trigger_panic  = false,  // log instead of rebooting
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
  }

  Touch_Init();
  lcd_lvgl_Init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  static bool _built = false;
  lv_async_call([](void*){ if(!_built){ wiim_ui_build(); _built=true; } }, NULL);
  delay(200);

  // load saved sleep setting
  prefs.begin("knob", true);
  _ui_sleep_minutes = prefs.getInt("sleep_min", 0);
  wiim_ip_addr      = prefs.getString("wiim_ip", WIIM_IP_DEFAULT);
  prefs.end();
  _last_activity_ms = millis();

  // WiFi: offline-aware boot. Try saved creds, then fallback, then just
  // continue offline -- status_task keeps reconnecting in the background
  // and the offline screen shows the state. Portal is manual (settings).
  String s_ssid, s_pass;
  bool ok = false;
  if (portal_load_creds(s_ssid, s_pass))
    ok = wifi_connect(s_ssid.c_str(), s_pass.c_str(), 10000);
  if (!ok && strlen(WIFI_SSID_FALLBACK) > 0)
    ok = wifi_connect(WIFI_SSID_FALLBACK, WIFI_PASS_FALLBACK, 10000);
  if (!ok && !portal_load_creds(s_ssid, s_pass) && strlen(WIFI_SSID_FALLBACK) == 0)
    portal_run();   // factory-fresh device with zero credentials: auto-portal
  if (!ok)
    Serial.println("[wifi] offline boot; retrying in background");

  wiim_mutex = xSemaphoreCreateMutex();
  if (WiFi.status()==WL_CONNECTED) safe_poll_status();
  ui_request_refresh();

  knob_config_t cfg = { .gpio_encoder_a = ENCODER_ECA_PIN,
                        .gpio_encoder_b = ENCODER_ECB_PIN };
  s_knob = iot_knob_create(&cfg);
  iot_knob_register_cb(s_knob, KNOB_LEFT,  _knob_left_cb,  NULL);
  iot_knob_register_cb(s_knob, KNOB_RIGHT, _knob_right_cb, NULL);

  // All app/network tasks pinned to CORE 0. The LVGL render task is
  // pinned to core 1 (see lcd_bsp.c edit) so TLS crypto can never
  // steal CPU from the UI.
  xTaskCreatePinnedToCore(knob_task,    "knob_task",    12288, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(action_task,  "action_task",  12288, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(status_task,  "status_task",  12288, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(battery_task, "battery_task",  4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(sleep_task,   "sleep_task",    4096, NULL, 1, NULL, 0);


}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
