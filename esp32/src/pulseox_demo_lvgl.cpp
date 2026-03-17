#include "pulseox_demo_lvgl.h"

#include <lvgl.h>

#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#endif

#include <cmath>

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

static lv_obj_t* s_segments[8] = {nullptr};
static lv_color_t* s_heart_buf[8] = {nullptr};
static int s_heart_sz = 0;

static lv_obj_t* s_peak_bg = nullptr;
static lv_obj_t* s_peak_fg = nullptr;
static int s_meter_y = 0;
static int s_meter_h = 0;
static int s_peak_w = 0;
static int s_peak_fg_x = 0;
static int s_peak_bg_x = 0;

static float s_peak_sig = 0.0f;
static uint32_t s_peak_ms = 0;

static float s_demo_spo2 = 0.0f;
static float s_demo_hr = 0.0f;
static float s_demo_pi = 0.0f;
static bool s_live_mode = false;
static float s_live_spo2 = 0.0f;
static float s_live_hr = 0.0f;
static float s_live_pi = 0.0f;
static int s_live_signal_level = 0;

// Reserve a bottom strip for time + WiFi/BLE dots, drawn by the main UI code
// (same as GK+). LVGL should not draw into this area.
static constexpr int kBottomStatusH = 12;

static lv_color_t make_rgb(uint8_t r, uint8_t g, uint8_t b) {
  if (s_swap_red_blue) {
    const uint8_t tmp = r;
    r = b;
    b = tmp;
  }
  return lv_color_make(r, g, b);
}

static lv_color_t kPinkOn() { return make_rgb(0xC7, 0x15, 0x85); }   // darker pink (medium violet red)
static lv_color_t kPinkOff() { return make_rgb(0x18, 0x00, 0x0C); }  // very dim pink
static lv_color_t kPeakBg() { return make_rgb(0x10, 0x10, 0x10); }
static lv_color_t kPeakFg() { return make_rgb(0xC7, 0x15, 0x85); }

static void* alloc_draw_buf_bytes(size_t bytes);

static void init_heart_mask(int grid, uint8_t* out /* grid*grid */) {
  // Procedural heart based on the classic implicit equation.
  // ((x^2 + y^2 - 1)^3 - x^2 * y^3) <= 0
  // We apply a small scale/offset tweak so it looks good in a square.
  const float inv = 1.0f / (float)(grid - 1);
  for (int y = 0; y < grid; y++) {
    for (int x = 0; x < grid; x++) {
      float nx = (2.0f * (x * inv)) - 1.0f;   // [-1, 1]
      float ny = 1.0f - (2.0f * (y * inv));   // [+1, -1] (up is positive)

      // Tweak: slightly narrower and shifted up to form a nicer heart.
      nx *= 1.05f;
      ny = (ny * 1.00f) + 0.10f;

      const float a = (nx * nx) + (ny * ny) - 1.0f;
      const float f = (a * a * a) - (nx * nx * ny * ny * ny);
      out[(y * grid) + x] = (f <= 0.0f) ? 1u : 0u;
    }
  }
}

static void draw_heart_canvas(int idx, bool on) {
  if (idx < 0 || idx >= 8) return;
  lv_obj_t* canvas = s_segments[idx];
  lv_color_t* buf = s_heart_buf[idx];
  const int sz = s_heart_sz;
  if (!canvas || !buf || sz <= 0) return;

  // Pixel-art heart masks.
  // - 8x8: original chunky look
  // - 16x16/24x24: procedurally generated so it reads as an actual heart
  static constexpr uint8_t kMask8[8] = {
      0b01100110,
      0b11111111,
      0b11111111,
      0b11111111,
      0b01111110,
      0b00111100,
      0b00011000,
      0b00000000,
  };

  const int grid = (sz >= 24) ? 24 : ((sz >= 16) ? 16 : 8);

  static bool s_mask16_ready = false;
  static uint8_t s_mask16[16 * 16];
  static bool s_mask24_ready = false;
  static uint8_t s_mask24[24 * 24];

  if (grid == 16 && !s_mask16_ready) {
    init_heart_mask(16, s_mask16);
    s_mask16_ready = true;
  }
  if (grid == 24 && !s_mask24_ready) {
    init_heart_mask(24, s_mask24);
    s_mask24_ready = true;
  }

  auto mask_at = [&](int x, int y) -> bool {
    if (grid == 24) {
      if (x < 0 || x >= 24 || y < 0 || y >= 24) return false;
      return s_mask24[(y * 24) + x] != 0;
    }
    if (grid == 16) {
      if (x < 0 || x >= 16 || y < 0 || y >= 16) return false;
      return s_mask16[(y * 16) + x] != 0;
    }
    if (x < 0 || x >= 8 || y < 0 || y >= 8) return false;
    return ((kMask8[y] >> (7 - x)) & 1u) != 0;
  };

  auto is_outline_pixel = [&](int x, int y) -> bool {
    if (!mask_at(x, y)) return false;
    // Boundary pixel if any neighbor is outside the mask.
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        if (dx == 0 && dy == 0) continue;
        if (!mask_at(x + dx, y + dy)) return true;
      }
    }
    return false;
  };

  const lv_color_t bg = lv_color_hex(0x000000);
  const lv_color_t fg = on ? kPinkOn() : kPinkOff();

  // Clear to black.
  for (int i = 0; i < sz * sz; i++) {
    buf[i] = bg;
  }

  const int cell = max(1, sz / grid);
  const int art_w = cell * grid;
  const int art_h = cell * grid;
  const int ox = max(0, (sz - art_w) / 2);
  const int oy = max(0, (sz - art_h) / 2);

  for (int y = 0; y < grid; y++) {
    for (int x = 0; x < grid; x++) {
      if (on) {
        if (!mask_at(x, y)) continue;
      } else {
        if (!is_outline_pixel(x, y)) continue;
      }
      const int px0 = ox + x * cell;
      const int py0 = oy + y * cell;
      for (int dy = 0; dy < cell; dy++) {
        const int py = py0 + dy;
        if (py < 0 || py >= sz) continue;
        lv_color_t* line = &buf[py * sz];
        for (int dx = 0; dx < cell; dx++) {
          const int px = px0 + dx;
          if (px < 0 || px >= sz) continue;
          line[px] = fg;
        }
      }
    }
  }

  lv_obj_invalidate(canvas);
}

static void compute_panel_abs_xy(int* out_x, int* out_y) {
  if (!s_tft) {
    *out_x = 0;
    *out_y = 0;
    return;
  }

  // Match the logic in main.cpp's getPanelAbsXY/applyPanelViewport.
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

  const int w = (area->x2 - area->x1 + 1);
  const int h = (area->y2 - area->y1 + 1);

  s_tft->setSwapBytes(s_swap_bytes);
  s_tft->startWrite();
  s_tft->pushImage(ax + area->x1, ay + area->y1, w, h, (uint16_t*)color_p);
  s_tft->endWrite();
  // Important: don't leak the LVGL swap-byte setting into subsequent draws.
  // If left enabled, later sprite blits will have their 16-bit pixels byte-swapped
  // and colors like red/yellow will appear incorrect.
  s_tft->setSwapBytes(false);

  lv_disp_flush_ready(disp);
}

static float beat_wave(float phase01) {
  // A simple pulse-like waveform: strong systolic peak + smaller dicrotic bump.
  // phase01 in [0..1).
  const float x1 = (phase01 - 0.12f) / 0.045f;
  const float x2 = (phase01 - 0.36f) / 0.07f;
  float v = 0.10f;
  v += 0.90f * expf(-(x1 * x1));
  v += 0.20f * expf(-(x2 * x2));
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  return v;
}

static void set_segment_level(int level_0_to_8) {
  const int lvl = max(0, min(8, level_0_to_8));
  for (int i = 0; i < 8; i++) {
    const bool on = (i < lvl);
    draw_heart_canvas(i, on);
  }
}

static void update_peak_gauge(float sig, uint32_t now_ms) {
  // Peak-hold for 2s, then decay down.
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
      // Linear decay of full-scale (1.0) over kDecayMs.
      const float dec = (float)dt_ms / (float)kDecayMs;
      s_peak_sig = max(sig, max(0.0f, s_peak_sig - dec));
    }
  }

  if (!s_peak_fg || s_meter_h <= 0 || s_peak_w <= 0) return;
  const int fill_h = (int)lroundf(max(0.0f, min(1.0f, s_peak_sig)) * (float)s_meter_h);
  const int y = s_meter_y + (s_meter_h - fill_h);
  lv_obj_set_pos(s_peak_fg, s_peak_fg_x, y);
  lv_obj_set_size(s_peak_fg, s_peak_w, fill_h);
}

static void update_demo_readings(uint32_t t_ms) {
  // Fake, deterministic vitals.
  // Also cycles through NORMAL/ABNORMAL/DANGER every few seconds so the
  // right-side status bar can demonstrate all states.
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

  const float hr_base[3] = {72.0f, 88.0f, 110.0f};
  const float spo2_base[3] = {98.0f, 92.0f, 86.0f};
  const float pi_base[3] = {2.6f, 1.7f, 0.9f};

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
  set_segment_level(seg);
  update_peak_gauge(sig, t_ms);

  const float spo20 = spo2_base[stage];
  const float spo21 = spo2_base[next_stage];
  const float spo2 = lerp(spo20, spo21, mix) + 0.6f * sinf((float)t_ms / 7000.0f);

  const float pi0 = pi_base[stage];
  const float pi1 = pi_base[next_stage];
  const float pi = lerp(pi0, pi1, mix) + 0.45f * (sig - 0.1f) + 0.10f * sinf((float)t_ms / 3000.0f);

  s_demo_spo2 = spo2;
  s_demo_hr = hr;
  s_demo_pi = pi;
}

static void update_live_readings(uint32_t now_ms) {
  const int seg = max(0, min(8, s_live_signal_level));
  const float sig = (float)seg / 8.0f;
  set_segment_level(seg);
  update_peak_gauge(sig, now_ms);
}

static void create_ui() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Layout goals:
  // - Meter: 8 stacked *square* segments (vertical), centered.
  // - Vitals: GK+-style: label above value (spo2/hr/pi), all visible.
  const int pad_x = 2;
  const int pad_top = 2;
  const int seg_gap = 2;
  const int peak_gap = 2;
  const int peak_w = 5;

  const int label_h = 10;  // unscii_8-ish
  const int value_h = 18;  // unscii_16-ish
  const int metric_gap = 4;
  const int metric_h = label_h + value_h;
  const int metrics_h = (3 * metric_h) + (2 * metric_gap);

  const int meter_area_h = max(0, s_ui_h - (pad_top + metrics_h + 6));
  const int max_s_by_w = max(6, (s_panel_w - (pad_x * 2)));
  const int max_s_by_h = max(6, (meter_area_h - (7 * seg_gap)) / 8);
  const int seg_s = max(6, min(max_s_by_w, max_s_by_h));
  s_heart_sz = seg_s;
  const int meter_h = (8 * seg_s) + (7 * seg_gap);
  // Keep the hearts centered; place the peak gauge just to their left.
  const int meter_x = (s_panel_w - seg_s) / 2;
  const int meter_y = pad_top + max(0, (meter_area_h - meter_h) / 2);

  s_meter_y = meter_y;
  s_meter_h = meter_h;
  s_peak_w = peak_w;

  // Peak gauge: real bar sits just left of the hearts, shadow is 1px behind it.
  const int peak_fg_x = meter_x - peak_gap - peak_w;
  const int peak_x0 = peak_fg_x - 1;
  s_peak_fg_x = peak_fg_x;
  s_peak_bg_x = peak_x0;
  s_peak_bg = lv_obj_create(scr);
  lv_obj_set_pos(s_peak_bg, peak_x0, meter_y);
  lv_obj_set_size(s_peak_bg, peak_w, meter_h);
  lv_obj_clear_flag(s_peak_bg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(s_peak_bg, 0, 0);
  lv_obj_set_style_outline_width(s_peak_bg, 0, 0);
  lv_obj_set_style_shadow_width(s_peak_bg, 0, 0);
  lv_obj_set_style_radius(s_peak_bg, 2, 0);
  lv_obj_set_style_pad_all(s_peak_bg, 0, 0);
  lv_obj_set_style_bg_color(s_peak_bg, kPeakBg(), 0);
  lv_obj_set_style_bg_opa(s_peak_bg, LV_OPA_50, 0);

  s_peak_fg = lv_obj_create(scr);
  // Real bar sits 1px to the right of the shadow, closest to the hearts.
  lv_obj_set_pos(s_peak_fg, peak_fg_x, meter_y + meter_h);
  lv_obj_set_size(s_peak_fg, peak_w, 0);
  lv_obj_clear_flag(s_peak_fg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(s_peak_fg, 0, 0);
  lv_obj_set_style_outline_width(s_peak_fg, 0, 0);
  lv_obj_set_style_shadow_width(s_peak_fg, 0, 0);
  lv_obj_set_style_radius(s_peak_fg, 2, 0);
  lv_obj_set_style_pad_all(s_peak_fg, 0, 0);
  lv_obj_set_style_bg_color(s_peak_fg, kPeakFg(), 0);
  lv_obj_set_style_bg_opa(s_peak_fg, LV_OPA_COVER, 0);

  for (int i = 0; i < 8; i++) {
    // Heart segment: LVGL canvas so we can do chunky pixel-art.
    lv_obj_t* c = lv_canvas_create(scr);
    s_segments[i] = c;
    lv_obj_set_size(c, seg_s, seg_s);
    const int y = meter_y + (7 - i) * (seg_s + seg_gap);
    lv_obj_set_pos(c, meter_x, y);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);

    const size_t bytes = (size_t)seg_s * (size_t)seg_s * sizeof(lv_color_t);
    s_heart_buf[i] = (lv_color_t*)alloc_draw_buf_bytes(bytes);
    lv_canvas_set_buffer(c, s_heart_buf[i], seg_s, seg_s, LV_IMG_CF_TRUE_COLOR);
    draw_heart_canvas(i, false);
  }

  // Vitals are drawn using TFT_eSPI in main.cpp so they match the GK+ font/style.

  // Force a full repaint on first frame.
  lv_obj_invalidate(scr);
}

static void* alloc_draw_buf_bytes(size_t bytes) {
#if defined(MALLOC_CAP_SPIRAM)
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  return malloc(bytes);
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

  // Small draw buffers; LVGL will flush in tiles.
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
  if (!live_mode) {
    s_live_signal_level = 0;
    s_peak_sig = 0.0f;
    s_peak_ms = 0;
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
    update_live_readings(now_ms);
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
