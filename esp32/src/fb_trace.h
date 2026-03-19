#pragma once
// ---------- Serial framebuffer trace – shared infrastructure ----------
// GPIO-gated, 4bpp base64-encoded pixel trace for offline inspection.
// Used by both the LVGL PulseOx renderer and the TFT_eSPI static screens.

#include <Arduino.h>

static constexpr int  kFbTraceGpio       = 7;
static constexpr int  kFbTraceW          = 38;
static constexpr int  kFbTraceH          = 68;
static constexpr int  kFbTraceTotalPx    = kFbTraceW * kFbTraceH;          // 2584
static constexpr int  kFbTracePackedBytes = (kFbTraceTotalPx + 1) / 2;    // 1292
static constexpr uint32_t kFbTraceIntervalMs = 300;

// Returns true when the trace pin is grounded (active-low).
inline bool fbTraceGateActive() {
  return digitalRead(kFbTraceGpio) == LOW;
}

// Map 8-bit RGB to a 4-bit palette index.
//   0=BLK  1=BG  2=DIM  3=WHT  4=CYN  5=GRN  6=AMB  7=RED  8=YEL
inline uint8_t fbTracePaletteIndex(uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t mx = max(r, max(g, b));
  if (mx < 14u) return 0;
  const uint8_t mn = min(r, min(g, b));
  if (r > 215u && g > 215u && b > 215u) return 3;  // white
  if ((mx - mn) < 40u) {                            // near-gray
    if (mx > 95u) return 2;                          // dim
    return 1;                                        // bg
  }
  if (r > 180u && g < 95u && b < 95u) return 7;  // red
  if (r > 180u && g > 140u && b < 110u) return 6; // amber
  if (b > 140u && g > 120u) return 4;              // cyan
  if (g > 150u && r < 190u && b < 170u) return 5;  // green
  if (r > 150u && g > 150u && b < 130u) return 8;  // yellow
  if (mx > 95u) return 2;                           // dim
  return 1;                                         // bg
}

// Emit an FBMETA banner followed by an FB line for the given trace buffer.
// `seq` is incremented after each emit.
inline void fbTraceEmit(const uint8_t* traceBuf, uint32_t& seq) {
  Serial.printf("FBMETA v3 gpio=%d active=LOW w=%d h=%d fmt=b64_4bpp\n",
                kFbTraceGpio, kFbTraceW, kFbTraceH);

  static constexpr char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  uint8_t packed[kFbTracePackedBytes];
  for (int i = 0; i < kFbTraceTotalPx; i += 2) {
    const uint8_t hi = traceBuf[i] & 0x0F;
    const uint8_t lo = (i + 1 < kFbTraceTotalPx) ? (traceBuf[i + 1] & 0x0F) : 0;
    packed[i >> 1] = (uint8_t)((hi << 4) | lo);
  }

  Serial.print("FB ");
  Serial.print(seq++);
  Serial.print(' ');

  char line[96];
  int used = 0;
  for (int i = 0; i < kFbTracePackedBytes; i += 3) {
    const uint8_t a = packed[i];
    const uint8_t b = (i + 1 < kFbTracePackedBytes) ? packed[i + 1] : 0;
    const uint8_t c = (i + 2 < kFbTracePackedBytes) ? packed[i + 2] : 0;
    line[used++] = b64[(a >> 2) & 0x3F];
    line[used++] = b64[((a << 4) | (b >> 4)) & 0x3F];
    line[used++] = b64[((b << 2) | (c >> 6)) & 0x3F];
    line[used++] = b64[c & 0x3F];
    if (used >= (int)sizeof(line) - 4) {
      Serial.write((const uint8_t*)line, used);
      used = 0;
    }
  }
  if (used > 0) {
    Serial.write((const uint8_t*)line, used);
  }
  Serial.println();
}
