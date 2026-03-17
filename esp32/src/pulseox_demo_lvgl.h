#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// LVGL-based Pulse Ox demo UI.
// - Renders a vertical 8-segment "signal" bar (bright green segments)
// - Shows SpO2, Heart Rate, and PI numeric readouts
// - Drives with a fake heartbeat waveform (demo only)

void pulseox_demo_lvgl_init(TFT_eSPI* tft, int panel_w, int panel_h, int panel_x_off, int panel_y_off,
                            bool swap_bytes_for_push, bool swap_red_blue);

void pulseox_demo_lvgl_set_active(bool active);

// Switch between demo-driven values and externally supplied live values.
void pulseox_demo_lvgl_set_live_mode(bool live_mode);

// Update the latest live PulseOx readings and signal level.
void pulseox_demo_lvgl_set_live_readings(float spo2, float hr, float pi, int signal_level_0_to_8);

// Call periodically from the render task while active.
void pulseox_demo_lvgl_pump(uint32_t now_ms);

// Latest demo readings (fake heartbeat-driven values). Intended for rendering
// with TFT_eSPI so PulseOx can match the GK+ font/style.
void pulseox_demo_lvgl_get_readings(float* out_spo2, float* out_hr, float* out_pi);
