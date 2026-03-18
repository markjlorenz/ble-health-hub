#include "pulseox_demo_lvgl.h"

#include <lvgl.h>

#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#endif

#include <cmath>
#include <cstdio>

namespace {

static TFT_eSPI* s_tft = nullptr;
static bool s_inited = false;
static bool s_active = false;
static bool s_swap_bytes = true;
static bool s_swap_red_blue = false;

static int s_panel_w = 0;
static int s_panel_h = 0;
static int s_panel_x_off = 0;
static int s_panel_y_off = 0;
static int s_ui_h = 0;

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_color_t* s_buf1 = nullptr;
static lv_color_t* s_buf2 = nullptr;

static uint32_t s_last_pump_ms = 0;
static float s_peak_sig = 0.0f;
static uint32_t s_peak_ms = 0;

static float s_demo_spo2 = 0.0f;
static float s_demo_hr = 0.0f;
static float s_demo_pi = 0.0f;

static bool s_live_mode = false;
static float s_live_spo2 = 0.0f;
static float s_live_hr = 0.0f;
static float s_live_pi = 0.0f;
static float s_display_spo2 = 0.0f;
static float s_display_hr = 0.0f;
static float s_display_pi = 0.0f;
static int s_live_signal_level = 0;

static constexpr int kBottomStatusH = 12;
static constexpr int kGaugeCount = 3;
static constexpr float kGaugeStartDeg = 140.0f;
static constexpr float kGaugeSweepDeg = 260.0f;
static constexpr uint32_t kGaugeAlertPulseMs = 240;
static constexpr int kGaugeAlertPulseCount = 3;

struct GaugeSpec {
  const char* label;
  float min_value;
  float max_value;
  float normal_min;
  float normal_max;
  int decimals;
  uint32_t accent_rgb;
  uint32_t accent_dim_rgb;
};

static const GaugeSpec kGaugeSpecs[kGaugeCount] = {
    {"SpO2", 70.0f, 100.0f, 94.0f, 100.0f, 0, 0x4DE8FFu, 0x123E47u},
    {"HR", 40.0f, 160.0f, 60.0f, 125.0f, 0, 0x95FF5Au, 0x233F19u},
    {"PI", 0.0f, 22.0f, 0.2f, 20.0f, 1, 0xFFC04Du, 0x4A3112u},
};

struct GaugeUi {
  lv_obj_t* card = nullptr;
  lv_obj_t* canvas = nullptr;
  lv_color_t* canvas_buf = nullptr;
  lv_obj_t* label = nullptr;
  lv_obj_t* value = nullptr;
  lv_obj_t* status = nullptr;
};

static GaugeUi s_gauges[kGaugeCount];
static bool s_gauge_out_of_range[kGaugeCount] = {false, false, false};
static uint32_t s_gauge_alert_start_ms[kGaugeCount] = {0, 0, 0};
static int s_gauge_canvas_size = 0;

static lv_obj_t* s_signal_panel = nullptr;
static lv_obj_t* s_signal_label = nullptr;
static lv_obj_t* s_signal_frame = nullptr;
static lv_obj_t* s_signal_bars[8] = {nullptr};
static lv_obj_t* s_signal_peak_marker = nullptr;
static int s_signal_inner_x = 0;
static int s_signal_inner_y = 0;
static int s_signal_bar_w = 0;
static int s_signal_bar_h = 0;
static int s_signal_bar_gap = 0;
static int s_signal_inner_w = 0;

static lv_color_t make_rgb(uint8_t r, uint8_t g, uint8_t b) {
  if (s_swap_red_blue) {
    const uint8_t tmp = r;
    r = b;
    b = tmp;
  }
  return lv_color_make(r, g, b);
}

static lv_color_t make_rgb_u32(uint32_t rgb) {
  return make_rgb((uint8_t)((rgb >> 16) & 0xFF), (uint8_t)((rgb >> 8) & 0xFF), (uint8_t)(rgb & 0xFF));
}

static lv_color_t kScreenBg() { return make_rgb(0x04, 0x09, 0x0F); }
static lv_color_t kPanelBg() { return make_rgb(0x09, 0x14, 0x1D); }
static lv_color_t kPanelBorder() { return make_rgb(0x18, 0x28, 0x31); }
static lv_color_t kGaugeTrack() { return make_rgb(0x12, 0x1B, 0x24); }
static lv_color_t kGaugeInner() { return make_rgb(0x03, 0x07, 0x0C); }
static lv_color_t kGaugeAlertRed() { return make_rgb(0xD8, 0x1F, 0x2E); }
static lv_color_t kNeedleWhite() { return make_rgb(0xF8, 0xFD, 0xFF); }
static lv_color_t kNeedleBlack() { return make_rgb(0x00, 0x00, 0x00); }
static lv_color_t kTextDim() { return make_rgb(0x92, 0xA7, 0xB3); }
static lv_color_t kTextBright() { return make_rgb(0xF2, 0xFB, 0xFF); }
static lv_color_t kSignalOff() { return make_rgb(0x11, 0x19, 0x22); }
static lv_color_t kSignalOn() { return make_rgb(0x7B, 0xFF, 0x72); }
static lv_color_t kSignalPeak() { return make_rgb(0xF8, 0xFF, 0xA0); }

static float clamp01(float v) {
  if (v <= 0.0f) return 0.0f;
  if (v >= 1.0f) return 1.0f;
  return v;
}

static float normalize_value(float value, const GaugeSpec& spec) {
  const float span = spec.max_value - spec.min_value;
  if (span <= 0.0f) return 0.0f;
  return clamp01((value - spec.min_value) / span);
}

static float angle_delta_from_start(float angle_deg) {
  float delta = angle_deg - kGaugeStartDeg;
  while (delta < 0.0f) delta += 360.0f;
  while (delta >= 360.0f) delta -= 360.0f;
  return delta;
}

static void* alloc_draw_buf_bytes(size_t bytes) {
#if defined(MALLOC_CAP_SPIRAM)
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  return malloc(bytes);
}

static void compute_panel_abs_xy(int* out_x, int* out_y) {
  if (!s_tft) {
    *out_x = 0;
    *out_y = 0;
    return;
  }

  s_tft->setRotation(0);
  s_tft->resetViewport();
  const int canvas_w = (int)s_tft->width();
  const int canvas_h = (int)s_tft->height();
  const int x = max(0, min(s_panel_x_off, max(0, canvas_w - s_panel_w)));
  const int max_y = max(0, canvas_h - s_panel_h);
  const int y = max(0, min(s_panel_y_off, max_y));
  *out_x = x;
  *out_y = y;
}

static void flush_cb(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  (void)disp;
  if (!s_tft) {
    lv_disp_flush_ready(disp);
    return;
  }

  int ax = 0;
  int ay = 0;
  compute_panel_abs_xy(&ax, &ay);

  const int w = area->x2 - area->x1 + 1;
  const int h = area->y2 - area->y1 + 1;

  s_tft->setSwapBytes(s_swap_bytes);
  s_tft->startWrite();
  s_tft->pushImage(ax + area->x1, ay + area->y1, w, h, (uint16_t*)color_p);
  s_tft->endWrite();
  s_tft->setSwapBytes(false);

  lv_disp_flush_ready(disp);
}

static float beat_wave(float phase01) {
  const float x1 = (phase01 - 0.12f) / 0.045f;
  const float x2 = (phase01 - 0.36f) / 0.07f;
  float v = 0.10f;
  v += 0.90f * expf(-(x1 * x1));
  v += 0.20f * expf(-(x2 * x2));
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  return v;
}

static void paint_dot(lv_color_t* buf, int size, int cx, int cy, int radius, lv_color_t color) {
  if (!buf || size <= 0 || radius < 0) return;
  for (int dy = -radius; dy <= radius; dy++) {
    const int y = cy + dy;
    if (y < 0 || y >= size) continue;
    for (int dx = -radius; dx <= radius; dx++) {
      const int x = cx + dx;
      if (x < 0 || x >= size) continue;
      if ((dx * dx) + (dy * dy) > (radius * radius)) continue;
      buf[y * size + x] = color;
    }
  }
}

static float ease_toward(float current, float target, uint32_t dt_ms) {
  static constexpr float kTauMs = 220.0f;
  if (dt_ms == 0) return current;
  const float alpha = 1.0f - expf(-(float)dt_ms / kTauMs);
  return current + (target - current) * alpha;
}

static void ease_live_display_values(uint32_t dt_ms) {
  s_display_spo2 = ease_toward(s_display_spo2, s_live_spo2, dt_ms);
  s_display_hr = ease_toward(s_display_hr, s_live_hr, dt_ms);
  s_display_pi = ease_toward(s_display_pi, s_live_pi, dt_ms);
}

static void brighten_range_band(lv_color_t* buf, int size, float start_deg, float end_deg, float inner_r, float outer_r,
                                lv_color_t color) {
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      const float dx = (float)x - (float)(size / 2) + 0.5f;
      const float dy = (float)y - (float)(size / 2) + 0.5f;
      const float dist = sqrtf((dx * dx) + (dy * dy));
      if (dist < inner_r || dist > outer_r) continue;

      float angle_deg = atan2f(dy, dx) * 180.0f / 3.14159265f;
      if (angle_deg < 0.0f) angle_deg += 360.0f;
      const float delta = angle_delta_from_start(angle_deg);
      if (delta >= start_deg && delta <= end_deg) {
        buf[y * size + x] = color;
      }
    }
  }
}

static void draw_radial_tick(lv_color_t* buf, int size, float angle_deg, int inner_r, int outer_r, lv_color_t color) {
  const float rad = angle_deg * 3.14159265f / 180.0f;
  const float ux = cosf(rad);
  const float uy = sinf(rad);
  const int cx = size / 2;
  const int cy = size / 2;

  for (int radius = inner_r; radius <= outer_r; radius++) {
    const int x = (int)lroundf((float)cx + ux * (float)radius);
    const int y = (int)lroundf((float)cy + uy * (float)radius);
    paint_dot(buf, size, x, y, 1, color);
  }

  const int tip_x = (int)lroundf((float)cx + ux * (float)outer_r);
  const int tip_y = (int)lroundf((float)cy + uy * (float)outer_r);
  paint_dot(buf, size, tip_x, tip_y, 1, color);
}

static void update_gauge_status_ui(int gauge_idx, float value) {
  if (gauge_idx < 0 || gauge_idx >= kGaugeCount) return;
  GaugeUi& gauge = s_gauges[gauge_idx];
  if (!gauge.status) return;
  (void)value;
  lv_obj_add_flag(gauge.status, LV_OBJ_FLAG_HIDDEN);
}

static float gauge_alert_mix(int gauge_idx, uint32_t now_ms) {
  if (gauge_idx < 0 || gauge_idx >= kGaugeCount) return 0.0f;
  if (!s_gauge_out_of_range[gauge_idx]) return 0.0f;

  const uint32_t elapsed = now_ms - s_gauge_alert_start_ms[gauge_idx];
  const uint32_t total_pulse_ms = kGaugeAlertPulseMs * 2u * (uint32_t)kGaugeAlertPulseCount;
  if (elapsed >= total_pulse_ms) {
    return 1.0f;
  }

  const uint32_t pulse_period_ms = kGaugeAlertPulseMs * 2u;
  const float phase = (float)(elapsed % pulse_period_ms) / (float)kGaugeAlertPulseMs;
  if (phase <= 1.0f) {
    return phase;
  }
  return 2.0f - phase;
}

static void update_gauge_alert_state(int gauge_idx, float value, uint32_t now_ms) {
  if (gauge_idx < 0 || gauge_idx >= kGaugeCount) return;
  const GaugeSpec& spec = kGaugeSpecs[gauge_idx];
  const bool out_of_range = value < spec.normal_min || value > spec.normal_max;
  if (out_of_range && !s_gauge_out_of_range[gauge_idx]) {
    s_gauge_alert_start_ms[gauge_idx] = now_ms;
  }
  if (!out_of_range) {
    s_gauge_alert_start_ms[gauge_idx] = 0;
  }
  s_gauge_out_of_range[gauge_idx] = out_of_range;
}

static void draw_gauge_canvas(int gauge_idx, float value, uint32_t now_ms) {
  if (gauge_idx < 0 || gauge_idx >= kGaugeCount) return;
  GaugeUi& gauge = s_gauges[gauge_idx];
  if (!gauge.canvas || !gauge.canvas_buf || s_gauge_canvas_size <= 0) return;

  const GaugeSpec& spec = kGaugeSpecs[gauge_idx];
  const int size = s_gauge_canvas_size;
  const int cx = size / 2;
  const int cy = size / 2;
  const float outer_r = (float)size * 0.47f;
  const float ring_w = max(5.0f, (float)size * 0.10f);
  const float inner_r = outer_r - ring_w;
  const float core_r = inner_r - 4.0f;
  const float normal_start = normalize_value(spec.normal_min, spec) * kGaugeSweepDeg;
  const float normal_end = normalize_value(spec.normal_max, spec) * kGaugeSweepDeg;
  const float marker_norm = normalize_value(value, spec);
  const float marker_angle = kGaugeStartDeg + marker_norm * kGaugeSweepDeg;
  const float marker_delta = marker_norm * kGaugeSweepDeg;
  const bool marker_over_normal = marker_delta >= normal_start && marker_delta <= normal_end;

  const lv_color_t bg = kPanelBg();
  const lv_color_t track = kGaugeTrack();
  const lv_color_t normal = make_rgb_u32(spec.accent_rgb);
  const lv_color_t trail = lv_color_mix(normal, track, (lv_opa_t)89);
  const lv_opa_t mix = (lv_opa_t)lroundf(gauge_alert_mix(gauge_idx, now_ms) * 255.0f);
  const lv_color_t inner = lv_color_mix(kGaugeAlertRed(), kGaugeInner(), mix);

  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      const float dx = (float)x - (float)cx + 0.5f;
      const float dy = (float)y - (float)cy + 0.5f;
      const float dist = sqrtf((dx * dx) + (dy * dy));
      lv_color_t color = bg;

      if (dist <= core_r) {
        color = inner;
      } else if (dist <= outer_r && dist >= inner_r) {
        float angle_deg = atan2f(dy, dx) * 180.0f / 3.14159265f;
        if (angle_deg < 0.0f) angle_deg += 360.0f;
        const float delta = angle_delta_from_start(angle_deg);
        if (delta <= kGaugeSweepDeg) {
          color = track;
        }
      }

      gauge.canvas_buf[y * size + x] = color;
    }
  }

  brighten_range_band(gauge.canvas_buf, size, 0.0f, marker_delta, inner_r + 1.0f, outer_r - 1.0f, trail);
  brighten_range_band(gauge.canvas_buf, size, normal_start, normal_end, inner_r, outer_r, normal);
  draw_radial_tick(gauge.canvas_buf, size, marker_angle, (int)inner_r - 1, (int)outer_r,
                   marker_over_normal ? kNeedleBlack() : kNeedleWhite());
  const float marker_rad = marker_angle * 3.14159265f / 180.0f;
  const int tip_x = (int)lroundf((float)cx + cosf(marker_rad) * (outer_r - 1.5f));
  const int tip_y = (int)lroundf((float)cy + sinf(marker_rad) * (outer_r - 1.5f));
  paint_dot(gauge.canvas_buf, size, tip_x, tip_y, 2, marker_over_normal ? kNeedleBlack() : normal);
  lv_obj_invalidate(gauge.canvas);
}

static void update_gauge_labels(float spo2, float hr, float pi, uint32_t now_ms) {
  const float values[kGaugeCount] = {spo2, hr, pi};
  char buf[16];

  for (int i = 0; i < kGaugeCount; i++) {
    update_gauge_alert_state(i, values[i], now_ms);
    const GaugeSpec& spec = kGaugeSpecs[i];
    if (!s_gauges[i].value) continue;
    if (spec.decimals == 0) {
      snprintf(buf, sizeof(buf), "%.0f", values[i]);
    } else {
      snprintf(buf, sizeof(buf), "%.1f", values[i]);
    }
    lv_label_set_text(s_gauges[i].value, buf);
    lv_obj_align_to(s_gauges[i].value, s_gauges[i].canvas, LV_ALIGN_CENTER, 0, 0);
    update_gauge_status_ui(i, values[i]);
    draw_gauge_canvas(i, values[i], now_ms);
  }
}

static void set_segment_level(int level_0_to_8, float pulse_mix = 1.0f) {
  const int lvl = max(0, min(8, level_0_to_8));
  for (int i = 0; i < 8; i++) {
    if (!s_signal_bars[i]) continue;
    const bool on = i < lvl;
    lv_color_t on_color = lv_color_mix(kTextBright(), kSignalOn(), (lv_opa_t)lroundf(clamp01(pulse_mix) * 255.0f));
    lv_obj_set_style_bg_color(s_signal_bars[i], on ? on_color : kSignalOff(), 0);
    lv_obj_set_style_bg_opa(s_signal_bars[i], on ? LV_OPA_COVER : LV_OPA_70, 0);
    lv_obj_set_style_border_color(s_signal_bars[i], on ? kTextBright() : kPanelBorder(), 0);
  }
}

static void update_peak_gauge(float sig, uint32_t now_ms) {
  static constexpr uint32_t kHoldMs = 2000;
  static constexpr uint32_t kDecayMs = 1000;
  static uint32_t s_last_ms = 0;

  uint32_t dt_ms = 0;
  if (s_last_ms != 0 && now_ms >= s_last_ms) {
    dt_ms = now_ms - s_last_ms;
  }
  s_last_ms = now_ms;

  if (sig > s_peak_sig || now_ms < s_peak_ms) {
    s_peak_sig = sig;
    s_peak_ms = now_ms;
  } else {
    const uint32_t age = now_ms - s_peak_ms;
    if (age > kHoldMs && dt_ms > 0) {
      const float dec = (float)dt_ms / (float)kDecayMs;
      s_peak_sig = max(sig, max(0.0f, s_peak_sig - dec));
    }
  }

  if (!s_signal_peak_marker || s_signal_inner_w <= 0) return;
  const int travel = max(0, s_signal_inner_w - 2);
  const int x = s_signal_inner_x + (int)lroundf(clamp01(s_peak_sig) * (float)travel);
  const int marker_h = max(2, s_signal_bar_h - 2);
  const int marker_y = s_signal_inner_y + max(0, (s_signal_bar_h - marker_h) / 2);
  lv_obj_set_size(s_signal_peak_marker, 2, marker_h);
  lv_obj_set_pos(s_signal_peak_marker, x, marker_y);
}

static void update_demo_readings(uint32_t t_ms) {
  auto lerp = [](float a, float b, float t) -> float { return a + (b - a) * t; };
  auto smoothstep01 = [](float t) -> float {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
  };

  static constexpr uint32_t kStageMs = 5500;
  static constexpr uint32_t kRampMs = 900;
  const int stage = (int)((t_ms / kStageMs) % 3u);
  const uint32_t stage_t = (uint32_t)(t_ms % kStageMs);
  const int next_stage = (stage + 1) % 3;

  const float hr_base[3] = {72.0f, 128.0f, 110.0f};
  const float spo2_base[3] = {98.0f, 95.0f, 86.0f};
  const float pi_base[3] = {2.6f, 21.2f, 0.9f};

  float mix = 0.0f;
  if (stage_t > (kStageMs - kRampMs)) {
    mix = smoothstep01((float)(stage_t - (kStageMs - kRampMs)) / (float)kRampMs);
  }

  const float hr0 = hr_base[stage];
  const float hr1 = hr_base[next_stage];
  const float hr = lerp(hr0, hr1, mix) + 1.5f * sinf((float)t_ms / 5000.0f);

  const float period_ms = 60000.0f / max(40.0f, hr);
  const float phase = fmodf((float)t_ms, period_ms) / period_ms;
  const float sig = beat_wave(phase);
  const int seg = (int)lroundf(sig * 8.0f);

  const float spo20 = spo2_base[stage];
  const float spo21 = spo2_base[next_stage];
  const float spo2 = lerp(spo20, spo21, mix) + 0.6f * sinf((float)t_ms / 7000.0f);

  const float pi0 = pi_base[stage];
  const float pi1 = pi_base[next_stage];
  const float pi = lerp(pi0, pi1, mix) + 0.45f * (sig - 0.1f) + 0.10f * sinf((float)t_ms / 3000.0f);

  s_demo_spo2 = spo2;
  s_demo_hr = hr;
  s_demo_pi = pi;

  set_segment_level(seg, 0.35f + 0.65f * sig);
  update_peak_gauge(sig, t_ms);
  update_gauge_labels(spo2, hr, pi, t_ms);
}

static void update_live_readings(uint32_t now_ms, uint32_t dt_ms) {
  const int seg = max(0, min(8, s_live_signal_level));
  const float sig = (float)seg / 8.0f;
  ease_live_display_values(dt_ms);
  set_segment_level(seg, 0.55f + 0.45f * sig);
  update_peak_gauge(sig, now_ms);
  update_gauge_labels(s_display_spo2, s_display_hr, s_display_pi, now_ms);
}

static void style_panel(lv_obj_t* obj, lv_color_t border) {
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(obj, 8, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, border, 0);
  lv_obj_set_style_bg_color(obj, kPanelBg(), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(obj, 0, 0);
  lv_obj_set_style_outline_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
}

static void style_signal_panel(lv_obj_t* obj) {
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(obj, 6, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, kPanelBorder(), 0);
  lv_obj_set_style_bg_color(obj, kPanelBg(), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(obj, 0, 0);
  lv_obj_set_style_outline_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
}

static void create_ui() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, kScreenBg(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  static constexpr int kSignalPanelH = 30;
  static constexpr int kSignalOuterGap = 4;
  const int card_w = min(s_panel_w - 4, 72);
  const int card_h = 72;
  const int gauge_size = card_w - 6;
  const int signal_panel_y = s_ui_h - kSignalPanelH - kSignalOuterGap;
  const int gauge_area_h = max(0, signal_panel_y - 2);
  const int gauge_gap = max(4, (gauge_area_h - (card_h * kGaugeCount)) / (kGaugeCount + 1));
  const int start_y = gauge_gap;
  const int card_x = (s_panel_w - card_w) / 2;
  const int canvas_x = (card_w - gauge_size) / 2;
  const int canvas_y = 3;
  const int label_y = canvas_y + gauge_size - 12;

  s_gauge_canvas_size = gauge_size;

  for (int i = 0; i < kGaugeCount; i++) {
    GaugeUi& gauge = s_gauges[i];
    const int y = start_y + i * (card_h + gauge_gap);

    gauge.card = lv_obj_create(scr);
    lv_obj_set_pos(gauge.card, card_x, y);
    lv_obj_set_size(gauge.card, card_w, card_h);
    style_panel(gauge.card, make_rgb_u32(kGaugeSpecs[i].accent_dim_rgb));

    gauge.canvas = lv_canvas_create(gauge.card);
    lv_obj_set_pos(gauge.canvas, canvas_x, canvas_y);
    lv_obj_set_size(gauge.canvas, gauge_size, gauge_size);
    lv_obj_set_style_border_width(gauge.canvas, 0, 0);
    lv_obj_set_style_bg_opa(gauge.canvas, LV_OPA_TRANSP, 0);

    const size_t bytes = (size_t)gauge_size * (size_t)gauge_size * sizeof(lv_color_t);
    gauge.canvas_buf = (lv_color_t*)alloc_draw_buf_bytes(bytes);
    lv_canvas_set_buffer(gauge.canvas, gauge.canvas_buf, gauge_size, gauge_size, LV_IMG_CF_TRUE_COLOR);

    gauge.label = lv_label_create(gauge.card);
    lv_label_set_text(gauge.label, kGaugeSpecs[i].label);
    lv_obj_set_style_text_font(gauge.label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(gauge.label, kTextDim(), 0);
    lv_obj_set_width(gauge.label, gauge_size - 8);
    lv_obj_set_style_text_align(gauge.label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(gauge.label, LV_ALIGN_TOP_MID, 0, label_y);

    gauge.value = lv_label_create(gauge.card);
    lv_label_set_text(gauge.value, "0");
    lv_obj_set_style_text_font(gauge.value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(gauge.value, kTextBright(), 0);
    lv_obj_set_size(gauge.value, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(gauge.value, LV_ALIGN_CENTER, 0, 0);

    gauge.status = lv_label_create(gauge.card);
    lv_label_set_text(gauge.status, "OK");
    lv_obj_set_style_text_font(gauge.status, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(gauge.status, kNeedleBlack(), 0);
    lv_obj_set_style_bg_color(gauge.status, make_rgb_u32(kGaugeSpecs[i].accent_rgb), 0);
    lv_obj_set_style_bg_opa(gauge.status, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(gauge.status, 3, 0);
    lv_obj_set_style_pad_ver(gauge.status, 1, 0);
    lv_obj_set_style_radius(gauge.status, 3, 0);
    lv_obj_set_pos(gauge.status, card_w - 23, 5);
    lv_obj_add_flag(gauge.status, LV_OBJ_FLAG_HIDDEN);
  }

  s_signal_panel = lv_obj_create(scr);
  lv_obj_set_pos(s_signal_panel, card_x, signal_panel_y);
  lv_obj_set_size(s_signal_panel, card_w, kSignalPanelH);
  style_signal_panel(s_signal_panel);

  s_signal_label = lv_label_create(s_signal_panel);
  lv_label_set_text(s_signal_label, "PULSE");
  lv_obj_set_style_text_font(s_signal_label, &lv_font_unscii_8, 0);
  lv_obj_set_style_text_color(s_signal_label, kTextDim(), 0);
  lv_obj_align(s_signal_label, LV_ALIGN_BOTTOM_MID, 0, -1);

  const int signal_frame_x = 0;
  const int signal_frame_y = 2;
  const int signal_frame_w = card_w - (signal_frame_x * 2);
  const int signal_frame_h = 14;
  s_signal_frame = lv_obj_create(s_signal_panel);
  lv_obj_set_pos(s_signal_frame, signal_frame_x, signal_frame_y);
  lv_obj_set_size(s_signal_frame, signal_frame_w, signal_frame_h);
  lv_obj_clear_flag(s_signal_frame, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(s_signal_frame, 6, 0);
  lv_obj_set_style_border_width(s_signal_frame, 0, 0);
  lv_obj_set_style_bg_color(s_signal_frame, kPanelBg(), 0);
  lv_obj_set_style_bg_opa(s_signal_frame, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(s_signal_frame, 0, 0);
  lv_obj_set_style_shadow_width(s_signal_frame, 0, 0);
  lv_obj_set_style_outline_width(s_signal_frame, 0, 0);

  s_signal_bar_gap = 2;
  const int signal_inner_pad_x = 2;
  const int signal_inner_pad_y = 2;
  s_signal_inner_y = signal_inner_pad_y;
  s_signal_bar_h = signal_frame_h - (signal_inner_pad_y * 2);
  const int signal_inner_available_w = signal_frame_w - (signal_inner_pad_x * 2);
  s_signal_bar_w = max(4, (signal_inner_available_w - (7 * s_signal_bar_gap)) / 8);
  s_signal_inner_w = (8 * s_signal_bar_w) + (7 * s_signal_bar_gap);
  s_signal_inner_x = signal_inner_pad_x + max(0, (signal_inner_available_w - s_signal_inner_w) / 2);

  for (int i = 0; i < 8; i++) {
    s_signal_bars[i] = lv_obj_create(s_signal_frame);
    lv_obj_set_pos(s_signal_bars[i], s_signal_inner_x + i * (s_signal_bar_w + s_signal_bar_gap), s_signal_inner_y);
    lv_obj_set_size(s_signal_bars[i], s_signal_bar_w, s_signal_bar_h);
    lv_obj_clear_flag(s_signal_bars[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_signal_bars[i], 2, 0);
    lv_obj_set_style_border_width(s_signal_bars[i], 1, 0);
    lv_obj_set_style_shadow_width(s_signal_bars[i], 0, 0);
    lv_obj_set_style_outline_width(s_signal_bars[i], 0, 0);
  }

  s_signal_peak_marker = lv_obj_create(s_signal_frame);
  lv_obj_set_size(s_signal_peak_marker, 2, max(2, s_signal_bar_h - 2));
  lv_obj_clear_flag(s_signal_peak_marker, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(s_signal_peak_marker, 1, 0);
  lv_obj_set_style_border_width(s_signal_peak_marker, 0, 0);
  lv_obj_set_style_bg_color(s_signal_peak_marker, kSignalPeak(), 0);
  lv_obj_set_style_bg_opa(s_signal_peak_marker, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(s_signal_peak_marker, 0, 0);
  lv_obj_set_style_outline_width(s_signal_peak_marker, 0, 0);

  set_segment_level(0);
  update_peak_gauge(0.0f, 0);
  update_gauge_labels(0.0f, 0.0f, 0.0f, 0);
  lv_obj_invalidate(scr);
}

}  // namespace

void pulseox_demo_lvgl_init(TFT_eSPI* tft, int panel_w, int panel_h, int panel_x_off, int panel_y_off,
                            bool swap_bytes_for_push, bool swap_red_blue) {
  s_tft = tft;
  s_panel_w = panel_w;
  s_panel_h = panel_h;
  s_panel_x_off = panel_x_off;
  s_panel_y_off = panel_y_off;
  s_swap_bytes = swap_bytes_for_push;
  s_swap_red_blue = swap_red_blue;
  s_ui_h = max(0, panel_h - kBottomStatusH);

  if (s_inited) return;
  s_inited = true;

  lv_init();

  const uint32_t lines = 40;
  const uint32_t buf_px = (uint32_t)panel_w * (uint32_t)lines;
  const size_t buf_bytes = (size_t)buf_px * sizeof(lv_color_t);
  s_buf1 = (lv_color_t*)alloc_draw_buf_bytes(buf_bytes);
  s_buf2 = (lv_color_t*)alloc_draw_buf_bytes(buf_bytes);

  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, buf_px);
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = panel_w;
  s_disp_drv.ver_res = s_ui_h;
  s_disp_drv.flush_cb = flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;
  s_disp_drv.full_refresh = 0;
  lv_disp_drv_register(&s_disp_drv);

  create_ui();
}

void pulseox_demo_lvgl_set_active(bool active) {
  s_active = active;
  if (active) {
    s_last_pump_ms = millis();
    if (s_inited) {
      lv_obj_invalidate(lv_scr_act());
    }
  }
}

void pulseox_demo_lvgl_set_live_mode(bool live_mode) {
  s_live_mode = live_mode;
  if (live_mode) {
    s_display_spo2 = s_live_spo2;
    s_display_hr = s_live_hr;
    s_display_pi = s_live_pi;
  } else {
    s_live_signal_level = 0;
    s_peak_sig = 0.0f;
    s_peak_ms = 0;
    for (int i = 0; i < kGaugeCount; i++) {
      s_gauge_out_of_range[i] = false;
      s_gauge_alert_start_ms[i] = 0;
    }
    set_segment_level(0);
    update_peak_gauge(0.0f, millis());
  }
  if (s_active && s_inited) {
    lv_obj_invalidate(lv_scr_act());
  }
}

void pulseox_demo_lvgl_set_live_readings(float spo2, float hr, float pi, int signal_level_0_to_8) {
  s_live_spo2 = spo2;
  s_live_hr = hr;
  s_live_pi = pi;
  s_live_signal_level = max(0, min(8, signal_level_0_to_8));
}

void pulseox_demo_lvgl_pump(uint32_t now_ms) {
  if (!s_active || !s_inited) return;

  const uint32_t last = s_last_pump_ms;
  s_last_pump_ms = now_ms;
  const uint32_t delta = (now_ms >= last) ? (now_ms - last) : 0;
  if (delta > 0) {
    lv_tick_inc(delta);
  }

  if (s_live_mode) {
    update_live_readings(now_ms, delta);
  } else {
    update_demo_readings(lv_tick_get());
  }
  lv_timer_handler();
}

void pulseox_demo_lvgl_get_readings(float* out_spo2, float* out_hr, float* out_pi) {
  if (s_live_mode) {
    if (out_spo2) *out_spo2 = s_live_spo2;
    if (out_hr) *out_hr = s_live_hr;
    if (out_pi) *out_pi = s_live_pi;
    return;
  }

  if (out_spo2) *out_spo2 = s_demo_spo2;
  if (out_hr) *out_hr = s_demo_hr;
  if (out_pi) *out_pi = s_demo_pi;
}