#pragma once
// =====================================================================
//  wiim_battery.h - battery voltage -> percent for the Waveshare
//  ESP32-S3-Knob-Touch-LCD-1.8.
//
//  Based on Waveshare's own adc_bsp demo:
//    - ADC1 channel 0 (GPIO1), 12dB attenuation, 12-bit
//    - curve-fitting calibration
//    - battery is behind a 1:2 divider  -> Vbat = cal_mV * 0.001 * 2
//
//  Usage:
//    battery_init();                 // once, in setup()
//    battery_sample();               // call periodically (e.g. every 5s)
//    battery_percent()  -> 0..100
//    battery_voltage()  -> volts (smoothed)
//    battery_charging() -> heuristic: on USB/charging
// =====================================================================

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static adc_oneshot_unit_handle_t _bat_adc  = nullptr;
static adc_cali_handle_t         _bat_cali = nullptr;
static float _bat_v_smooth = 0.0f;   // exponentially smoothed voltage
static int   _bat_pct      = -1;     // -1 = unknown yet

static void battery_init() {
  adc_cali_curve_fitting_config_t cali_config = {
    .unit_id  = ADC_UNIT_1,
    .atten    = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  adc_cali_create_scheme_curve_fitting(&cali_config, &_bat_cali);

  adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
  adc_oneshot_new_unit(&init_config1, &_bat_adc);

  adc_oneshot_chan_cfg_t config = {
    .atten    = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  adc_oneshot_config_channel(_bat_adc, ADC_CHANNEL_0, &config);
}

// LiPo open-circuit voltage -> percent (piecewise linear approximation)
static int _lipo_v_to_pct(float v) {
  struct P { float v; int p; };
  static const P curve[] = {
    {4.20f,100},{4.10f, 90},{4.00f, 78},{3.90f, 62},
    {3.80f, 45},{3.70f, 28},{3.60f, 14},{3.50f,  6},
    {3.40f,  2},{3.30f,  0},
  };
  if (v >= curve[0].v) return 100;
  const int n = sizeof(curve)/sizeof(curve[0]);
  if (v <= curve[n-1].v) return 0;
  for (int i = 1; i < n; i++) {
    if (v > curve[i].v) {
      float span = curve[i-1].v - curve[i].v;
      float frac = (v - curve[i].v) / span;
      return curve[i].p + (int)(frac * (curve[i-1].p - curve[i].p) + 0.5f);
    }
  }
  return 0;
}

// Read once and update the smoothed value + percent.
static void battery_sample() {
  if (!_bat_adc) return;
  int raw = 0;
  if (adc_oneshot_read(_bat_adc, ADC_CHANNEL_0, &raw) != ESP_OK) return;

  float v;
  int mv = 0;
  if (_bat_cali && adc_cali_raw_to_voltage(_bat_cali, raw, &mv) == ESP_OK) {
    v = 0.001f * mv * 2.0f;              // Waveshare's exact formula
  } else {
    v = ((float)raw * 3.3f / 4096.0f) * 2.0f;
  }

  if (_bat_v_smooth <= 0.1f) _bat_v_smooth = v;       // first sample
  else _bat_v_smooth = 0.8f * _bat_v_smooth + 0.2f * v; // smooth

  _bat_pct = _lipo_v_to_pct(_bat_v_smooth);
}

static float battery_voltage()  { return _bat_v_smooth; }
static int   battery_percent()  { return _bat_pct; }

// Heuristic: a LiPo only sits above ~4.25V while being charged / on USB.
// (With no battery connected, the rail also reads high -> shows as "charging",
//  which is the desired glyph for "external power" anyway.)
static bool battery_charging()  { return _bat_v_smooth > 4.25f; }
