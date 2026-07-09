#pragma once
// =====================================================================
//  wiim_ui.h - LVGL v8 UI for the WiiM knob
//  Screens: MAIN -> SETTINGS MENU -> { WiFi Details | Sleep Timer }
//
//  Gestures:
//    main:      tap=play/pause  swipeL/R=next/prev  swipeDOWN=settings
//    anywhere in settings: swipe UP = back one level
//
//  Requires LV_FONT_MONTSERRAT_20 and _28 enabled in lv_conf.h.
//  DISCIPLINE: raw touch handler sets flags only; network stays in the
//  .ino tasks; widgets are written in LVGL context only.
// =====================================================================

#include "lvgl.h"
#include "wiim_client.h"
#include <WiFi.h>

// ---- fallback-hotspot (setup mode) state, set by the .ino ----
static volatile bool _ui_ap_active = false;

// preset chosen from the list (0 = none pending); drained by action_task
static volatile int _pending_preset = 0;
// last preset the user selected (highlighted in the list; persisted by .ino)
static volatile int  _ui_active_preset = 0;
static volatile bool _preset_sel_changed = false;

// ---- deferred actions (drained by status_task in the .ino) ----
enum WiimAction { WA_NONE = 0, WA_TOGGLE, WA_NEXT, WA_PREV, WA_MUTE, WA_START_PORTAL,
                  WA_OPEN_PRESETS, WA_RESTART, WA_RESET_WIFI, WA_FACTORY_RESET };
static volatile WiimAction _pending_action = WA_NONE;

// ---- which screen is showing (kept in sync by the nav helpers) ----
enum UiScreen { SCR_MAIN = 0, SCR_MENU, SCR_WIFI, SCR_SLEEP, SCR_OFFLINE, SCR_PRESET,
                SCR_SYSTEM, SCR_ABOUT };
static volatile UiScreen _ui_screen = SCR_MAIN;

// ---- activity / sleep state ----
static volatile uint32_t _last_activity_ms = 0;
static volatile bool     _screen_asleep    = false;

// ---- data pushed from the .ino ----
static volatile int  _ui_batt_pct      = -1;
static volatile bool _ui_batt_charging = false;
static char          _ui_net_ssid[33]  = "";
static char          _ui_net_ip[16]    = "";
static volatile int  _ui_net_rssi      = 0;

// ---- sleep-timer selection ----
static volatile int  _ui_sleep_minutes  = 0;
static volatile bool _sleep_sel_changed = false;


// ---- screens ----
static lv_obj_t *scr_main  = nullptr;
static lv_obj_t *scr_menu  = nullptr;
static lv_obj_t *scr_wifi  = nullptr;
static lv_obj_t *scr_sleep = nullptr;
static lv_obj_t *scr_off   = nullptr;
static lv_obj_t *scr_pre   = nullptr;
static lv_obj_t *pre_list  = nullptr;
static lv_obj_t *scr_sys   = nullptr;
static lv_obj_t *sys_reset_lbl = nullptr;   // factory-reset confirm label
static lv_obj_t *scr_about = nullptr;
static lv_obj_t *ab_l1=nullptr,*ab_l2=nullptr,*ab_l3=nullptr,*ab_l4=nullptr,*ab_l5=nullptr;
static lv_obj_t *off_hdr = nullptr;
static lv_obj_t *off_l1 = nullptr, *off_l2 = nullptr, *off_l3 = nullptr;

// ---- main-screen widgets ----
static lv_obj_t *ui_arc_vol   = nullptr;
static lv_obj_t *ui_lbl_vol   = nullptr;
static lv_obj_t *ui_lbl_title = nullptr;
static lv_obj_t *ui_lbl_artist= nullptr;
static lv_obj_t *ui_lbl_state = nullptr;
static lv_obj_t *ui_lbl_wifi  = nullptr;
static lv_obj_t *ui_lbl_batt  = nullptr;
static lv_obj_t *ui_btn_mute  = nullptr;
static lv_obj_t *ui_lbl_mute  = nullptr;

// ---- wifi-details widgets ----
static lv_obj_t *st_ssid = nullptr, *st_ip = nullptr, *st_rssi = nullptr,
                *st_wiim = nullptr, *st_batt = nullptr;

// ---- sleep-page widgets ----
static lv_obj_t *st_sleep_lbl = nullptr;

// ---- menu-page widget (portal status hint) ----
static lv_obj_t *st_portal_lbl = nullptr;

#define SWIPE_MIN_PX  40

static lv_coord_t _press_x = 0, _press_y = 0;
static bool       _press_valid = false;

// -------- navigation helpers: LVGL CONTEXT ONLY ----------------------
static void wiim_ui_refresh();   // fwd decl: nav refreshes the new page

static volatile uint32_t _preset_open_ms = 0;   // grace period after opening

static void wiim_ui_nav(UiScreen s) {
  Serial.printf("[ui] nav %d -> %d\n", (int)_ui_screen, (int)s);
  if (s == SCR_PRESET) _preset_open_ms = millis();
  lv_obj_t *t = scr_main;
  switch (s) {
    case SCR_MENU:  t = scr_menu;  break;
    case SCR_WIFI:  t = scr_wifi;  break;
    case SCR_SLEEP: t = scr_sleep; break;
    case SCR_OFFLINE: t = scr_off;  break;
    case SCR_PRESET:  t = scr_pre;  break;
    case SCR_SYSTEM:  t = scr_sys;  break;
    case SCR_ABOUT:   t = scr_about; break;
    default:        t = scr_main;  break;
  }
  if (t) { lv_scr_load(t); _ui_screen = s; wiim_ui_refresh(); }
}
static void wiim_ui_nav_back() {
  switch (_ui_screen) {
    case SCR_WIFI:
    case SCR_SLEEP:
    case SCR_SYSTEM:
    case SCR_ABOUT: wiim_ui_nav(SCR_MENU); break;
    case SCR_PRESET: wiim_ui_nav(SCR_MAIN); break;
    case SCR_MENU:  wiim_ui_nav(SCR_MAIN); break;
    default: break;
  }
}

// -------- raw touch handler: flags + timestamps ONLY ------------------
static void _on_screen_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;

  lv_point_t p;
  lv_indev_get_point(indev, &p);

  if (code == LV_EVENT_PRESSED) {
    _last_activity_ms = millis();
    _press_x = p.x; _press_y = p.y; _press_valid = true;
  }
  else if (code == LV_EVENT_LONG_PRESSED) {
    _press_valid = false;                 // swallow the upcoming release
    if (!_screen_asleep && _ui_screen == SCR_MAIN)
      _pending_action = WA_OPEN_PRESETS;  // fetch + open handled by action_task
  }
  else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (!_press_valid) return;
    _press_valid = false;
    _last_activity_ms = millis();
    if (_screen_asleep) return;               // waking touch: swallow

    lv_coord_t dx = p.x - _press_x;
    lv_coord_t dy = p.y - _press_y;
    lv_coord_t adx = dx < 0 ? -dx : dx;
    lv_coord_t ady = dy < 0 ? -dy : dy;

    bool on_main = (_ui_screen == SCR_MAIN);
    bool can_open_settings = (_ui_screen == SCR_MAIN || _ui_screen == SCR_OFFLINE);

    if (ady > SWIPE_MIN_PX && ady > adx) {
      // navigation is pure LVGL work -> do it right here, instantly
      if (dy > 0 && can_open_settings) wiim_ui_nav(SCR_MENU);   // down
      if (dy < 0 && !on_main && _ui_screen != SCR_OFFLINE) {
        if (_ui_screen == SCR_PRESET && millis() - _preset_open_ms < 500) return;
        wiim_ui_nav_back();  // up
      }
    }
    else if (on_main) {
      if (adx > SWIPE_MIN_PX && adx > ady) {
        _pending_action = (dx < 0) ? WA_NEXT : WA_PREV;
      } else if (adx < SWIPE_MIN_PX && ady < SWIPE_MIN_PX) {
        _pending_action = WA_TOGGLE;
      }
    }
  }
}

// -------- button callbacks (run in LVGL context: nav + light UI ok) --
static void _on_mute_btn(lv_event_t *e) { _pending_action = WA_MUTE; }
static void _on_menu_wifi (lv_event_t *e) { wiim_ui_nav(SCR_WIFI);  }
static void _on_menu_sleep(lv_event_t *e) { wiim_ui_nav(SCR_SLEEP); }
static void _on_menu_system(lv_event_t *e) { wiim_ui_nav(SCR_SYSTEM); }
static void _on_menu_about (lv_event_t *e) { wiim_ui_nav(SCR_ABOUT);  }

static void _on_sys_restart(lv_event_t *e) { _pending_action = WA_RESTART; }
static void _on_sys_resetwifi(lv_event_t *e) { _pending_action = WA_RESET_WIFI; }
// factory reset: two taps within 6s
static void _on_sys_factory(lv_event_t *e) {
  static uint32_t armed = 0;
  if (armed && millis() - armed < 6000) {
    _pending_action = WA_FACTORY_RESET;
  } else {
    armed = millis();
    if (sys_reset_lbl) lv_label_set_text(sys_reset_lbl, "TAP AGAIN TO CONFIRM");
  }
}
static void _on_menu_portal(lv_event_t *e) {
  _pending_action = WA_START_PORTAL;
  if (st_portal_lbl)
    lv_label_set_text(st_portal_lbl,
      "Hotspot 'WiiM-Knob-Setup' active\nOpen http://192.168.4.1");
}
static void _on_preset_btn(lv_event_t *e) {
  if (millis() - _preset_open_ms < 500) return;   // swallow phantom taps
  int n = (int)(intptr_t)lv_event_get_user_data(e);
  _pending_preset = n;           // action_task sends the command
  _ui_active_preset = n;         // highlight next time; persisted by .ino
  _preset_sel_changed = true;
  wiim_ui_nav(SCR_MAIN);         // optimistic: back to now-playing
}

static void _on_sleep_btn(lv_event_t *e) {
  int m = (int)(intptr_t)lv_event_get_user_data(e);
  _ui_sleep_minutes = m;
  _sleep_sel_changed = true;
  if (st_sleep_lbl) {
    if (m == 0) lv_label_set_text(st_sleep_lbl, "Screen sleep: off");
    else        lv_label_set_text_fmt(st_sleep_lbl, "Screen sleep: %d min", m);
  }
}

// -------- small builders ---------------------------------------------
static void _attach_gestures(lv_obj_t *scr) {
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, _on_screen_event, LV_EVENT_PRESSED,    NULL);
  lv_obj_add_event_cb(scr, _on_screen_event, LV_EVENT_RELEASED,   NULL);
  lv_obj_add_event_cb(scr, _on_screen_event, LV_EVENT_PRESS_LOST, NULL);
  lv_obj_add_event_cb(scr, _on_screen_event, LV_EVENT_LONG_PRESSED, NULL);
}

static lv_obj_t* _mk_header(lv_obj_t *parent, const char *txt) {
  lv_obj_t *h = lv_label_create(parent);
  lv_label_set_text(h, txt);
  lv_obj_set_style_text_color(h, lv_color_white(), 0);
  lv_obj_set_style_text_font(h, &lv_font_montserrat_20, 0);
  lv_obj_add_flag(h, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 34);
  return h;
}

static void _on_back_tap(lv_event_t *e) { wiim_ui_nav_back(); }

// "< TITLE" header like the stock knob menus: tappable chevron + title.
static void _mk_screen_header(lv_obj_t *parent, const char *title) {
  lv_obj_t *back = lv_btn_create(parent);
  lv_obj_set_size(back, 44, 44);
  lv_obj_align(back, LV_ALIGN_TOP_MID, -84, 22);
  lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_set_ext_click_area(back, 12);
  lv_obj_add_event_cb(back, _on_back_tap, LV_EVENT_CLICKED, NULL);
  lv_obj_t *ch = lv_label_create(back);
  lv_label_set_text(ch, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(ch, lv_color_white(), 0);
  lv_obj_center(ch);

  lv_obj_t *t = lv_label_create(parent);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 32);
}

// Left-aligned icon + text row, transparent (stock-menu style).
static lv_obj_t* _mk_item(lv_obj_t *parent, const char *icon,
                          const char *txt, int y, lv_event_cb_t cb,
                          void *user_data) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 250, 46);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x444444), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_radius(b, 8, 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);

  lv_obj_t *ic = lv_label_create(b);
  lv_label_set_text(ic, icon);
  lv_obj_set_style_text_color(ic, lv_color_hex(0xBBBBBB), 0);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);

  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 48, 0);
  return b;
}

static lv_obj_t* _mk_hint(lv_obj_t *parent, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -24);
  return l;
}

static lv_obj_t* _mk_status_lbl(lv_obj_t *parent, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, "");
  lv_obj_set_style_text_color(l, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}

static lv_obj_t* _mk_menu_btn(lv_obj_t *parent, const char *txt, int y,
                              lv_event_cb_t cb, uint32_t color) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 220, 48);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_center(l);
  return b;
}

// -------- screens ------------------------------------------------------
static void _build_main() {
  scr_main = lv_scr_act();
  _attach_gestures(scr_main);

  ui_arc_vol = lv_arc_create(scr_main);
  lv_obj_set_size(ui_arc_vol, 352, 352);
  lv_obj_center(ui_arc_vol);
  lv_arc_set_rotation(ui_arc_vol, 135);
  lv_arc_set_bg_angles(ui_arc_vol, 0, 270);
  lv_arc_set_range(ui_arc_vol, 0, 100);
  lv_arc_set_value(ui_arc_vol, 0);
  lv_obj_remove_style(ui_arc_vol, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(ui_arc_vol, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(ui_arc_vol, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_style_arc_width(ui_arc_vol, 15, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ui_arc_vol, 15, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ui_arc_vol, lv_color_hex(0x222222), LV_PART_MAIN);
  lv_obj_set_style_arc_color(ui_arc_vol, lv_color_hex(0x1DB954), LV_PART_INDICATOR);

  ui_lbl_wifi = lv_label_create(scr_main);
  lv_label_set_text(ui_lbl_wifi, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(ui_lbl_wifi, lv_color_hex(0x555555), 0);
  lv_obj_add_flag(ui_lbl_wifi, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(ui_lbl_wifi, LV_ALIGN_TOP_MID, -40, 38);

  ui_lbl_batt = lv_label_create(scr_main);
  lv_label_set_text(ui_lbl_batt, LV_SYMBOL_BATTERY_EMPTY " --");
  lv_obj_set_style_text_color(ui_lbl_batt, lv_color_hex(0x999999), 0);
  lv_obj_add_flag(ui_lbl_batt, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(ui_lbl_batt, LV_ALIGN_TOP_MID, 40, 38);

  ui_lbl_title = lv_label_create(scr_main);
  lv_label_set_long_mode(ui_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_anim_speed(ui_lbl_title, 15, 0);
  lv_obj_set_width(ui_lbl_title, 300);
  lv_obj_set_style_text_align(ui_lbl_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(ui_lbl_title, "\xE2\x80\x94");
  lv_obj_set_style_text_color(ui_lbl_title, lv_color_white(), 0);
  lv_obj_set_style_text_font(ui_lbl_title, &lv_font_montserrat_28, 0);
  lv_obj_add_flag(ui_lbl_title, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(ui_lbl_title, LV_ALIGN_CENTER, 0, -36);

  ui_lbl_artist = lv_label_create(scr_main);
  lv_label_set_long_mode(ui_lbl_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_anim_speed(ui_lbl_artist, 15, 0);
  lv_obj_set_width(ui_lbl_artist, 260);
  lv_obj_set_style_text_align(ui_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(ui_lbl_artist, "");
  lv_obj_set_style_text_color(ui_lbl_artist, lv_color_hex(0x999999), 0);
  lv_obj_set_style_text_font(ui_lbl_artist, &lv_font_montserrat_20, 0);
  lv_obj_add_flag(ui_lbl_artist, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(ui_lbl_artist, LV_ALIGN_CENTER, 0, 6);

  ui_lbl_state = lv_label_create(scr_main);
  lv_label_set_text(ui_lbl_state, LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(ui_lbl_state, lv_color_white(), 0);
  lv_obj_set_style_text_font(ui_lbl_state, &lv_font_montserrat_20, 0);
  lv_obj_set_style_opa(ui_lbl_state, LV_OPA_50, 0);
  lv_obj_add_flag(ui_lbl_state, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(ui_lbl_state, LV_ALIGN_CENTER, 0, 58);

  // mute button, bottom center (clickable: does NOT bubble to screen taps)
  ui_btn_mute = lv_btn_create(scr_main);
  lv_obj_set_size(ui_btn_mute, 72, 72);
  lv_obj_align(ui_btn_mute, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_set_style_radius(ui_btn_mute, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ui_btn_mute, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_ext_click_area(ui_btn_mute, 22);   // widen the touch zone
  lv_obj_add_event_cb(ui_btn_mute, _on_mute_btn, LV_EVENT_CLICKED, NULL);
  ui_lbl_mute = lv_label_create(ui_btn_mute);
  lv_label_set_text(ui_lbl_mute, LV_SYMBOL_VOLUME_MAX);
  lv_obj_set_style_text_color(ui_lbl_mute, lv_color_hex(0x999999), 0);
  lv_obj_center(ui_lbl_mute);

  // volume number, top center between the wifi and battery icons
  ui_lbl_vol = lv_label_create(scr_main);
  lv_label_set_text(ui_lbl_vol, "0");
  lv_obj_set_style_text_color(ui_lbl_vol, lv_color_hex(0x1DB954), 0);
  lv_obj_set_style_text_font(ui_lbl_vol, &lv_font_montserrat_20, 0);
  lv_obj_add_flag(ui_lbl_vol, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(ui_lbl_vol, LV_ALIGN_TOP_MID, 0, 66);
}

static void _build_menu() {
  scr_menu = lv_obj_create(NULL);
  _attach_gestures(scr_menu);
  _mk_screen_header(scr_menu, "SETTINGS");

  _mk_item(scr_menu, LV_SYMBOL_WIFI,     "WIFI DETAILS", 76,  _on_menu_wifi,   NULL);
  _mk_item(scr_menu, LV_SYMBOL_POWER,    "SLEEP TIMER",  124, _on_menu_sleep,  NULL);
  _mk_item(scr_menu, LV_SYMBOL_SETTINGS, "WIFI SETUP",   172, _on_menu_portal, NULL);
  _mk_item(scr_menu, LV_SYMBOL_REFRESH,  "SYSTEM",       220, _on_menu_system, NULL);
  _mk_item(scr_menu, LV_SYMBOL_LIST,     "ABOUT",        268, _on_menu_about,  NULL);

  st_portal_lbl = _mk_hint(scr_menu, "");
}

static void _build_wifi() {
  scr_wifi = lv_obj_create(NULL);
  _attach_gestures(scr_wifi);
  _mk_screen_header(scr_wifi, "WIFI DETAILS");

  st_ssid = _mk_status_lbl(scr_wifi, 100);
  st_ip   = _mk_status_lbl(scr_wifi, 128);
  st_rssi = _mk_status_lbl(scr_wifi, 156);
  st_wiim = _mk_status_lbl(scr_wifi, 184);
  st_batt = _mk_status_lbl(scr_wifi, 212);

  _mk_hint(scr_wifi, "swipe up to go back");
}

static void _build_sleep() {
  scr_sleep = lv_obj_create(NULL);
  _attach_gestures(scr_sleep);
  _mk_screen_header(scr_sleep, "SLEEP TIMER");

  st_sleep_lbl = lv_label_create(scr_sleep);
  lv_label_set_text(st_sleep_lbl, "Screen sleep: off");
  lv_obj_set_style_text_color(st_sleep_lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(st_sleep_lbl, &lv_font_montserrat_14, 0);
  lv_obj_add_flag(st_sleep_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(st_sleep_lbl, LV_ALIGN_TOP_MID, 0, 108);

  static const int mins[4] = {0, 1, 5, 10};
  static const char *labels[4] = {"Off", "1m", "5m", "10m"};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *b = lv_btn_create(scr_sleep);
    lv_obj_set_size(b, 62, 44);
    lv_obj_align(b, LV_ALIGN_TOP_MID, -102 + i*68, 150);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x222222), 0);
    lv_obj_add_event_cb(b, _on_sleep_btn, LV_EVENT_CLICKED,
                        (void*)(intptr_t)mins[i]);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, labels[i]);
    lv_obj_center(bl);
  }

  lv_obj_t *note = lv_label_create(scr_sleep);
  lv_label_set_text(note, "Screen turns off after idle.\nTouch or turn knob to wake.");
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(note, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
  lv_obj_add_flag(note, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 216);

  _mk_hint(scr_sleep, "swipe up to go back");
}

static void _build_offline() {
  scr_off = lv_obj_create(NULL);
  _attach_gestures(scr_off);

  lv_obj_t *icon = lv_label_create(scr_off);
  lv_label_set_text(icon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(icon, lv_color_hex(0xCC7733), 0);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
  lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 62);

  off_hdr = lv_label_create(scr_off);
  lv_label_set_text(off_hdr, "Waiting for connection");
  lv_obj_set_style_text_color(off_hdr, lv_color_white(), 0);
  lv_obj_set_style_text_font(off_hdr, &lv_font_montserrat_20, 0);
  lv_obj_add_flag(off_hdr, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(off_hdr, LV_ALIGN_TOP_MID, 0, 108);

  off_l1 = _mk_status_lbl(scr_off, 152);
  off_l2 = _mk_status_lbl(scr_off, 178);
  off_l3 = _mk_status_lbl(scr_off, 204);

  lv_obj_t *hint = _mk_hint(scr_off, "swipe down for settings");
  (void)hint;
}

static void _build_presets() {
  scr_pre = lv_obj_create(NULL);
  _attach_gestures(scr_pre);
  _mk_screen_header(scr_pre, "PRESETS");

  pre_list = lv_obj_create(scr_pre);
  lv_obj_set_size(pre_list, 236, 252);
  lv_obj_align(pre_list, LV_ALIGN_TOP_MID, 0, 64);
  lv_obj_set_scrollbar_mode(pre_list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_opa(pre_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pre_list, 0, 0);
  lv_obj_set_flex_flow(pre_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(pre_list, 8, 0);
  lv_obj_set_scroll_dir(pre_list, LV_DIR_VER);

  _mk_hint(scr_pre, "swipe up (edges) to go back");
}

// Rebuild the preset list from wiim_presets[]. LVGL CONTEXT ONLY.
static void wiim_ui_presets_rebuild() {
  if (!pre_list) return;
  lv_obj_clean(pre_list);
  if (wiim_preset_count == 0) {
    lv_obj_t *l = lv_label_create(pre_list);
    lv_label_set_text(l, "No presets found.\nAdd them in the WiiM Home app.");
    lv_obj_set_style_text_color(l, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    return;
  }
  for (int i = 0; i < wiim_preset_count; i++) {
    bool sel = (wiim_presets[i].number == _ui_active_preset);
    lv_obj_t *b = lv_btn_create(pre_list);
    lv_obj_set_size(b, 230, sel ? 58 : 48);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x444444), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_add_event_cb(b, _on_preset_btn, LV_EVENT_CLICKED,
                        (void*)(intptr_t)wiim_presets[i].number);
    lv_obj_t *ic = lv_label_create(b);
    lv_label_set_text(ic, sel ? LV_SYMBOL_PLAY : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(ic,
      sel ? lv_color_hex(0x1DB954) : lv_color_hex(0x666666), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, wiim_presets[i].name);
    lv_label_set_long_mode(bl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(bl, 158);
    lv_obj_set_style_text_color(bl,
      sel ? lv_color_white() : lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(bl,
      sel ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 40, 0);
  }
}

static void _build_system() {
  scr_sys = lv_obj_create(NULL);
  _attach_gestures(scr_sys);
  _mk_screen_header(scr_sys, "SYSTEM");

  _mk_item(scr_sys, LV_SYMBOL_REFRESH, "RESTART",     92,  _on_sys_restart,   NULL);
  _mk_item(scr_sys, LV_SYMBOL_WIFI,    "RESET WIFI",  146, _on_sys_resetwifi, NULL);
  _mk_item(scr_sys, LV_SYMBOL_TRASH,   "FACTORY RESET",200,_on_sys_factory,   NULL);

  sys_reset_lbl = lv_label_create(scr_sys);
  lv_label_set_text(sys_reset_lbl, "");
  lv_obj_set_style_text_color(sys_reset_lbl, lv_color_hex(0xCC3333), 0);
  lv_obj_set_style_text_font(sys_reset_lbl, &lv_font_montserrat_14, 0);
  lv_obj_add_flag(sys_reset_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(sys_reset_lbl, LV_ALIGN_TOP_MID, 0, 252);

  _mk_hint(scr_sys, "reset wifi clears saved networks");
}

static void _build_about() {
  scr_about = lv_obj_create(NULL);
  _attach_gestures(scr_about);
  _mk_screen_header(scr_about, "ABOUT");

  lv_obj_t *nm = lv_label_create(scr_about);
  lv_label_set_text(nm, "WiiM Knob  v1.0");
  lv_obj_set_style_text_color(nm, lv_color_white(), 0);
  lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
  lv_obj_add_flag(nm, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 84);

  ab_l1 = _mk_status_lbl(scr_about, 116);
  ab_l2 = _mk_status_lbl(scr_about, 142);
  ab_l3 = _mk_status_lbl(scr_about, 168);
  ab_l4 = _mk_status_lbl(scr_about, 194);
  ab_l5 = _mk_status_lbl(scr_about, 220);

  lv_obj_t *bd = lv_label_create(scr_about);
  lv_label_set_text(bd, "build " __DATE__);
  lv_obj_set_style_text_color(bd, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_font(bd, &lv_font_montserrat_14, 0);
  lv_obj_add_flag(bd, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(bd, LV_ALIGN_TOP_MID, 0, 250);
}

static void wiim_ui_build() {
  _build_main();
  _build_menu();
  _build_wifi();
  _build_sleep();
  _build_offline();
  _build_presets();
  _build_system();
  _build_about();
}

static const char* _batt_sym(int pct) {
  if (pct >= 88) return LV_SYMBOL_BATTERY_FULL;
  if (pct >= 63) return LV_SYMBOL_BATTERY_3;
  if (pct >= 38) return LV_SYMBOL_BATTERY_2;
  if (pct >= 13) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

static void wiim_ui_refresh() {
  // ---- change detection caches: only touch widgets when values change ----
  static int  c_vol = -1;
  static bool c_playing = false, c_online = false, c_first = true;
  static char c_title[192] = "", c_artist[192] = "";
  static int  c_pct = -2; static bool c_chg = false;

  if (ui_arc_vol && (c_first || wiim.volume != c_vol)) {
    lv_arc_set_value(ui_arc_vol, wiim.volume);
    lv_label_set_text_fmt(ui_lbl_vol, "%d", wiim.volume);
    c_vol = wiim.volume;
  }

  const char *t = wiim.title.length() ? wiim.title.c_str() : "\xE2\x80\x94";
  if (ui_lbl_title && (c_first || strncmp(t, c_title, sizeof(c_title)-1) != 0)) {
    lv_label_set_text(ui_lbl_title, t);
    strncpy(c_title, t, sizeof(c_title)-1); c_title[sizeof(c_title)-1]=0;
  }
  if (ui_lbl_artist && (c_first || strncmp(wiim.artist.c_str(), c_artist, sizeof(c_artist)-1) != 0)) {
    lv_label_set_text(ui_lbl_artist, wiim.artist.c_str());
    strncpy(c_artist, wiim.artist.c_str(), sizeof(c_artist)-1); c_artist[sizeof(c_artist)-1]=0;
  }

  if (ui_lbl_state && (c_first || wiim.playing != c_playing)) {
    lv_label_set_text(ui_lbl_state, wiim.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    c_playing = wiim.playing;
  }
  static bool c_muted = false;
  if (ui_lbl_mute && (c_first || wiim.muted != c_muted)) {
    lv_label_set_text(ui_lbl_mute, wiim.muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(ui_lbl_mute,
      wiim.muted ? lv_color_hex(0xCC3333) : lv_color_hex(0x999999), 0);
    if (ui_lbl_vol)
      lv_obj_set_style_text_color(ui_lbl_vol,
        wiim.muted ? lv_color_hex(0x555555) : lv_color_hex(0x1DB954), 0);
    c_muted = wiim.muted;
  }

  if (ui_lbl_wifi && (c_first || wiim.online != c_online)) {
    lv_obj_set_style_text_color(ui_lbl_wifi,
      wiim.online ? lv_color_hex(0x1DB954) : lv_color_hex(0x552222), 0);
    c_online = wiim.online;
  }

  if (ui_lbl_batt && (c_first || _ui_batt_pct != c_pct || _ui_batt_charging != c_chg)) {
    int pct = _ui_batt_pct;
    if (_ui_batt_charging) {
      lv_label_set_text_fmt(ui_lbl_batt, LV_SYMBOL_CHARGE " %d%%", pct < 0 ? 100 : pct);
      lv_obj_set_style_text_color(ui_lbl_batt, lv_color_hex(0x1DB954), 0);
    } else if (pct >= 0) {
      lv_label_set_text_fmt(ui_lbl_batt, "%s %d%%", _batt_sym(pct), pct);
      lv_obj_set_style_text_color(ui_lbl_batt,
        pct <= 15 ? lv_color_hex(0xCC3333) : lv_color_hex(0x999999), 0);
    }
    c_pct = _ui_batt_pct; c_chg = _ui_batt_charging;
  }

  // page-specific labels (small, cheap; still fine to set each pass)
  if (_ui_screen == SCR_WIFI) {
    if (st_ssid) lv_label_set_text_fmt(st_ssid, "Network: %s", _ui_net_ssid[0] ? _ui_net_ssid : "-");
    if (st_ip)   lv_label_set_text_fmt(st_ip,   "IP: %s",      _ui_net_ip[0]   ? _ui_net_ip   : "-");
    if (st_rssi) lv_label_set_text_fmt(st_rssi, "Signal: %d dBm", _ui_net_rssi);
    if (st_wiim) lv_label_set_text_fmt(st_wiim, "WiiM: %s", wiim.online ? "online" : "offline");
    if (st_batt) lv_label_set_text_fmt(st_batt, "Battery: %d%%", _ui_batt_pct < 0 ? 0 : _ui_batt_pct);
  }
  if (_ui_screen == SCR_OFFLINE && off_l1) {
    bool wl = (_ui_net_ip[0] != 0) && wiim.online;  // unused guard
    (void)wl;
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);
    if (wifi_ok) {
      if (off_hdr) lv_label_set_text(off_hdr, "WiiM not found");
      lv_label_set_text_fmt(off_l1, "WiFi: %s (%s)", _ui_net_ssid, _ui_net_ip);
      lv_label_set_text_fmt(off_l2, "WiiM: no reply from %s", wiim_ip_addr.c_str());
      lv_label_set_text(off_l3, "Setup: http://wiim-knob.local");
    } else if (_ui_ap_active) {
      if (off_hdr) lv_label_set_text(off_hdr, "WiFi Setup");
      lv_label_set_text(off_l1, "1. Join WiFi: WiiM-Knob-Setup");
      lv_label_set_text(off_l2, "2. Open http://192.168.4.1");
      lv_label_set_text(off_l3, "3. Choose your network + save");
    } else {
      if (off_hdr) lv_label_set_text(off_hdr, "Waiting for connection");
      lv_label_set_text(off_l1, "Searching for saved network...");
      lv_label_set_text(off_l2, "Setup hotspot starts shortly");
      lv_label_set_text(off_l3, "");
    }
  }

  if (_ui_screen == SCR_ABOUT && ab_l1) {
    lv_label_set_text_fmt(ab_l1, "IP: %s", _ui_net_ip[0] ? _ui_net_ip : "-");
    lv_label_set_text_fmt(ab_l2, "MAC: %s", WiFi.macAddress().c_str());
    lv_label_set_text_fmt(ab_l3, "WiiM: %s", wiim_ip_addr.c_str());
    lv_label_set_text_fmt(ab_l4, "Free RAM: %u KB", (unsigned)(esp_get_free_heap_size()/1024));
    lv_label_set_text_fmt(ab_l5, "Uptime: %lu min", (unsigned long)(millis()/60000));
  }

  if (_ui_screen == SCR_SLEEP && st_sleep_lbl) {
    int m = _ui_sleep_minutes;
    if (m == 0) lv_label_set_text(st_sleep_lbl, "Screen sleep: off");
    else        lv_label_set_text_fmt(st_sleep_lbl, "Screen sleep: %d min", m);
  }

  c_first = false;
}

static void wiim_ui_set_volume(int v) {
  static int last = -1;
  if (v == last) return;
  last = v;
  if (ui_arc_vol) lv_arc_set_value(ui_arc_vol, v);
  if (ui_lbl_vol) lv_label_set_text_fmt(ui_lbl_vol, "%d", v);
}
