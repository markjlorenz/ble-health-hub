#include <Arduino.h>
#include <TFT_eSPI.h>
#include "fb_trace.h"

#include <WiFi.h>
#include <WiFiSTA.h>

// WiFiManager (tzapu/WiFiManager)
#include <WiFiManager.h>

// QRCode (ricmoo/QRCode)
#include <qrcode.h>

// BLE (NimBLE-Arduino)
#include <NimBLEDevice.h>

#include <cmath>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#if __has_include(<esp_wifi.h>)
extern "C" {
#include <esp_wifi.h>
}
#endif

#if __has_include(<esp_system.h>)
extern "C" {
#include <esp_system.h>
}
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <rlottie.h>

#if __has_include(<esp_sleep.h>)
extern "C" {
#include <esp_sleep.h>
}
#endif

#if __has_include(<esp32-hal-touch.h>)
#include <esp32-hal-touch.h>
#endif

#include "generated/flame_lottie.h"

#include "pulseox_demo_lvgl.h"

#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#endif

static TFT_eSPI tft;

// Bring-up mode: continuously cycle a high-contrast pattern so we can
// confidently confirm the panel is rendering (and which rotation looks right)
// before we switch back to the dashboard UI.
static constexpr bool kDisplayBringupMode = false;
static constexpr bool kBacklightCalMode = false;
static constexpr bool kDebugTestPattern = false;
static constexpr bool kDebugPixelStats = false;
static constexpr bool kForceOpaqueNearWhite = false;
static constexpr bool kKeyOutOpaqueWhiteBackground = true;
static constexpr bool kInvertDisplay = false;
// Color pipeline settings (locked-in after calibration).
static constexpr bool kSwapBytesForPushImage = true;
static constexpr bool kSwapRedBlueInConvert = true;

// Demo mode: cycles through ketosis states and toggles Lottie layer visibility.
static constexpr bool kDemoMode = true;
static constexpr uint32_t kDemoStepMs = 4500;

// Demo gates (active-low): ground the pin to enable a demo.
// Uses the internal pull-up, so leaving it floating will read HIGH (demo off).
// Picked to avoid common strapping pins and to avoid TFT pins.
//
// - GPIO5: GK+ demo cycle (familiar ketosis stages)
// - GPIO6: Pulse Ox demo screen (LVGL)
// - GPIO4: Utility demo carousel (NO WIFI / LOW BATTERY / GOOD BYE)
static constexpr int kGkDemoGateGpio = 5;
static constexpr int kPulseOxDemoGateGpio = 6;
static constexpr int kUtilityDemoGateGpio = 4;

// Deep sleep wake source.
// NOTE: This must be a touch-capable GPIO for your target (ESP32-S3 has many).
// Override at build time with `-D WAKE_TOUCH_GPIO=<gpio>` if needed.
#ifndef WAKE_TOUCH_GPIO
#define WAKE_TOUCH_GPIO 13
#endif
static constexpr int kWakeTouchGpio = WAKE_TOUCH_GPIO;

// Touch wake threshold.
// - ESP32: absolute raw threshold (lower triggers)
// - ESP32-S2/S3: increment above baseline (higher triggers)
// Set to 0 to auto-select.
#ifndef WAKE_TOUCH_THRESHOLD
#define WAKE_TOUCH_THRESHOLD 0
#endif

// Sleep rules (per user spec).
static constexpr uint32_t kSleepAfterWifiSetupMs = 2u * 60u * 1000u;
static constexpr uint32_t kSleepAfterBleWaitMs = 1u * 60u * 1000u;
static constexpr uint32_t kSleepAfterShowingReadingMs = 30u * 1000u;

// Physical visible window is 76px wide.
static constexpr int kPanelW = 76;
static constexpr int kPanelH = 284;

// Empirical default for a 240px-wide controller canvas cropped to a 76px window:
// maxX = 240 - 76 = 164, midpoint ~= 82.
// If your image is shifted, adjust this single value.
static constexpr int kPanelXOff = 82;

// Long-axis (Y) alignment tweak.
// This is applied as a *viewport* Y offset (safe: never makes text disappear).
// If the top is clipped, try small positive values (e.g. 4, 8, 12).
// With a 240x320 logical canvas, a good first guess is centering 284px inside 320px:
// (320 - 284) / 2 = 18.
static constexpr int kPanelYOff = 18;

// Reserve a small header for static UI text so the animation doesn't overwrite it.
static constexpr int kOverlayH = 170;
static constexpr int kOverlayTextPadTopPx = 4;
// In the LOADING stage we want the logs to fall from near the top, but we still
// need a stable place for the "LOADING" label that doesn't get overwritten by
// the animation every frame (which causes visible flashing).
static constexpr int kLoadingStatusH = 16;

static float gGluMgDl = 110.0f;
static float gKetMmolL = 1.2f;
static bool gGkiOverride = false;
static float gGkiValue = 0.0f;
static String gKetosisLabel = "";
static uint16_t gKetosisLabelBg = TFT_BLACK;
static uint16_t gKetosisLabelFg = TFT_WHITE;

static inline uint16_t rgb888To565(uint8_t r, uint8_t g, uint8_t b) {
  if (kSwapRedBlueInConvert) {
    const uint8_t tmp = r;
    r = b;
    b = tmp;
  }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Colors sampled from esp32/assets/flame.lottie (fills in named layers).
static uint16_t gKetosisBrown565 = 0;
static uint16_t gKetosisOrange565 = 0;
static uint16_t gKetosisYellow565 = 0;
static uint16_t gKetosisRed565 = 0;

static void setupKetosisLabelColorsOnce() {
  static bool inited = false;
  if (inited) return;
  inited = true;

  // Extracted via tmp/extract_lottie_colors.py
  // Brown:  #713E27 (Logs) (darkest of the browns found in Logs)
  // Orange: #FD7719 (Orange Container)
  // Yellow: #FBD03F (Yellow Container)
  // Red:    #F4383E (BigRed/SideRed/RedWhips Outline Container)
  gKetosisBrown565 = rgb888To565(0x71, 0x3E, 0x27);
  gKetosisOrange565 = rgb888To565(0xFD, 0x77, 0x19);
  gKetosisYellow565 = rgb888To565(0xFB, 0xD0, 0x3F);
  gKetosisRed565 = rgb888To565(0xF4, 0x38, 0x3E);
}

static float computeGki(float gluMgDl, float ketMmolL) {
  if (ketMmolL <= 0.0f) return 0.0f;
  const float gluMmolL = gluMgDl / 18.0f;
  return gluMmolL / ketMmolL;
}

static float getDisplayedGki() {
  return gGkiOverride ? gGkiValue : computeGki(gGluMgDl, gKetMmolL);
}

static void applyPanelViewport();
static void getPanelAbsXY(int* outX, int* outY);
static void* mallocFrameBuffer(size_t bytes);
static void renderAndPushLottieFrame();
static void drawStatusDots();
static void drawPulseOxMetrics();
static bool isGkDemoGateActive();
static bool isPulseOxDemoGateActive();
static bool isUtilityDemoGateActive();
static bool isAnyDemoGateActive();
static bool isDemoNoWifiScreen();
static void setBleConnected(bool v);
static void requestUiStep(int step);
static void setBacklight(bool on);
static void setBacklightRaw(bool levelHigh);

enum SleepPolicyState : uint8_t {
  kSleepDisabled = 0,
  kSleepWaitingWifiSetup = 1,
  kSleepWaitingBle = 2,
  kSleepShowingReading = 3,
  kSleepNone = 4,
};

static volatile bool gTouchWakeConfigured = false;
static volatile uint32_t gTouchWakeThreshold = 0;

// Touch configuration snapshot (for runtime press detection).
static volatile uint32_t gTouchWakeBaseline = 0;
static volatile uint32_t gTouchWakeThresholdAbs = 0;

// Touch-driven BLE behavior.
static volatile bool gBlockGkPlusConnections = false;
static volatile bool gBlockPulseOxConnections = false;
static volatile bool gGkDisconnectRequested = false;
static volatile bool gPulseOxDisconnectRequested = false;

static SleepPolicyState computeSleepPolicyState();
static uint32_t sleepTimeoutForPolicyState(SleepPolicyState st);
static void configureTouchWakeIfNeeded();
static void enterDeepSleepNow(const char* why);
static bool readTouchActiveNow();
static void handleTouchPress();

// ---------- Serial frame trace for static screens (GPIO7 gated) ----------
// Reads pixels back from gPanelFxSprite after a static screen has been drawn,
// quantises with the shared palette, and emits via the common FB protocol.
static uint32_t sMainTraceSeq = 0;

static uint8_t mainTracePaletteIndex(uint16_t rgb565) {
  uint8_t r = (uint8_t)(((rgb565 >> 11) & 0x1F) * 255u / 31u);
  const uint8_t g = (uint8_t)(((rgb565 >> 5) & 0x3F) * 255u / 63u);
  uint8_t b = (uint8_t)((rgb565 & 0x1F) * 255u / 31u);
  if (kSwapRedBlueInConvert) { const uint8_t t = r; r = b; b = t; }
  return fbTracePaletteIndex(r, g, b);
}

// Forward declaration – body after gPanelFxSprite is declared.
static void emitStaticScreenTrace();

static inline uint16_t rgba8888ToRgb565(uint32_t argb) {
  // RLottie uses ARGB32 pixels.
  uint32_t a = (argb >> 24) & 0xFF;
  uint32_t r = (argb >> 16) & 0xFF;
  uint32_t g = (argb >> 8) & 0xFF;
  uint32_t b = (argb >> 0) & 0xFF;

  if (kSwapRedBlueInConvert) {
    const uint32_t tmp = r;
    r = b;
    b = tmp;
  }

  if (kForceOpaqueNearWhite) {
    // Heuristic for "white-ish but transparent" pixels.
    // RLottie outputs premultiplied ARGB; for semi-transparent white,
    // RGB is often ~= A (e.g. 0x80_808080). In that case, we treat it as
    // intended-white and force it fully opaque so it stays bright on black.
    const int ir = (int)r;
    const int ig = (int)g;
    const int ib = (int)b;
    const int ia = (int)a;
    const int maxDiffRgb = max(abs(ir - ig), abs(ir - ib));
    const bool rgbIsGray = (maxDiffRgb <= 3);
    const bool rgbMatchesAlpha = (abs(ir - ia) <= 3);
    const bool isVeryBright = (r >= 240 && g >= 240 && b >= 240);
    const bool looksLikePremulWhite = (a >= 32 && a < 255 && rgbIsGray && rgbMatchesAlpha);

    if ((a < 255 && isVeryBright) || looksLikePremulWhite) {
      a = 255;
      r = 255;
      g = 255;
      b = 255;
    }
  }

  // Composite onto a black background.
  // Even if the renderer is premultiplied, multiplying by alpha again is safe
  // (transparent stays black; opaque remains unchanged).
  r = (r * a) / 255;
  g = (g * a) / 255;
  b = (b * a) / 255;

  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static bool isGkDemoGateActive() {
  // LOW means the pin is grounded.
  return digitalRead(kGkDemoGateGpio) == LOW;
}

static bool isPulseOxDemoGateActive() {
  // LOW means the pin is grounded.
  return digitalRead(kPulseOxDemoGateGpio) == LOW;
}

static bool isUtilityDemoGateActive() {
  // LOW means the pin is grounded.
  return digitalRead(kUtilityDemoGateGpio) == LOW;
}

static bool isAnyDemoGateActive() {
  return isGkDemoGateActive() || isPulseOxDemoGateActive() || isUtilityDemoGateActive();
}

static bool isDemoNoWifiScreen() {
  // Only the GK+ demo or utility demo ever shows the NO WIFI screen.
  return kDemoMode && (isGkDemoGateActive() || isUtilityDemoGateActive());
}

static volatile bool gBleConnected = false;

// Latest GK+ reading time (from the meter): "HH:MM".
static char gGkWhenLocal[8] = {0};

// Forces a repaint of the reserved bottom status strip (time + dots). This is
// needed because some stage transitions clear parts of the screen while the
// placeholder values may remain constant.
static volatile bool gStatusStripDirty = true;

// Some screens (e.g. PulseOx demo) want the WiFi/BLE dots but not a timestamp.
static bool gSuppressStatusTime = false;

// Reserve a bottom strip for status (time + dots) so the animation never
// overwrites it. This prevents visible flicker during the long pushImage.
static constexpr int kBottomStatusH = 12;

static void drawStatusDots() {
  // Always draw dots so we don't leave stale pixels.
  // Wi-Fi status stays in the lower-right. BLE status (green when connected) is in the lower-left.
  int ax = 0;
  int ay = 0;
  getPanelAbsXY(&ax, &ay);

  static constexpr int kDot = 4;
  const int stripY = ay + (kPanelH - kBottomStatusH);
  const int y = ay + (kPanelH - kDot);
  const int bleX = ax;
  const int wifiX = ax + (kPanelW - kDot);

  // Use our calibrated RGB888->565 conversion (handles red/blue swap)
  // instead of TFT_* constants.
  static const uint16_t kBlue = rgb888To565(0x00, 0x00, 0xFF);
  static const uint16_t kRed = rgb888To565(0xFF, 0x00, 0x00);
  // Darker green is less distracting and reads better on this panel.
  static const uint16_t kGreen = rgb888To565(0x00, 0xA0, 0x00);

  const bool demoPlaceholders = (kDemoMode && isAnyDemoGateActive());

  // In demo mode, show placeholder status values so the bottom strip is always
  // populated (time + connection indicators) while cycling stages.
  const bool wifiUp = demoPlaceholders ? true : (WiFi.status() == WL_CONNECTED);
  const uint16_t wifiC = wifiUp ? kBlue : kRed;
  const bool bleUp = demoPlaceholders ? true : gBleConnected;
  const uint16_t bleC = bleUp ? kGreen : TFT_BLACK;

  // Bottom status row: show time centered between dots (unless suppressed).
  const char* timeStr = gSuppressStatusTime ? "" : (demoPlaceholders ? "10:26" : gGkWhenLocal);
  const bool haveTime = (!gSuppressStatusTime) && (timeStr && timeStr[0] != '\0');

  // Only repaint if something changed (avoids flicker from "erase then redraw").
  static bool sInited = false;
  static bool sLastWifiUp = false;
  static bool sLastBleUp = false;
  static char sLastTime[sizeof(gGkWhenLocal)] = {0};

  bool changed = !sInited;
  if (gStatusStripDirty) changed = true;
  if (sLastWifiUp != wifiUp) changed = true;
  if (sLastBleUp != bleUp) changed = true;
  if (strncmp(sLastTime, timeStr ? timeStr : "", sizeof(sLastTime)) != 0) changed = true;

  if (!changed) {
    return;
  }

  sInited = true;
  sLastWifiUp = wifiUp;
  sLastBleUp = bleUp;
  strncpy(sLastTime, timeStr ? timeStr : "", sizeof(sLastTime));
  sLastTime[sizeof(sLastTime) - 1] = '\0';
  gStatusStripDirty = false;

  // Clear the full strip once, then paint dots + time.
  tft.startWrite();
  tft.fillRect(ax, stripY, kPanelW, kBottomStatusH, TFT_BLACK);
  tft.fillRect(bleX, y, kDot, kDot, bleC);
  tft.fillRect(wifiX, y, kDot, kDot, wifiC);

  if (haveTime) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(BC_DATUM);
    tft.drawString(timeStr, ax + (kPanelW / 2), ay + kPanelH - 1);
    tft.setTextDatum(TL_DATUM);
  }
  tft.endWrite();
}

// Offscreen PulseOx metrics sprite (to avoid visible clear+redraw flicker).
static constexpr int kPulseOxMetricsH = 96;  // 3 rows @ 32px each
static TFT_eSprite gPulseOxMetricsSprite = TFT_eSprite(&tft);
static bool gPulseOxMetricsSpriteOk = false;
static void initPulseOxMetricsSpriteOnce() {
  if (gPulseOxMetricsSpriteOk) return;
  gPulseOxMetricsSprite.setColorDepth(16);
  gPulseOxMetricsSpriteOk = (gPulseOxMetricsSprite.createSprite(kPanelW, kPulseOxMetricsH) != nullptr);
  if (gPulseOxMetricsSpriteOk) {
    gPulseOxMetricsSprite.fillSprite(TFT_BLACK);
  }
}

static void drawPulseOxMetrics() {
  // Draw PulseOx vitals using the same TFT_eSPI styling as GK+ metrics:
  // label (slightly gray) above value (white), left aligned.
  float spo2 = 0.0f;
  float hr = 0.0f;
  float pi = 0.0f;
  pulseox_demo_lvgl_get_readings(&spo2, &hr, &pi);

  // SpO2 status thresholds.
  // NOTE: These are demo-oriented heuristics, not medical advice.
  static constexpr float kSpo2NormalMin = 95.0f;
  static constexpr float kSpo2AbnormalMin = 90.0f;

  int ax = 0;
  int ay = 0;
  getPanelAbsXY(&ax, &ay);

  // Reserve bottom strip for dots.
  const int uiH = kPanelH - kBottomStatusH;
  const int metricsY = ay + max(0, uiH - kPulseOxMetricsH);

  initPulseOxMetricsSpriteOnce();
  TFT_eSprite* spr = gPulseOxMetricsSpriteOk ? &gPulseOxMetricsSprite : nullptr;

  struct StatusBarStyle {
    uint16_t color;
    uint16_t textColor;
    const char* label;
  };

  auto computeSpo2Status = [&]() -> StatusBarStyle {
    StatusBarStyle s;
    if (spo2 < kSpo2AbnormalMin) {
      s.color = rgb888To565(255, 0, 0);
      s.textColor = TFT_WHITE;
      s.label = "DANGER";
      return s;
    }
    if (spo2 < kSpo2NormalMin) {
      s.color = rgb888To565(255, 215, 0);
      s.textColor = TFT_BLACK;
      s.label = "ABNORMAL";
      return s;
    }
    s.color = rgb888To565(0, 140, 0);  // darker, grassier green
    s.textColor = TFT_WHITE;
    s.label = "NORMAL";
    return s;
  };

  // Draw a label rotated 90deg CCW (reads bottom->top) by rasterizing the word
  // into a tiny sprite and blitting its pixels with a CCW transform.
  static TFT_eSprite sLabelSpr = TFT_eSprite(&tft);
  static bool sLabelSprOk = false;
  if (!sLabelSprOk) {
    sLabelSpr.setColorDepth(16);
    // Enough width for "ABNORMAL" at font size 1 (~6px/char).
    sLabelSprOk = (sLabelSpr.createSprite(96, 16) != nullptr);
  }

  auto drawRotatedLabelCCWIntoSprite = [&](TFT_eSprite* dst, int barLeftX, int barTopY, int barW, int barH,
                                           const char* text, uint16_t fg, uint16_t bg) {
    if (!sLabelSprOk) return;

    static constexpr int kKernPx = 2;  // slightly wider letter spacing

    int len = 0;
    while (text[len] != '\0') len++;
    const int srcH = 8;                 // built-in font height at text size 1
    const int srcW = min(95, max(0, (len * (6 + kKernPx)) - kKernPx));

    sLabelSpr.fillSprite(bg);
    sLabelSpr.setTextDatum(TL_DATUM);
    sLabelSpr.setTextSize(1);
    sLabelSpr.setTextColor(fg, bg);
    int cx = 0;
    for (int i = 0; i < len; i++) {
      char cstr[2] = {text[i], 0};
      sLabelSpr.drawString(cstr, cx, 0);
      cx += (6 + kKernPx);
      if (cx >= (sLabelSpr.width() - 6)) break;
    }

    const int dstW = srcH;
    const int dstH = srcW;
    const int dx0 = barLeftX + max(0, (barW - dstW) / 2);
    const int dy0 = barTopY + max(0, (barH - dstH) / 2);

    for (int sy = 0; sy < srcH; sy++) {
      for (int sx = 0; sx < srcW; sx++) {
        const uint16_t c = sLabelSpr.readPixel(sx, sy);
        if (c == bg) continue;
        // CCW: (sx,sy) -> (sy, srcW-1-sx)
        const int dx = dx0 + sy;
        const int dy = dy0 + (srcW - 1 - sx);
        if (dx < barLeftX || dx >= (barLeftX + barW) || dy < barTopY || dy >= (barTopY + barH)) continue;
        dst->drawPixel(dx, dy, c);
      }
    }
  };

  if (spr) {
    spr->fillSprite(TFT_BLACK);
  } else {
    tft.startWrite();
    tft.fillRect(ax, metricsY, kPanelW, kPulseOxMetricsH, TFT_BLACK);
  }

  // Right-side status bar (glued to the edge).
  const int barW = 18;
  const int barX = kPanelW - barW;
  const int barR = 4;
  const StatusBarStyle status = computeSpo2Status();

  if (spr) {
    spr->fillRoundRect(barX, 0, barW, kPulseOxMetricsH, barR, status.color);
    drawRotatedLabelCCWIntoSprite(spr, barX, 0, barW, kPulseOxMetricsH, status.label, status.textColor, status.color);
  } else {
    tft.fillRoundRect(ax + barX, metricsY, barW, kPulseOxMetricsH, barR, status.color);
    // Fallback: rasterize into sLabelSpr and blit pixels to the main display.
    if (sLabelSprOk) {
      static constexpr int kKernPx = 2;
      int len = 0;
      while (status.label[len] != '\0') len++;
      const int srcH = 8;
      const int srcW = min(95, max(0, (len * (6 + kKernPx)) - kKernPx));

      sLabelSpr.fillSprite(status.color);
      sLabelSpr.setTextDatum(TL_DATUM);
      sLabelSpr.setTextSize(1);
      sLabelSpr.setTextColor(status.textColor, status.color);
      int cx = 0;
      for (int i = 0; i < len; i++) {
        char cstr[2] = {status.label[i], 0};
        sLabelSpr.drawString(cstr, cx, 0);
        cx += (6 + kKernPx);
        if (cx >= (sLabelSpr.width() - 6)) break;
      }

      const int dstW = srcH;
      const int dstH = srcW;
      const int dx0 = (ax + barX) + max(0, (barW - dstW) / 2);
      const int dy0 = metricsY + max(0, (kPulseOxMetricsH - dstH) / 2);

      for (int sy = 0; sy < srcH; sy++) {
        for (int sx = 0; sx < srcW; sx++) {
          const uint16_t c = sLabelSpr.readPixel(sx, sy);
          if (c == status.color) continue;
          const int dx = dx0 + sy;
          const int dy = dy0 + (srcW - 1 - sx);
          if (dx < (ax + barX) || dx >= (ax + barX + barW) || dy < metricsY || dy >= (metricsY + kPulseOxMetricsH)) continue;
          tft.drawPixel(dx, dy, c);
        }
      }
    }
  }

  auto drawMetric = [&](int y, const char* label, const String& value) {
    if (spr) {
      spr->setTextDatum(TL_DATUM);
      spr->setTextSize(1);
      spr->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      spr->drawString(label, 2, y);

      spr->setTextSize(2);
      spr->setTextColor(TFT_WHITE, TFT_BLACK);
      spr->drawString(value, 2, y + 14);
    } else {
      tft.setTextDatum(TL_DATUM);
      tft.setTextSize(1);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString(label, ax + 2, metricsY + y);

      tft.setTextSize(2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(value, ax + 2, metricsY + y + 14);
    }
  };

  char buf[16];
  snprintf(buf, sizeof(buf), "%.0f", spo2);
  drawMetric(0, "spo2", String(buf));

  snprintf(buf, sizeof(buf), "%.0f", hr);
  drawMetric(32, "hr", String(buf));

  snprintf(buf, sizeof(buf), "%.1f", pi);
  drawMetric(64, "pi", String(buf));

  if (spr) {
    // Use the calibrated swap-bytes setting for 16-bit sprite blits.
    tft.setSwapBytes(kSwapBytesForPushImage);
    spr->pushSprite(ax, metricsY);
  } else {
    tft.endWrite();
  }
}

static inline bool isNearWhiteRgb(uint32_t argb, uint8_t thresh) {
  const uint8_t r = (uint8_t)((argb >> 16) & 0xFF);
  const uint8_t g = (uint8_t)((argb >> 8) & 0xFF);
  const uint8_t b = (uint8_t)((argb >> 0) & 0xFF);
  return (r >= thresh && g >= thresh && b >= thresh);
}

static inline bool rgbExactly(uint32_t a, uint32_t b) {
  return ((a ^ b) & 0x00FFFFFFu) == 0;
}

static std::unique_ptr<rlottie::Animation> gAnim;
static uint32_t* gFrameArgb = nullptr;
static uint16_t* gFrame565 = nullptr;
static uint16_t* gTest565 = nullptr;
static uint32_t gAnimStartMs = 0;
static double gAnimFps = 30.0;
static size_t gAnimTotalFrames = 0;
static bool gLottieReady = false;
static uint32_t gLastLottieInitAttemptMs = 0;
static TaskHandle_t gRenderTask = nullptr;
static bool gOverlayDirty = true;

// Offscreen overlay sprite (used only during reveal animation to avoid visible
// "clear then redraw" flicker).
static TFT_eSprite gOverlaySprite = TFT_eSprite(&tft);
static bool gOverlaySpriteOk = false;
static void initOverlaySpriteOnce() {
  if (gOverlaySpriteOk) return;
  gOverlaySprite.setColorDepth(16);
  // createSprite() returns nullptr on failure.
  gOverlaySpriteOk = (gOverlaySprite.createSprite(kPanelW, kOverlayH) != nullptr);
  if (gOverlaySpriteOk) {
    gOverlaySprite.fillSprite(TFT_BLACK);
  }
}

// Post-reveal "juice": expand ketosis label background to full-screen.
// Sequence: expand (0.5s) -> hold (2s) -> contract (0.5s) -> done.
static TFT_eSprite gPanelFxSprite = TFT_eSprite(&tft);
static bool gPanelFxSpriteOk = false;
static void initPanelFxSpriteOnce() {
  if (gPanelFxSpriteOk) return;
  gPanelFxSprite.setColorDepth(16);
  gPanelFxSpriteOk = (gPanelFxSprite.createSprite(kPanelW, kPanelH) != nullptr);
  if (gPanelFxSpriteOk) {
    gPanelFxSprite.fillSprite(TFT_BLACK);
  }
}

// ---------- emitStaticScreenTrace (body – after gPanelFxSprite) ----------
static void emitStaticScreenTrace() {
  if (!fbTraceGateActive()) return;
  initPanelFxSpriteOnce();
  if (!gPanelFxSpriteOk) return;

  uint8_t traceBuf[kFbTraceW * kFbTraceH];
  for (int ty = 0; ty < kFbTraceH; ty++) {
    const int sy = (ty * kPanelH) / kFbTraceH;
    for (int tx = 0; tx < kFbTraceW; tx++) {
      const int sx = (tx * kPanelW) / kFbTraceW;
      const uint16_t px = gPanelFxSprite.readPixel(sx, sy);
      traceBuf[ty * kFbTraceW + tx] = mainTracePaletteIndex(px);
    }
  }

  fbTraceEmit(traceBuf, sMainTraceSeq);
}

enum LabelFxState {
  kLabelFxNone = 0,
  kLabelFxExpand = 1,
  kLabelFxHold = 2,
};

static volatile uint8_t gLabelFxState = kLabelFxNone;
static volatile uint32_t gLabelFxPhaseStartMs = 0;

static inline bool isLabelFxActive() {
  return gLabelFxState != kLabelFxNone;
}

static void startLabelFxIfPossible() {
  if (gKetosisLabel.length() == 0) return;
  if (isLabelFxActive()) return;
  gLabelFxState = kLabelFxExpand;
  gLabelFxPhaseStartMs = millis();
  gOverlayDirty = true;
}

// Overlay reveal animation (slide in from left) when real readings arrive.
static volatile bool gOverlayRevealActive = false;
static volatile uint32_t gOverlayRevealStartMs = 0;
static void startOverlayReveal() {
  gOverlayRevealStartMs = millis();
  gOverlayRevealActive = true;
  gLabelFxState = kLabelFxNone;
  gOverlayDirty = true;
  gStatusStripDirty = true;
}

static void setBleConnected(bool v) {
  if (gBleConnected == v) return;
  gBleConnected = v;
  gStatusStripDirty = true;
}

static TaskHandle_t gWifiTask = nullptr;
static TaskHandle_t gGkTask = nullptr;

static bool gShowLoading = false;
static bool gShowNoWifi = false;
static bool gShowLowBattery = false;
static bool gShowPulseOx = false;
static bool gShowGoodbye = false;
static String gTopStatus = "";

// GOODBYE screen animation.
// Phase flow: confetti falls -> banner expands -> done (optionally triggers deep sleep).
enum GoodbyePhase : uint8_t {
  kGoodbyeNone = 0,
  kGoodbyeConfetti = 1,
  kGoodbyeBannerExpand = 2,
  kGoodbyeDone = 3,
};

static volatile uint8_t gGoodbyePhase = kGoodbyeNone;
static volatile uint32_t gGoodbyePhaseStartMs = 0;
static volatile uint32_t gGoodbyeAnimStartMs = 0;
static volatile uint32_t gGoodbyeBannerStartMs = 0;
static volatile bool gGoodbyeAnimDone = false;
static volatile bool gGoodbyeSleepAfterAnim = false;

// Normal (non-demo) UI control: a background task can request which visual stage
// should be shown. renderTask() applies the stage changes so RLottie calls are
// serialized on the render task.
static portMUX_TYPE gUiMux = portMUX_INITIALIZER_UNLOCKED;
static volatile int gUiRequestedStep = 4;  // default: LOADING
static volatile bool gUiRequestedStepDirty = true;



static volatile bool gForceNoWifiScreen = false;
static char gPortalSsid[64] = "ble-health-hub";
static char gPortalQrPayload[160] = {0};

static void wifiQrEscape(const char* in, char* out, size_t outSize) {
  if (outSize == 0) return;
  size_t j = 0;
  for (size_t i = 0; in && in[i] != '\0'; i++) {
    const char ch = in[i];
    const bool esc = (ch == '\\' || ch == ';' || ch == ',' || ch == ':');
    if (esc) {
      if (j + 2 >= outSize) break;
      out[j++] = '\\';
      out[j++] = ch;
    } else {
      if (j + 1 >= outSize) break;
      out[j++] = ch;
    }
  }
  out[j] = '\0';
}

static void updatePortalQrPayload() {
  // Encode a WiFi QR that joins the WiFiManager AP (usually open / nopass).
  // Format: WIFI:T:nopass;S:<ssid>;;
  char ssidEsc[96] = {0};
  wifiQrEscape(gPortalSsid, ssidEsc, sizeof(ssidEsc));
  snprintf(gPortalQrPayload, sizeof(gPortalQrPayload), "WIFI:T:nopass;S:%s;;", ssidEsc);
}

static void onWmApStarted(WiFiManager* wm) {
  if (!wm) return;
  const String ssid = wm->getConfigPortalSSID();
  if (ssid.length() > 0) {
    snprintf(gPortalSsid, sizeof(gPortalSsid), "%s", ssid.c_str());
    updatePortalQrPayload();
  }
  gForceNoWifiScreen = true;
  gOverlayDirty = true;
  Serial.printf("WIFI: portal started ssid='%s'\n", gPortalSsid);
}

static SleepPolicyState computeSleepPolicyState() {
  // Never sleep while demo stages are playing.
  if (kDemoMode && isAnyDemoGateActive()) {
    return kSleepDisabled;
  }

  // Waiting for WiFi setup: WiFiManager portal has started and we show QR.
  if (gForceNoWifiScreen) {
    return kSleepWaitingWifiSetup;
  }

  // Determine current UI step (best-effort snapshot).
  int req = 4;
  portENTER_CRITICAL(&gUiMux);
  req = gUiRequestedStep;
  portEXIT_CRITICAL(&gUiMux);

  // Waiting for BLE connection: CONNECTING stage with BLE not connected.
  if (req == 4 && !gBleConnected) {
    return kSleepWaitingBle;
  }

  // Showing readings to the user.
  if (req == 7 || req == 0 || req == 1 || req == 2 || req == 3 || req == 6) {
    return kSleepShowingReading;
  }

  return kSleepNone;
}

static uint32_t sleepTimeoutForPolicyState(SleepPolicyState st) {
  switch (st) {
    case kSleepWaitingWifiSetup: return kSleepAfterWifiSetupMs;
    case kSleepWaitingBle: return kSleepAfterBleWaitMs;
    case kSleepShowingReading: return kSleepAfterShowingReadingMs;
    default: return 0;
  }
}

static void configureTouchWakeIfNeeded() {
  if (gTouchWakeConfigured) return;

#if __has_include(<esp_sleep.h>) && __has_include(<esp32-hal-touch.h>)
#if SOC_TOUCH_SENSOR_NUM > 0
  // Prime touch hardware and estimate a baseline.
  // NOTE: Arduino-ESP32 semantics differ by SoC:
  // - ESP32: touchRead falls when touched; threshold is an absolute raw value.
  // - ESP32-S2/S3: touchRead rises when touched; threshold is an increment.
  touch_value_t baseline = 0;
  touch_value_t maxSeen = 0;
  {
    // On touch wake, the user may still be touching the pad during boot.
    // Use a settle window and treat baseline as the *minimum* observed value
    // (best proxy for "untouched"), so we don't calibrate against a touched pad.
    uint32_t settleMs = 250;
#if __has_include(<esp_sleep.h>)
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TOUCHPAD) {
      settleMs = 1200;
    }
#endif
    touch_value_t minSeen = (touch_value_t)~(touch_value_t)0;
    const uint32_t t0 = millis();
    while ((millis() - t0) < settleMs) {
      const touch_value_t v = touchRead((uint8_t)kWakeTouchGpio);
      if (v < minSeen) minSeen = v;
      if (v > maxSeen) maxSeen = v;
      delay(5);
    }
    baseline = (minSeen == (touch_value_t)~(touch_value_t)0) ? (touch_value_t)0 : minSeen;
  }

  touch_value_t thr = (touch_value_t)WAKE_TOUCH_THRESHOLD;

  if (thr == (touch_value_t)0) {
#if defined(SOC_TOUCH_VERSION_2)
    // ESP32-S2/S3: Arduino's touchSleepWakeUpEnable() ultimately calls
    // touch_pad_sleep_set_threshold(), which expects an *absolute* threshold.
    // We treat our threshold as an *increment* and convert: abs = baseline + inc.
    // Make the increment noise-aware to avoid spurious wakes if the pad is floating.
    const touch_value_t noiseDelta = (maxSeen > baseline) ? (touch_value_t)(maxSeen - baseline) : (touch_value_t)0;

    // Heuristic: require a bump comfortably above observed noise.
    // - at least 1/8 of baseline
    // - at least 4x the observed noise delta
    // - at least a small absolute floor
    touch_value_t incA = (touch_value_t)(baseline / (touch_value_t)8);
    touch_value_t incB = (touch_value_t)(noiseDelta * (touch_value_t)4);
    touch_value_t incC = (touch_value_t)300;  // floor
    touch_value_t inc = incA;
    if (incB > inc) inc = incB;
    if (incC > inc) inc = incC;
    if (inc < (touch_value_t)1) inc = (touch_value_t)1;

    // If caller provided WAKE_TOUCH_THRESHOLD, treat it as an increment.
    if ((touch_value_t)WAKE_TOUCH_THRESHOLD != (touch_value_t)0) {
      inc = (touch_value_t)WAKE_TOUCH_THRESHOLD;
    }

    // Convert to absolute threshold.
    const uint32_t thrAbs32 = (uint32_t)baseline + (uint32_t)inc;
    thr = (touch_value_t)thrAbs32;

    Serial.printf("SLEEP: touch noise baseline=%u max=%u noiseDelta=%u inc=%u thrAbs=%u\n",
            (unsigned)baseline, (unsigned)maxSeen, (unsigned)noiseDelta, (unsigned)inc, (unsigned)thrAbs32);
#else
    // ESP32: threshold is an absolute raw value (lower triggers).
    thr = (touch_value_t)((uint64_t)baseline * 80u / 100u);  // 80% of baseline
    if (thr < (touch_value_t)1) thr = (touch_value_t)1;
#endif
  }

  gTouchWakeBaseline = (uint32_t)baseline;
  gTouchWakeThreshold = (uint32_t)thr;
  gTouchWakeThresholdAbs = (uint32_t)thr;
  touchSleepWakeUpEnable((uint8_t)kWakeTouchGpio, (touch_value_t)thr);
  (void)esp_sleep_enable_touchpad_wakeup();
  gTouchWakeConfigured = true;
  Serial.printf("SLEEP: touch wake enabled gpio=%d baseline=%u thr=%u\n", kWakeTouchGpio, (unsigned)baseline, (unsigned)thr);
#else
  Serial.println("SLEEP: touch sensors not supported on this target");
  gTouchWakeConfigured = true;
#endif
#else
  Serial.println("SLEEP: touch wake not available in this build");
  gTouchWakeConfigured = true;
#endif
}

static bool readTouchActiveNow() {
#if __has_include(<esp32-hal-touch.h>)
#if SOC_TOUCH_SENSOR_NUM > 0
  const touch_value_t v = touchRead((uint8_t)kWakeTouchGpio);
#if defined(SOC_TOUCH_VERSION_2)
  // ESP32-S2/S3: higher value tends to indicate touch.
  const uint32_t thrAbs = gTouchWakeThresholdAbs;
  if (thrAbs == 0) return false;
  return (uint32_t)v >= thrAbs;
#else
  // ESP32: lower value tends to indicate touch.
  const uint32_t thrAbs = gTouchWakeThresholdAbs;
  if (thrAbs == 0) return false;
  return (uint32_t)v <= thrAbs;
#endif
#else
  return false;
#endif
#else
  return false;
#endif
}

static void handleTouchPress() {
  if (kDemoMode && isAnyDemoGateActive()) return;

  // Snapshot UI step.
  int req = 4;
  portENTER_CRITICAL(&gUiMux);
  req = gUiRequestedStep;
  portEXIT_CRITICAL(&gUiMux);

  // 1) If waiting for BLE connection, a touch forces sleep.
  if (computeSleepPolicyState() == kSleepWaitingBle) {
    enterDeepSleepNow("touch sleep");
    return;
  }

  // 2/3) If results are displayed, disconnect + block reconnection until reboot/wake.
  if (req == 7) {
    // Pulse Ox results.
    gBlockPulseOxConnections = true;
    gPulseOxDisconnectRequested = true;
    Serial.println("TOUCH: block PulseOx + request disconnect");
    // Return to connecting screen after blocking.
    requestUiStep(4);
    return;
  }

  if (req == 0 || req == 1 || req == 2 || req == 3 || req == 6) {
    // GK+ results.
    gBlockGkPlusConnections = true;
    gGkDisconnectRequested = true;
    Serial.println("TOUCH: block GK+ + request disconnect");
    // Return to connecting screen after blocking.
    requestUiStep(4);
    return;
  }
}

static void enterDeepSleepNow(const char* why) {
#if __has_include(<esp_sleep.h>)
  Serial.printf("SLEEP: entering deep sleep (%s)\n", why ? why : "");
  Serial.flush();

  // Minimal power-down prep.
  setBacklight(false);
#ifdef TFT_RST
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
#endif

  // Stop Wi-Fi to reduce draw before sleep.
  // IMPORTANT: do NOT erase stored credentials here.
  // `eraseap=true` would clear the STA config in NVS, preventing reconnect on wake.
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  // Ensure wake source is armed.
  configureTouchWakeIfNeeded();

  esp_deep_sleep_start();
#else
  (void)why;
#endif
}

static void drawQrCodeFromText(const char* text, int x0, int y0, int scale, int quiet, TFT_eSprite* spr) {
  if (!text || text[0] == '\0') return;
  static constexpr int kQrVersion = 3;
  static constexpr int kQrSize = 4 * kQrVersion + 17;  // 29
  static constexpr int kQrBufBytes = ((kQrSize * kQrSize) + 7) / 8;  // 106
  static uint8_t qrcodeData[kQrBufBytes];
  QRCode qr;
  if (qrcode_initText(&qr, qrcodeData, kQrVersion, ECC_LOW, text) < 0) {
    // Too much data for the chosen version; clear area and bail.
    const int px = (kQrSize + 2 * quiet) * scale;
    tft.fillRect(x0, y0, px, px, TFT_BLACK);
    if (spr) spr->fillRect(x0, y0, px, px, TFT_BLACK);
    return;
  }

  const int size = qr.size;
  const int px = (size + 2 * quiet) * scale;
  tft.fillRect(x0, y0, px, px, TFT_BLACK);
  if (spr) spr->fillRect(x0, y0, px, px, TFT_BLACK);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        tft.fillRect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale, scale, TFT_WHITE);
        if (spr) {
          spr->fillRect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale, scale, TFT_WHITE);
        }
      }
    }
  }
}

// RTC Wi-Fi cache: persists across reset/deep sleep (but not power loss).
// Purpose: speed up Wi-Fi association by pinning the last known AP channel and
// BSSID, and optionally pre-setting last IP config.
struct RtcWifiCache {
  uint32_t magic;
  uint32_t checksum;
  uint8_t bssid[6];
  uint8_t channel;
  uint8_t reserved[1];

  uint32_t ip;
  uint32_t gateway;
  uint32_t subnet;
  uint32_t dns1;
  uint32_t dns2;
};

static constexpr uint32_t kRtcWifiMagic = 0x57494649u; // 'WIFI'
RTC_DATA_ATTR static RtcWifiCache gRtcWifiCache;

static uint32_t rtcWifiChecksum(const RtcWifiCache& c) {
  // Simple, deterministic checksum (not cryptographic).
  uint32_t x = 0xA5A5F00Du;
  x ^= c.magic;
  x ^= (uint32_t)c.channel;
  for (int i = 0; i < 6; i++) {
    x = (x << 5) ^ (x >> 27) ^ (uint32_t)c.bssid[i];
  }
  x ^= c.ip;
  x ^= c.gateway;
  x ^= c.subnet;
  x ^= c.dns1;
  x ^= c.dns2;
  return x;
}

static bool rtcWifiCacheValid() {
  if (gRtcWifiCache.magic != kRtcWifiMagic) return false;
  const uint32_t want = rtcWifiChecksum(gRtcWifiCache);
  return want == gRtcWifiCache.checksum;
}

static void rtcWifiCacheClear() {
  memset(&gRtcWifiCache, 0, sizeof(gRtcWifiCache));
}

static void rtcWifiCacheUpdateFromCurrent() {
  if (WiFi.status() != WL_CONNECTED) return;

  RtcWifiCache next{};
  next.magic = kRtcWifiMagic;

  const uint8_t* b = WiFi.BSSID();
  if (b) {
    memcpy(next.bssid, b, 6);
  }
  const int32_t ch = WiFi.channel();
  next.channel = (uint8_t)max<int32_t>(0, min<int32_t>(255, ch));

  next.ip = (uint32_t)WiFi.localIP();
  next.gateway = (uint32_t)WiFi.gatewayIP();
  next.subnet = (uint32_t)WiFi.subnetMask();
  next.dns1 = (uint32_t)WiFi.dnsIP(0);
  next.dns2 = (uint32_t)WiFi.dnsIP(1);

  next.checksum = rtcWifiChecksum(next);
  gRtcWifiCache = next;

  Serial.printf("WIFI: cached ip=%s bssid=%s ch=%d\n",
                WiFi.localIP().toString().c_str(),
                WiFi.BSSIDstr().c_str(),
                (int)next.channel);
}

static bool readStoredStaConfig(String* outSsid, String* outPass) {
#if __has_include(<esp_wifi.h>)
  wifi_config_t conf{};
  const esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &conf);
  if (err != ESP_OK) {
    return false;
  }
  const char* ssid = (const char*)conf.sta.ssid;
  const char* pass = (const char*)conf.sta.password;
  if (!ssid || ssid[0] == '\0') {
    return false;
  }
  if (outSsid) *outSsid = String(ssid);
  if (outPass) *outPass = pass ? String(pass) : String("");
  return true;
#else
  (void)outSsid;
  (void)outPass;
  return false;
#endif
}

static bool waitForWifiConnected(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while ((millis() - t0) < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return (WiFi.status() == WL_CONNECTED);
}

static bool tryFastConnectFromRtc(uint32_t timeoutMs) {
  if (!rtcWifiCacheValid()) return false;

  String ssid;
  String pass;
  if (!readStoredStaConfig(&ssid, &pass)) {
    Serial.println("WIFI: RTC cache present but no stored SSID (skipping fast connect)");
    return false;
  }

  const uint8_t ch = gRtcWifiCache.channel;
  const bool hasBssid = (gRtcWifiCache.bssid[0] | gRtcWifiCache.bssid[1] | gRtcWifiCache.bssid[2] |
                         gRtcWifiCache.bssid[3] | gRtcWifiCache.bssid[4] | gRtcWifiCache.bssid[5]) != 0;

  Serial.printf("WIFI: fast connect ssid='%s' ch=%u bssid=%s\n",
                ssid.c_str(), (unsigned)ch, hasBssid ? "yes" : "no");

  // Optional: reuse last IP config to avoid DHCP round-trip.
  // Only apply if we have the required fields.
  const bool hasIpCfg = (gRtcWifiCache.ip != 0) && (gRtcWifiCache.gateway != 0) && (gRtcWifiCache.subnet != 0);
  bool appliedStaticIp = false;
  if (hasIpCfg) {
    WiFi.config(IPAddress(gRtcWifiCache.ip), IPAddress(gRtcWifiCache.gateway), IPAddress(gRtcWifiCache.subnet),
                IPAddress(gRtcWifiCache.dns1), IPAddress(gRtcWifiCache.dns2));
    appliedStaticIp = true;
  }

  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr, (int32_t)ch,
             hasBssid ? gRtcWifiCache.bssid : nullptr, true);

  if (!waitForWifiConnected(timeoutMs)) {
    Serial.println("WIFI: fast connect failed");

    // IMPORTANT: WiFi.config() flips an internal WiFiSTAClass flag that disables
    // DHCP on subsequent WiFi.begin() calls. If we tried a cached static IP and
    // it didn't work, revert to DHCP before the next connect attempt.
    if (appliedStaticIp) {
      struct WiFiStaHack : public WiFiSTAClass {
        static void setUseStaticIp(bool v) { _useStaticIp = v; }
      };
      WiFiStaHack::setUseStaticIp(false);
    }

    return false;
  }

  Serial.printf("WIFI: connected (fast) ip=%s\n", WiFi.localIP().toString().c_str());
  rtcWifiCacheUpdateFromCurrent();
  return true;
}

static void wifiTask(void* /*param*/) {
  Serial.println("WIFI: task start");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);

  // Prefer fast connect with cached channel+BSSID.
  if (tryFastConnectFromRtc(5000)) {
    Serial.println("WIFI: fast connect OK");
    vTaskDelete(nullptr);
    return;
  }

  // Next: normal connect using stored credentials (scan).
  String storedSsid;
  String storedPass;
  if (readStoredStaConfig(&storedSsid, &storedPass)) {
    Serial.printf("WIFI: begin() using stored credentials ssid='%s'\n", storedSsid.c_str());
    WiFi.begin();
    if (waitForWifiConnected(8000)) {
      Serial.printf("WIFI: connected ip=%s\n", WiFi.localIP().toString().c_str());
      rtcWifiCacheUpdateFromCurrent();
      vTaskDelete(nullptr);
      return;
    }
    Serial.println("WIFI: stored-credential connect failed");
  } else {
    Serial.println("WIFI: no stored credentials; skipping WiFi.begin()");
  }

  // Fallback: WiFiManager portal.
  Serial.println("WIFI: launching WiFiManager portal");
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  WiFiManager wm;
  wm.setAPCallback(onWmApStarted);
  wm.setCaptivePortalEnable(true);
  wm.setWiFiAutoReconnect(true);
  wm.setCleanConnect(true);
  wm.setConnectTimeout(30);
  wm.setSaveConnectTimeout(30);
  wm.setConfigPortalBlocking(false);
  // Keep portal name stable so your phone remembers it.
  const bool started = wm.startConfigPortal("ble-health-hub");
  if (!started && !wm.getConfigPortalActive()) {
    Serial.println("WIFI: failed to start WiFiManager portal");
    vTaskDelete(nullptr);
    return;
  }

  while (wm.getConfigPortalActive()) {
    const bool connected = wm.process();
    if (connected || WiFi.status() == WL_CONNECTED) {
      Serial.printf("WIFI: WiFiManager connected ip=%s\n", WiFi.localIP().toString().c_str());
      rtcWifiCacheUpdateFromCurrent();
      gForceNoWifiScreen = false;
      gOverlayDirty = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WIFI: WiFiManager portal exited without connection");
  }

  vTaskDelete(nullptr);
}

static void requestUiStep(int step) {
  portENTER_CRITICAL(&gUiMux);
  if (gUiRequestedStep != step) {
    gUiRequestedStep = step;
    gUiRequestedStepDirty = true;
  }
  portEXIT_CRITICAL(&gUiMux);
}

static float computeShownGki(float glucoseMgDl, float ketoneMmolL) {
  // Match web/official displayed GKI behavior by computing from the rounded
  // displayed inputs, not the raw decoded floats.
  const float shownGluMgDl = roundf(glucoseMgDl);
  const float shownKetMmolL = floorf((ketoneMmolL * 10.0f) + 0.5f) / 10.0f;
  if (!(shownKetMmolL > 0.0f)) return NAN;
  const float gki = (shownGluMgDl / 18.0f) / shownKetMmolL;
  return floorf(gki * 10.0f) / 10.0f;
}

static int ketosisStageFromGki(float gki) {
  if (!isfinite(gki)) return 4;  // LOADING fallback
  if (gki > 9.0f) return 0;      // NOT KETOSIS
  if (gki >= 6.0f) return 1;     // LOW KETOSIS
  if (gki >= 3.0f) return 2;     // MID KETOSIS
  if (gki >= 1.0f) return 3;     // HIGH KETOSIS
  return 6;                      // !!! KETOSIS
}

static inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  // Howard Hinnant's algorithm: days since 1970-01-01.
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int64_t epochMinutesUtcLike(int year, int month, int day, int hour, int minute) {
  const int64_t days = daysFromCivil(year, (unsigned)month, (unsigned)day);
  return days * 1440 + (int64_t)hour * 60 + (int64_t)minute;
}

struct GkRecord {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int64_t tMin = 0;
  int whenKey = 0;
  uint8_t type = 0;
  uint8_t flags = 0;
  enum Kind : uint8_t { UNKNOWN = 0, GLUCOSE = 1, KETONE = 2 } kind = UNKNOWN;
  enum Prandial : uint8_t { GENERAL = 0, PRE = 1, POST = 2 } prandial = GENERAL;
  int raw = 0;
  float value = NAN;
};

static bool decodeGkRecordSnippet9(const uint8_t* rec9, size_t len, GkRecord* out) {
  if (!rec9 || len != 9 || !out) return false;
  const uint8_t yy = rec9[0];
  const uint8_t mo = rec9[1];
  const uint8_t dd = rec9[2];
  const uint8_t hh = rec9[3];
  const uint8_t mi = rec9[4];
  const int year = 2000 + (int)yy;
  const int whenKey = year * 100000000 + (int)mo * 1000000 + (int)dd * 10000 + (int)hh * 100 + (int)mi;
  const int raw = (int)rec9[5] * 100 + (int)rec9[6];
  const uint8_t flags = rec9[7];
  const uint8_t prNib = (flags >> 4) & 0x0F;
  const uint8_t type = rec9[8];

  GkRecord r;
  r.year = year;
  r.month = (int)mo;
  r.day = (int)dd;
  r.hour = (int)hh;
  r.minute = (int)mi;
  r.tMin = epochMinutesUtcLike(r.year, r.month, r.day, r.hour, r.minute);
  r.whenKey = whenKey;
  r.type = type;
  r.flags = flags;
  r.raw = raw;
  r.prandial = (prNib == 1) ? GkRecord::PRE : (prNib == 2) ? GkRecord::POST : GkRecord::GENERAL;
  r.kind = GkRecord::UNKNOWN;
  r.value = NAN;

  if (type == 0x11 || type == 0x12 || type == 0x22) {
    r.kind = GkRecord::GLUCOSE;
    r.value = (float)raw;  // mg/dL
  } else if (type == 0x55 || type == 0x56 || type == 0x66) {
    r.kind = GkRecord::KETONE;
    r.value = (float)raw / 100.0f;  // mmol/L
  }

  *out = r;
  return true;
}

static uint16_t crc16Modbus(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) crc = (uint16_t)((crc >> 1) ^ 0xA001);
      else crc = (uint16_t)(crc >> 1);
    }
  }
  return crc;
}

static void crcSendBytesFromCrc16(uint16_t crc16, uint8_t out4[4]) {
  // Matches APK: 4 hex digits -> nibble-bytes, then swap pairs.
  const uint8_t n0 = (crc16 >> 12) & 0x0F;
  const uint8_t n1 = (crc16 >> 8) & 0x0F;
  const uint8_t n2 = (crc16 >> 4) & 0x0F;
  const uint8_t n3 = (crc16 >> 0) & 0x0F;
  out4[0] = n2;
  out4[1] = n3;
  out4[2] = n0;
  out4[3] = n1;
}

static void buildLatestRecords16Command(uint16_t param, std::vector<uint8_t>* out) {
  if (!out) return;
  out->clear();

  // Body bytes: 01 10 01 20 16 55 00 02 <param_hi> <param_lo>
  uint8_t body[10] = {0x01, 0x10, 0x01, 0x20, 0x16, 0x55, 0x00, 0x02,
                      (uint8_t)((param >> 8) & 0xFF), (uint8_t)(param & 0xFF)};
  const uint16_t crc = crc16Modbus(body, sizeof(body));
  uint8_t crc4[4] = {0};
  crcSendBytesFromCrc16(crc, crc4);

  out->reserve(1 + sizeof(body) + sizeof(crc4) + 1);
  out->push_back(0x7B);
  out->insert(out->end(), body, body + sizeof(body));
  out->insert(out->end(), crc4, crc4 + sizeof(crc4));
  out->push_back(0x7D);
}

enum TargetDeviceKind {
  kTargetNone = 0,
  kTargetGkPlus = 1,
  kTargetPulseOx = 2,
};

struct BleTargetCandidate {
  TargetDeviceKind kind = kTargetNone;
  NimBLEAddress address{};
  std::string name;
  int8_t rssi = 0;
  bool valid = false;
};

static const NimBLEUUID kPulseOxSvcUuid("56313100-504f-6e2e-6175-696a2e6d6f63");
static const NimBLEUUID kPulseOxSvcUuidAlt("00313156-4f50-2e6e-6175-696a2e6d6f63");
static const NimBLEUUID kPulseOxSvcUuidWireshark("636f6d2e-6a69-7561-6e2e-504f56313100");
static uint32_t gPulseOxReconnectAllowedAtMs = 0;

static bool isPulseOxReconnectCooldownActive() {
  return (int32_t)(millis() - gPulseOxReconnectAllowedAtMs) < 0;
}

static void startPulseOxReconnectCooldown(uint32_t cooldownMs) {
  gPulseOxReconnectAllowedAtMs = millis() + cooldownMs;
}

static bool advLooksLikeGkPlus(const NimBLEAdvertisedDevice& d) {
  // Most reliable observed filter: Service Data (AD type 0x16) with UUID 0x78ac.
  // As a fallback, accept devices whose name includes "Keto-Mojo".
  const std::string name = d.getName();
  if (!name.empty() && name.find("Keto-Mojo") != std::string::npos) {
    return true;
  }

  const int n = d.getServiceDataCount();
  for (int i = 0; i < n; i++) {
    const NimBLEUUID u = d.getServiceDataUUID(i);
    if (u.equals(NimBLEUUID((uint16_t)0x78ac)) || u.equals(NimBLEUUID((uint16_t)0xac78))) {
      return true;
    }
  }
  return false;
}

static bool advLooksLikePulseOx(const NimBLEAdvertisedDevice& d) {
  const std::string name = d.getName();
  if (!name.empty()) {
    if (name.find("Pulse") != std::string::npos || name.find("Oximeter") != std::string::npos ||
        name.find("PO3") != std::string::npos) {
      return true;
    }
  }

  if (d.isAdvertisingService(kPulseOxSvcUuid) || d.isAdvertisingService(kPulseOxSvcUuidAlt) ||
      d.isAdvertisingService(kPulseOxSvcUuidWireshark)) {
    return true;
  }

  const uint8_t serviceCount = d.getServiceUUIDCount();
  for (uint8_t i = 0; i < serviceCount; i++) {
    const NimBLEUUID uuid = d.getServiceUUID(i);
    if (uuid.equals(kPulseOxSvcUuid) || uuid.equals(kPulseOxSvcUuidAlt) || uuid.equals(kPulseOxSvcUuidWireshark)) {
      return true;
    }
  }

  return false;
}

static TargetDeviceKind classifyAdvertisedDevice(const NimBLEAdvertisedDevice& d) {
  if (advLooksLikePulseOx(d)) return kTargetPulseOx;
  if (advLooksLikeGkPlus(d)) return kTargetGkPlus;
  return kTargetNone;
}

class TargetScanCallbacks : public NimBLEScanCallbacks {
 public:
  explicit TargetScanCallbacks(BleTargetCandidate* out) : out_(out) {}

  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (!out_ || !advertisedDevice || out_->valid) return;
    const TargetDeviceKind kind = classifyAdvertisedDevice(*advertisedDevice);
    if (kind == kTargetNone) return;
    if (kind == kTargetPulseOx && isPulseOxReconnectCooldownActive()) return;

    out_->kind = kind;
    out_->address = advertisedDevice->getAddress();
    out_->name = advertisedDevice->getName();
    out_->rssi = advertisedDevice->getRSSI();
    out_->valid = true;
    advertisedDevice->getScan()->stop();
  }

 private:
  BleTargetCandidate* out_ = nullptr;
};

static bool scanForSupportedTarget(NimBLEScan* scan, BleTargetCandidate* out) {
  if (!scan || !out) return false;
  *out = BleTargetCandidate{};

  TargetScanCallbacks callbacks(out);
  scan->clearResults();
  scan->setScanCallbacks(&callbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(15);
  scan->setDuplicateFilter(1);
  scan->setMaxResults(0);
  (void)scan->getResults(5000, false);
  scan->clearResults();
  return out->valid;
}

struct PulseOxPrompt {
  bool valid = false;
  uint8_t tail1 = 0;
  uint8_t tail2 = 0;
  bool isA006 = false;
};

struct PulseOxReassembly {
  bool active = false;
  uint8_t totalFragments = 0;
  uint8_t opcode = 0;
  bool opcodeValid = false;
  bool havePart[8] = {false};
  uint8_t partLen[8] = {0};
  uint8_t parts[8][16] = {{0}};
};

struct PulseOxSession {
  NimBLERemoteCharacteristic* writer = nullptr;
  bool writeWithResponse = false;
  bool handshakeRunning = false;
  bool handshakeComplete = false;
  bool stage5WasA006 = false;
  uint8_t txSeq = 1;
  bool ackSeen[256] = {false};
  PulseOxPrompt promptByBase[256];
  uint8_t vendorRxBuf[512] = {0};
  size_t vendorRxLen = 0;
  PulseOxReassembly reassembly;
  bool identifyReady = false;
  uint8_t identifyOpcode = 0;
  uint8_t identifyPayload[48] = {0};
  float lastGoodPi = 0.0f;
  bool gotMeasurement = false;
  bool firstMeasurement = false;
  uint32_t lastFrameAtMs = 0;
  float spo2 = 0.0f;
  float hr = 0.0f;
  float pi = 0.0f;
  uint8_t strength = 0;
  uint16_t wave[3] = {0, 0, 0};
};

static portMUX_TYPE gPulseOxMux = portMUX_INITIALIZER_UNLOCKED;
static PulseOxSession* gPulseOxSession = nullptr;
static constexpr uint8_t kPulseOxIdentifyCmd = 0xAC;
static constexpr uint8_t kPulseOxIdentifyStage1Marker = 0xFA;
static constexpr uint8_t kPulseOxIdentifyStage2Marker = 0xFC;
static const uint8_t kPulseOxIdentifyFBytes[16] = {'C', 'h', '/', 'H', 'Q', '4', 'L', 'z', 'I', 't', 'Y', 'T', '4', '2', 's', '='};
static const uint8_t kPulseOxKeyBlob16[16] = {
    0xbf, 0x4b, 0x05, 0x11, 0x42, 0xd2, 0x70, 0xa3,
    0x93, 0x2d, 0x2d, 0xaa, 0xcc, 0xa9, 0xbd, 0x1e,
};

static void pulseoxSetPrompt(PulseOxSession* sess, uint8_t base, uint8_t tail1, uint8_t tail2, bool isA006) {
  if (!sess) return;
  portENTER_CRITICAL(&gPulseOxMux);
  sess->promptByBase[base].valid = true;
  sess->promptByBase[base].tail1 = tail1;
  sess->promptByBase[base].tail2 = tail2;
  sess->promptByBase[base].isA006 = isA006;
  portEXIT_CRITICAL(&gPulseOxMux);
}

static void pulseoxMarkAckSeen(PulseOxSession* sess, uint8_t step) {
  if (!sess) return;
  portENTER_CRITICAL(&gPulseOxMux);
  sess->ackSeen[step] = true;
  portEXIT_CRITICAL(&gPulseOxMux);
}

static void pulseoxStoreIdentifyPayload(PulseOxSession* sess, uint8_t opcode, const uint8_t payload[48]) {
  if (!sess || !payload) return;
  portENTER_CRITICAL(&gPulseOxMux);
  memcpy(sess->identifyPayload, payload, 48);
  sess->identifyOpcode = opcode;
  sess->identifyReady = true;
  portEXIT_CRITICAL(&gPulseOxMux);
}

static void pulseoxSetLiveMeasurement(PulseOxSession* sess, float spo2, float hr, float pi, uint8_t strength, const uint16_t wave[3]) {
  if (!sess) return;
  portENTER_CRITICAL(&gPulseOxMux);
  sess->spo2 = spo2;
  sess->hr = hr;
  sess->pi = pi;
  sess->strength = strength;
  if (wave) {
    sess->wave[0] = wave[0];
    sess->wave[1] = wave[1];
    sess->wave[2] = wave[2];
  }
  sess->gotMeasurement = true;
  sess->firstMeasurement = true;
  sess->lastFrameAtMs = millis();
  portEXIT_CRITICAL(&gPulseOxMux);

  pulseox_demo_lvgl_set_live_readings(spo2, hr, pi, strength);
}

static bool pulseoxConsumeAck(PulseOxSession* sess, uint8_t step) {
  if (!sess) return false;
  bool seen = false;
  portENTER_CRITICAL(&gPulseOxMux);
  seen = sess->ackSeen[step];
  if (seen) sess->ackSeen[step] = false;
  portEXIT_CRITICAL(&gPulseOxMux);
  return seen;
}

static bool pulseoxWaitForAck(PulseOxSession* sess, uint8_t step, uint32_t timeoutMs) {
  const uint32_t startMs = millis();
  while ((millis() - startMs) < timeoutMs) {
    if (pulseoxConsumeAck(sess, step)) return true;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return false;
}

static bool pulseoxWaitForAckAny(PulseOxSession* sess, const uint8_t* steps, size_t count, uint8_t* outStep, uint32_t timeoutMs) {
  if (!sess || !steps || count == 0) return false;
  const uint32_t startMs = millis();
  while ((millis() - startMs) < timeoutMs) {
    for (size_t i = 0; i < count; i++) {
      if (pulseoxConsumeAck(sess, steps[i])) {
        if (outStep) *outStep = steps[i];
        return true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return false;
}

static bool pulseoxWaitForPrompt(PulseOxSession* sess, uint8_t base, PulseOxPrompt* out, uint32_t timeoutMs) {
  if (!sess || !out) return false;
  const uint32_t startMs = millis();
  while ((millis() - startMs) < timeoutMs) {
    bool havePrompt = false;
    portENTER_CRITICAL(&gPulseOxMux);
    havePrompt = sess->promptByBase[base].valid;
    if (havePrompt) {
      *out = sess->promptByBase[base];
      sess->promptByBase[base].valid = false;
    }
    portEXIT_CRITICAL(&gPulseOxMux);
    if (havePrompt) return true;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return false;
}

static bool pulseoxWaitForIdentifyChallenge(PulseOxSession* sess, uint8_t* outOpcode, uint8_t outPayload48[48], uint32_t timeoutMs) {
  if (!sess || !outPayload48) return false;
  const uint32_t startMs = millis();
  while ((millis() - startMs) < timeoutMs) {
    bool ready = false;
    uint8_t opcode = 0;
    portENTER_CRITICAL(&gPulseOxMux);
    ready = sess->identifyReady;
    if (ready) {
      opcode = sess->identifyOpcode;
      memcpy(outPayload48, sess->identifyPayload, 48);
      sess->identifyReady = false;
    }
    portEXIT_CRITICAL(&gPulseOxMux);
    if (ready) {
      if (outOpcode) *outOpcode = opcode;
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return false;
}

static void pulseoxFillRandomBytes(uint8_t* out, size_t len) {
  if (!out) return;
  size_t i = 0;
  while (i < len) {
    const uint32_t r = esp_random();
    for (int j = 0; j < 4 && i < len; j++, i++) {
      out[i] = (uint8_t)((r >> (j * 8)) & 0xFF);
    }
  }
}

static void pulseoxIdentifyC16(const uint8_t in16[16], uint8_t out16[16]) {
  for (int i = 0; i < 4; i++) {
    out16[i] = in16[3 - i];
    out16[i + 4] = in16[7 - i];
    out16[i + 8] = in16[11 - i];
    out16[i + 12] = in16[15 - i];
  }
}

static void pulseoxNibbleSwapBytes(const uint8_t* in, uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const uint8_t b = in[i];
    out[i] = (uint8_t)(((b & 0x0F) << 4) | ((b & 0xF0) >> 4));
  }
}

static void pulseoxBytesToWordsBe(const uint8_t* in, size_t len, uint32_t* outWords, size_t wordCount) {
  for (size_t i = 0; i < wordCount; i++) {
    const size_t off = i * 4;
    if ((off + 3) >= len) {
      outWords[i] = 0;
      continue;
    }
    outWords[i] = ((uint32_t)in[off] << 24) | ((uint32_t)in[off + 1] << 16) | ((uint32_t)in[off + 2] << 8) |
                  (uint32_t)in[off + 3];
  }
}

static void pulseoxWordsToBytesBe(const uint32_t* words, size_t wordCount, uint8_t* out) {
  for (size_t i = 0; i < wordCount; i++) {
    const uint32_t v = words[i];
    const size_t off = i * 4;
    out[off] = (uint8_t)((v >> 24) & 0xFF);
    out[off + 1] = (uint8_t)((v >> 16) & 0xFF);
    out[off + 2] = (uint8_t)((v >> 8) & 0xFF);
    out[off + 3] = (uint8_t)(v & 0xFF);
  }
}

static void pulseoxXxteaEncryptBytes(const uint8_t* dataBytes, size_t dataLen, const uint8_t keyBytes16[16], uint8_t* out) {
  if (!dataBytes || !keyBytes16 || !out) return;
  if (dataLen < 8 || (dataLen % 4) != 0) {
    memcpy(out, dataBytes, dataLen);
    return;
  }

  const size_t n = dataLen / 4;
  uint32_t v[16] = {0};
  uint32_t k[4] = {0};
  pulseoxBytesToWordsBe(dataBytes, dataLen, v, n);
  pulseoxBytesToWordsBe(keyBytes16, 16, k, 4);

  int rounds = (int)(52 / n) + 6;
  const uint32_t delta = 1640531527u;
  uint32_t sum = 0;
  uint32_t z = v[n - 1];

  while (rounds-- > 0) {
    sum -= delta;
    const uint32_t e = (sum >> 2) & 3u;
    for (size_t p = 0; p < (n - 1); p++) {
      const uint32_t y = v[p + 1];
      const uint32_t mx = ((((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((y ^ sum) + (z ^ k[(p & 3u) ^ e])));
      v[p] += mx;
      z = v[p];
    }
    const uint32_t y0 = v[0];
    const uint32_t mxLast = ((((z >> 5) ^ (y0 << 2)) + ((y0 >> 3) ^ (z << 4))) ^
                             ((y0 ^ sum) + (z ^ k[((n - 1) & 3u) ^ e])));
    v[n - 1] += mxLast;
    z = v[n - 1];
  }

  pulseoxWordsToBytesBe(v, n, out);
}

static void pulseoxIdentifyGetKa(uint8_t out16[16]) {
  uint8_t swappedKey[16] = {0};
  uint8_t swappedF[16] = {0};
  pulseoxNibbleSwapBytes(kPulseOxKeyBlob16, swappedKey, sizeof(swappedKey));
  pulseoxNibbleSwapBytes(kPulseOxIdentifyFBytes, swappedF, sizeof(swappedF));
  pulseoxXxteaEncryptBytes(swappedKey, sizeof(swappedKey), swappedF, out16);
}

static void pulseoxIdentifyStage1(uint8_t out18[18]) {
  uint8_t random16[16] = {0};
  uint8_t c16[16] = {0};
  pulseoxFillRandomBytes(random16, sizeof(random16));
  for (size_t i = 0; i < sizeof(random16); i++) {
    int s = (random16[i] >= 128) ? ((int)random16[i] - 256) : (int)random16[i];
    if (s < 0) s = -s;
    if (s > 128) s = 128;
    random16[i] = (uint8_t)s;
  }
  pulseoxIdentifyC16(random16, c16);
  out18[0] = kPulseOxIdentifyCmd;
  out18[1] = kPulseOxIdentifyStage1Marker;
  memcpy(out18 + 2, c16, 16);
}

static void pulseoxIdentifyStage2(const uint8_t challenge48[48], uint8_t out18[18]) {
  uint8_t d[16] = {0};
  uint8_t b[16] = {0};
  uint8_t c[16] = {0};
  uint8_t c16[16] = {0};
  uint8_t ka[16] = {0};
  uint8_t t0[16] = {0};
  uint8_t t2[16] = {0};
  uint8_t out16[16] = {0};

  memcpy(d, challenge48, 16);
  memcpy(b, challenge48 + 16, 16);
  memcpy(c, challenge48 + 32, 16);

  pulseoxIdentifyC16(d, c16);
  pulseoxIdentifyGetKa(ka);
  pulseoxXxteaEncryptBytes(c16, sizeof(c16), ka, t0);
  pulseoxIdentifyC16(b, c16);
  pulseoxXxteaEncryptBytes(c16, sizeof(c16), t0, c16);
  pulseoxIdentifyC16(c, c16);
  pulseoxXxteaEncryptBytes(c16, sizeof(c16), t0, t2);
  pulseoxIdentifyC16(t2, out16);

  out18[0] = kPulseOxIdentifyCmd;
  out18[1] = kPulseOxIdentifyStage2Marker;
  memcpy(out18 + 2, out16, 16);
}

static void pulseoxPackageForWrite(const uint8_t* payloadBytes, size_t payloadLen, uint8_t* txSeq,
                                   std::vector<std::vector<uint8_t>>* frames) {
  if (!payloadBytes || payloadLen == 0 || !txSeq || !frames) return;
  frames->clear();

  uint8_t seq = *txSeq;
  if (payloadLen <= 15) {
    const uint8_t lenByte = (uint8_t)((payloadLen + 2) & 0xFF);
    std::vector<uint8_t> frame(payloadLen + 5);
    frame[0] = 0xB0;
    frame[1] = lenByte;
    frame[2] = 0x00;
    frame[3] = seq;
    frame[4] = payloadBytes[0];
    if (payloadLen > 1) {
      memcpy(frame.data() + 5, payloadBytes + 1, payloadLen - 1);
    }
    uint8_t sum = 0;
    for (size_t i = 2; i < (frame.size() - 1); i++) {
      sum = (uint8_t)((sum + frame[i]) & 0xFF);
    }
    frame[frame.size() - 1] = sum;
    frames->push_back(frame);
    *txSeq = (uint8_t)((seq + 2) & 0xFF);
    return;
  }

  const uint8_t cmd = payloadBytes[0];
  const uint8_t* rest = payloadBytes + 1;
  const size_t restLen = payloadLen - 1;
  const size_t numFrags = (restLen / 14) + 1;
  const size_t lastIndex = numFrags - 1;
  size_t offset = 0;

  for (size_t fragIndex = 0; fragIndex < numFrags; fragIndex++) {
    size_t chunkLen = 14;
    if (fragIndex == lastIndex) {
      chunkLen = restLen % 14;
    }
    std::vector<uint8_t> frame(chunkLen + 6);
    frame[0] = 0xB0;
    frame[1] = (uint8_t)((chunkLen + 3) & 0xFF);
    frame[2] = (uint8_t)(((lastIndex << 4) + lastIndex - fragIndex) & 0xFF);
    frame[3] = seq;
    frame[4] = cmd;
    if (chunkLen > 0) {
      memcpy(frame.data() + 5, rest + offset, chunkLen);
      offset += chunkLen;
    }
    uint8_t sum = 0;
    for (size_t i = 2; i < (frame.size() - 1); i++) {
      sum = (uint8_t)((sum + frame[i]) & 0xFF);
    }
    frame[chunkLen + 5] = sum;
    frames->push_back(frame);
    seq = (uint8_t)((seq + 2) & 0xFF);
  }

  *txSeq = seq;
}

static void pulseoxBuildB003(uint8_t step, uint8_t out6[6]) {
  out6[0] = 0xB0;
  out6[1] = 0x03;
  out6[2] = 0xA0;
  out6[3] = step;
  out6[4] = 0xAC;
  out6[5] = (uint8_t)((step + 0x4C) & 0xFF);
}

static void pulseoxBuildB004Fixed(uint8_t step, uint8_t dataByte, uint8_t out7[7]) {
  out7[0] = 0xB0;
  out7[1] = 0x04;
  out7[2] = 0x00;
  out7[3] = step;
  out7[4] = 0xAC;
  out7[5] = dataByte;
  out7[6] = (uint8_t)((step + 0xAC + dataByte) & 0xFF);
}

static void pulseoxBuildB004FromPrompt(uint8_t step, uint8_t tail1, uint8_t /*tail2*/, uint8_t out7[7]) {
  pulseoxBuildB004Fixed(step, tail1, out7);
}

static void pulseoxBuildFragmentAck(uint8_t meta, uint8_t seq, uint8_t cmd, uint8_t out6[6]) {
  const uint8_t ackCmd = (uint8_t)((meta + 0x70) & 0xFF);
  const uint8_t ackSeq = (uint8_t)((seq + 1) & 0xFF);
  out6[0] = 0xB0;
  out6[1] = 0x03;
  out6[2] = ackCmd;
  out6[3] = ackSeq;
  out6[4] = cmd;
  out6[5] = (uint8_t)((ackCmd + ackSeq + cmd) & 0xFF);
}

static bool pulseoxWriteValue(NimBLERemoteCharacteristic* writer, const uint8_t* data, size_t len, bool response, const char* label) {
  if (!writer || !data || len == 0) return false;
  const bool ok = writer->writeValue(data, len, response);
  Serial.printf("PO3: TX %s (%u bytes) => %s\n", label ? label : "", (unsigned)len, ok ? "OK" : "FAIL");
  return ok;
}

static void pulseoxVendorRxAppend(PulseOxSession* sess, const uint8_t* data, size_t len) {
  if (!sess || !data || len == 0) return;
  if ((sess->vendorRxLen + len) > sizeof(sess->vendorRxBuf)) {
    sess->vendorRxLen = 0;
  }
  const size_t copyLen = min(len, sizeof(sess->vendorRxBuf) - sess->vendorRxLen);
  memcpy(sess->vendorRxBuf + sess->vendorRxLen, data, copyLen);
  sess->vendorRxLen += copyLen;
}

static void pulseoxHandleVendorFragmentFrame(PulseOxSession* sess, const uint8_t* frame, size_t frameLen) {
  if (!sess || !frame || frameLen < 7) return;
  const uint8_t meta = frame[2];
  const uint8_t totalFragments = (uint8_t)((meta >> 4) + 1);
  const uint8_t reverseIndex = (uint8_t)(meta & 0x0F);
  if (totalFragments == 0 || totalFragments > 8 || reverseIndex >= totalFragments) return;

  const uint8_t fragIndex = (uint8_t)(totalFragments - reverseIndex - 1);
  uint8_t payloadLen = 0;
  const uint8_t* payload = nullptr;

  if (!sess->reassembly.active || sess->reassembly.totalFragments != totalFragments) {
    sess->reassembly = PulseOxReassembly{};
    sess->reassembly.active = true;
    sess->reassembly.totalFragments = totalFragments;
  }

  if (reverseIndex == (totalFragments - 1)) {
    sess->reassembly.opcode = frame[5];
    sess->reassembly.opcodeValid = true;
    payload = frame + 6;
    payloadLen = (uint8_t)(frameLen - 7);
  } else {
    payload = frame + 5;
    payloadLen = (uint8_t)(frameLen - 6);
  }

  if (payloadLen > sizeof(sess->reassembly.parts[fragIndex])) return;
  memcpy(sess->reassembly.parts[fragIndex], payload, payloadLen);
  sess->reassembly.partLen[fragIndex] = payloadLen;
  sess->reassembly.havePart[fragIndex] = true;

  uint8_t ack[6] = {0};
  pulseoxBuildFragmentAck(meta, frame[3], frame[4], ack);
  (void)pulseoxWriteValue(sess->writer, ack, sizeof(ack), sess->writeWithResponse, "fragment ack");

  if (!sess->reassembly.opcodeValid) return;
  for (uint8_t i = 0; i < totalFragments; i++) {
    if (!sess->reassembly.havePart[i]) return;
  }

  uint8_t assembled[64] = {0};
  size_t assembledLen = 0;
  for (uint8_t i = 0; i < totalFragments; i++) {
    const uint8_t n = sess->reassembly.partLen[i];
    if ((assembledLen + n) > sizeof(assembled)) {
      sess->reassembly = PulseOxReassembly{};
      return;
    }
    memcpy(assembled + assembledLen, sess->reassembly.parts[i], n);
    assembledLen += n;
  }

  if (assembledLen == 48) {
    pulseoxStoreIdentifyPayload(sess, sess->reassembly.opcode, assembled);
  }

  sess->reassembly = PulseOxReassembly{};
}

static void pulseoxVendorRxParseFrames(PulseOxSession* sess) {
  if (!sess) return;
  while (sess->vendorRxLen >= 2) {
    size_t start = 0;
    while (start < sess->vendorRxLen && sess->vendorRxBuf[start] != 0xA0) start++;
    if (start >= sess->vendorRxLen) {
      sess->vendorRxLen = 0;
      return;
    }
    if (start > 0) {
      memmove(sess->vendorRxBuf, sess->vendorRxBuf + start, sess->vendorRxLen - start);
      sess->vendorRxLen -= start;
    }
    if (sess->vendorRxLen < 2) return;

    const size_t totalLen = (size_t)sess->vendorRxBuf[1] + 3u;
    if (totalLen < 6 || totalLen > sizeof(sess->vendorRxBuf)) {
      memmove(sess->vendorRxBuf, sess->vendorRxBuf + 1, sess->vendorRxLen - 1);
      sess->vendorRxLen -= 1;
      continue;
    }
    if (sess->vendorRxLen < totalLen) return;

    uint8_t frame[128] = {0};
    memcpy(frame, sess->vendorRxBuf, totalLen);
    memmove(sess->vendorRxBuf, sess->vendorRxBuf + totalLen, sess->vendorRxLen - totalLen);
    sess->vendorRxLen -= totalLen;

    uint8_t sum = 0;
    for (size_t i = 2; i < (totalLen - 1); i++) {
      sum = (uint8_t)((sum + frame[i]) & 0xFF);
    }
    if (sum != frame[totalLen - 1]) continue;

    const uint8_t meta = frame[2];
    if (meta == 0x00 || meta == 0xF0 || meta >= 0xA0) continue;
    if (frame[4] != kPulseOxIdentifyCmd) continue;
    pulseoxHandleVendorFragmentFrame(sess, frame, totalLen);
  }
}

struct PulseOxDecodedFrame {
  bool ok = false;
  bool valid = false;
  uint8_t type = 0;
  uint8_t seq = 0;
  uint8_t spo2 = 0;
  float hr = 0.0f;
  uint8_t strength = 0;
  float pi = 0.0f;
  uint16_t piNum = 0;
  uint16_t piDen = 0;
  uint16_t wave[3] = {0, 0, 0};
};

static uint16_t pulseoxU16le(const uint8_t* data, size_t index) {
  return (uint16_t)((uint16_t)data[index] | ((uint16_t)data[index + 1] << 8));
}

static PulseOxDecodedFrame pulseoxDecodeMeasurementFrame(PulseOxSession* sess, const uint8_t frame[20]) {
  PulseOxDecodedFrame out;
  if (!sess || !frame) return out;
  if (frame[0] != 0xA0 || frame[1] != 0x11) return out;

  out.type = frame[2];
  out.seq = frame[3];
  out.spo2 = frame[6];
  out.hr = (float)frame[7];
  out.strength = frame[8];
  out.piNum = pulseoxU16le(frame, 9);
  out.piDen = pulseoxU16le(frame, 11);
  out.wave[0] = pulseoxU16le(frame, 13);
  out.wave[1] = pulseoxU16le(frame, 15);
  out.wave[2] = pulseoxU16le(frame, 17);

  float pi = sess->lastGoodPi;
  if (out.spo2 != 0 && out.hr != 0.0f && out.piDen != 0) {
    const float candidate = roundf(((float)out.piNum / (float)out.piDen) * 1000.0f) / 10.0f;
    if (candidate >= 0.2f && candidate <= 20.0f) {
      sess->lastGoodPi = candidate;
      pi = candidate;
    }
  }
  out.pi = pi;
  out.valid = !(out.strength == 0 || (out.wave[0] == 0 && out.wave[1] == 0 && out.wave[2] == 0));
  out.ok = (out.type == 0xF0 && out.hr > 0.0f && out.hr <= 250.0f && out.spo2 >= 50 && out.spo2 <= 110 &&
            out.strength <= 8);
  return out;
}

static void pulseoxHandleMeasurementBytes(PulseOxSession* sess, const uint8_t* data, size_t len) {
  if (!sess || !data || len < 20) return;
  for (size_t pos = 0; (pos + 20) <= len; pos++) {
    if (data[pos] != 0xA0 || data[pos + 1] != 0x11) continue;
    PulseOxDecodedFrame decoded = pulseoxDecodeMeasurementFrame(sess, data + pos);
    if (!decoded.ok) continue;
    pulseoxSetLiveMeasurement(sess, (float)decoded.spo2, decoded.hr, decoded.pi, decoded.strength, decoded.wave);
  }
}

static void pulseoxNotifyHandler(NimBLERemoteCharacteristic* /*c*/, uint8_t* data, size_t len, bool /*isNotify*/) {
  PulseOxSession* sess = gPulseOxSession;
  if (!sess || !data || len == 0) return;

  if (sess->handshakeRunning && !sess->handshakeComplete) {
    pulseoxVendorRxAppend(sess, data, len);
    pulseoxVendorRxParseFrames(sess);
  }

  if (len == 6 && data[0] == 0xA0 && data[1] == 0x03 && data[2] == 0xA0) {
    pulseoxMarkAckSeen(sess, data[3]);
  }

  if (len == 7 && data[0] == 0xA0 && data[1] == 0x04 && data[2] == 0x00 && data[4] == 0xAC) {
    pulseoxSetPrompt(sess, data[3], data[5], data[6], false);
  }

  if (len == 12 && data[0] == 0xA0 && data[1] == 0x06 && data[2] == 0x00 && data[4] == 0xAC) {
    pulseoxSetPrompt(sess, data[3], data[5], data[6], true);
  }

  pulseoxHandleMeasurementBytes(sess, data, len);
}

static bool pulseoxRunVendorHandshake(PulseOxSession* sess) {
  if (!sess || !sess->writer) return false;

  uint8_t identify1[18] = {0};
  std::vector<std::vector<uint8_t>> frames;
  pulseoxIdentifyStage1(identify1);
  pulseoxPackageForWrite(identify1, sizeof(identify1), &sess->txSeq, &frames);
  for (const auto& frame : frames) {
    if (!pulseoxWriteValue(sess->writer, frame.data(), frame.size(), sess->writeWithResponse, "stage1")) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(60));
  }
  if (!pulseoxWaitForAck(sess, 0x04, 5000)) {
    Serial.println("PO3: stage1 ACK timeout");
    return false;
  }

  uint8_t challenge48[48] = {0};
  uint8_t opcode = 0;
  if (!pulseoxWaitForIdentifyChallenge(sess, &opcode, challenge48, 5000)) {
    Serial.println("PO3: identify challenge timeout");
    return false;
  }

  uint8_t identify2[18] = {0};
  pulseoxIdentifyStage2(challenge48, identify2);
  pulseoxPackageForWrite(identify2, sizeof(identify2), &sess->txSeq, &frames);
  for (const auto& frame : frames) {
    if (!pulseoxWriteValue(sess->writer, frame.data(), frame.size(), sess->writeWithResponse, "stage2")) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(60));
  }

  uint8_t stage2Ack = 0;
  const uint8_t stage2Steps[2] = {0x08, 0x12};
  if (!pulseoxWaitForAckAny(sess, stage2Steps, 2, &stage2Ack, 5000)) {
    Serial.println("PO3: stage2 ACK timeout");
    return false;
  }

  int delta = (int)stage2Ack - 0x12;
  if (delta > 127) delta -= 256;
  if (delta < -127) delta += 256;

  struct HandshakeStage {
    const char* name;
    uint8_t completeAckStep;
    uint8_t promptBase;
    uint8_t b004Data;
    bool allowPrompt;
  };

  const HandshakeStage stages[] = {
      {"stage3", (uint8_t)((0x18 + delta) & 0xFF), (uint8_t)((0x14 + delta) & 0xFF), 0xC1, true},
      {"stage4", (uint8_t)((0x1E + delta) & 0xFF), (uint8_t)((0x1A + delta) & 0xFF), 0xC6, false},
      {"stage5", (uint8_t)((0x24 + delta) & 0xFF), (uint8_t)((0x20 + delta) & 0xFF), 0xA5, true},
      {"stage6", (uint8_t)((0x2C + delta) & 0xFF), 0x00, 0xA6, false},
  };

  for (const HandshakeStage& stage : stages) {
    if (strcmp(stage.name, "stage6") != 0) {
      const uint8_t b003Step = (uint8_t)((stage.completeAckStep - 3) & 0xFF);
      const uint8_t b004Step = (uint8_t)((stage.completeAckStep - 1) & 0xFF);
      uint8_t w1[6] = {0};
      uint8_t w2[7] = {0};
      PulseOxPrompt prompt;
      bool havePrompt = false;
      if (stage.allowPrompt) {
        havePrompt = pulseoxWaitForPrompt(sess, stage.promptBase, &prompt, 400);
      }

      pulseoxBuildB003(b003Step, w1);
      if (strcmp(stage.name, "stage5") == 0 && havePrompt && !prompt.isA006) {
        pulseoxBuildB004FromPrompt(b004Step, prompt.tail1, prompt.tail2, w2);
      } else {
        if (strcmp(stage.name, "stage5") == 0) {
          sess->stage5WasA006 = havePrompt && prompt.isA006;
        }
        pulseoxBuildB004Fixed(b004Step, stage.b004Data, w2);
      }

      if (!pulseoxWriteValue(sess->writer, w1, sizeof(w1), sess->writeWithResponse, stage.name)) return false;
      vTaskDelay(pdMS_TO_TICKS(60));
      if (!pulseoxWriteValue(sess->writer, w2, sizeof(w2), sess->writeWithResponse, stage.name)) return false;
    } else {
      const uint8_t promptBase26 = (uint8_t)((stage.completeAckStep - 6) & 0xFF);
      const uint8_t promptBase28 = (uint8_t)((stage.completeAckStep - 4) & 0xFF);
      PulseOxPrompt prompt26;
      PulseOxPrompt prompt28;
      (void)pulseoxWaitForPrompt(sess, promptBase26, &prompt26, 800);

      uint8_t first[6] = {0};
      pulseoxBuildB003((uint8_t)((stage.completeAckStep - 5) & 0xFF), first);
      if (!pulseoxWriteValue(sess->writer, first, sizeof(first), sess->writeWithResponse, "stage6")) return false;
      vTaskDelay(pdMS_TO_TICKS(60));

      const bool havePrompt28 = pulseoxWaitForPrompt(sess, promptBase28, &prompt28, 200);
      const uint8_t dataByteForB004 = havePrompt28 ? prompt28.tail1 : stage.b004Data;

      uint8_t w1[6] = {0};
      uint8_t w2[7] = {0};
      pulseoxBuildB003((uint8_t)((stage.completeAckStep - 3) & 0xFF), w1);
      pulseoxBuildB004Fixed((uint8_t)((stage.completeAckStep - 1) & 0xFF), dataByteForB004, w2);
      if (!pulseoxWriteValue(sess->writer, w1, sizeof(w1), sess->writeWithResponse, "stage6")) return false;
      vTaskDelay(pdMS_TO_TICKS(60));
      if (!pulseoxWriteValue(sess->writer, w2, sizeof(w2), sess->writeWithResponse, "stage6")) return false;
    }

    if (!pulseoxWaitForAck(sess, stage.completeAckStep, 5000)) {
      Serial.printf("PO3: %s ACK timeout\n", stage.name);
      return false;
    }
  }

  sess->handshakeRunning = false;
  sess->handshakeComplete = true;
  return true;
}

static bool runPulseOxSession(const BleTargetCandidate& target) {
  static constexpr uint32_t kPulseOxInactivityDisconnectMs = 10000;
  static constexpr uint32_t kPulseOxReconnectCooldownMs = 120000;

  NimBLEClient* client = NimBLEDevice::createClient();
  if (!client) return false;

  client->setConnectTimeout(8000);
  const bool okConn = client->connect(target.address);
  if (!okConn) {
    Serial.printf("PO3: connect failed err=%d\n", client->getLastError());
    NimBLEDevice::deleteClient(client);
    return false;
  }

  setBleConnected(true);
  pulseox_demo_lvgl_set_live_mode(true);
  pulseox_demo_lvgl_set_live_readings(0.0f, 0.0f, 0.0f, 0);
  requestUiStep(7);
  Serial.printf("PO3: connected to %s\n", target.name.empty() ? target.address.toString().c_str() : target.name.c_str());

  NimBLERemoteService* svc = nullptr;
  const NimBLEUUID serviceCandidates[] = {kPulseOxSvcUuid, kPulseOxSvcUuidAlt, kPulseOxSvcUuidWireshark};
  for (int attempt = 1; attempt <= 4 && !svc; attempt++) {
    for (const NimBLEUUID& uuid : serviceCandidates) {
      svc = client->getService(uuid);
      if (svc) break;
    }
    if (!svc) {
      vTaskDelay(pdMS_TO_TICKS(250 * attempt));
    }
  }

  if (!svc) {
    Serial.println("PO3: vendor service not found");
    pulseox_demo_lvgl_set_live_mode(false);
    setBleConnected(false);
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  const auto& characteristics = svc->getCharacteristics(true);
  std::vector<NimBLERemoteCharacteristic*> notifyCandidates;
  std::vector<NimBLERemoteCharacteristic*> writeCandidates;
  for (NimBLERemoteCharacteristic* ch : characteristics) {
    if (!ch) continue;
    if (ch->canNotify() || ch->canIndicate()) {
      notifyCandidates.push_back(ch);
    }
    if (ch->canWriteNoResponse() || ch->canWrite()) {
      writeCandidates.push_back(ch);
    }
  }

  if (notifyCandidates.empty() || writeCandidates.empty()) {
    Serial.println("PO3: missing notify/write characteristics");
    pulseox_demo_lvgl_set_live_mode(false);
    setBleConnected(false);
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  NimBLERemoteCharacteristic* writer = nullptr;
  for (NimBLERemoteCharacteristic* ch : notifyCandidates) {
    if (ch && (ch->canWriteNoResponse() || ch->canWrite())) {
      writer = ch;
      break;
    }
  }
  if (!writer) {
    writer = writeCandidates[0];
  }

  PulseOxSession sess;
  sess.writer = writer;
  sess.writeWithResponse = !writer->canWriteNoResponse();
  sess.handshakeRunning = true;
  gPulseOxSession = &sess;

  bool subscribed = false;
  for (NimBLERemoteCharacteristic* ch : notifyCandidates) {
    if (!ch) continue;
    if (ch->subscribe(ch->canNotify(), pulseoxNotifyHandler, true)) {
      subscribed = true;
      Serial.printf("PO3: subscribed %s\n", ch->getUUID().toString().c_str());
    }
  }

  if (!subscribed) {
    gPulseOxSession = nullptr;
    pulseox_demo_lvgl_set_live_mode(false);
    setBleConnected(false);
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  bool ok = pulseoxRunVendorHandshake(&sess);
  if (ok) {
    sess.lastFrameAtMs = millis();
    Serial.println("PO3: handshake complete, waiting for live measurements");
    while (client->isConnected()) {
      if (isAnyDemoGateActive()) break;
      if (gPulseOxDisconnectRequested) {
        gPulseOxDisconnectRequested = false;
        Serial.println("PO3: disconnect requested (touch)");
        client->disconnect();
        break;
      }
      const uint32_t idleMs = millis() - sess.lastFrameAtMs;
      if (idleMs > kPulseOxInactivityDisconnectMs) {
        Serial.println("PO3: no readings for 10s; disconnecting");
        client->disconnect();
        break;
      }
      if (sess.firstMeasurement && idleMs > 1500) {
        pulseox_demo_lvgl_set_live_readings(sess.spo2, sess.hr, sess.pi, 0);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  startPulseOxReconnectCooldown(kPulseOxReconnectCooldownMs);
  gPulseOxSession = nullptr;
  pulseox_demo_lvgl_set_live_mode(false);
  setBleConnected(false);
  client->disconnect();
  NimBLEDevice::deleteClient(client);
  if (!isAnyDemoGateActive()) {
    requestUiStep(4);
  }
  return ok;
}

static bool runGkPlusSession(const BleTargetCandidate& target) {
  // GK+ GATT UUIDs (pcap-derived, canonical).
  const NimBLEUUID kSvc("0003cdd0-0000-1000-8000-00805f9b0131");
  const NimBLEUUID kNotify("0003cdd1-0000-1000-8000-00805f9b0131");
  const NimBLEUUID kWrite("0003cdd2-0000-1000-8000-00805f9b0131");

  static const uint8_t kReqInfo66[] = {0x7b, 0x01, 0x10, 0x01, 0x20, 0x66, 0x55, 0x00, 0x00, 0x01, 0x0e, 0x08, 0x08, 0x7d};
  static const uint8_t kReqInitAa[] = {0x7b, 0x01, 0x10, 0x01, 0x20, 0xaa, 0x55, 0x00, 0x00, 0x02, 0x01, 0x0d, 0x08, 0x7d};
  static const uint8_t kReqInfo77[] = {0x7b, 0x01, 0x10, 0x01, 0x20, 0x77, 0x55, 0x00, 0x00, 0x01, 0x0b, 0x0b, 0x04, 0x7d};
  static const uint8_t kReqCountDb[] = {0x7b, 0x01, 0x10, 0x01, 0x20, 0xdb, 0x55, 0x00, 0x00, 0x03, 0x0a, 0x0e, 0x04, 0x7d};

  struct Session {
    std::vector<GkRecord> raw;
    volatile bool gotDbCount = false;
    volatile bool gotReading = false;
  };

  static Session* sSession = nullptr;
  static auto onNotify = [](NimBLERemoteCharacteristic* /*c*/, uint8_t* data, size_t len, bool /*isNotify*/) {
    Session* sess = sSession;
    if (!sess || !data || len == 0) return;

    size_t i = 0;
    while (i < len) {
      while (i < len && data[i] != 0x7B) i++;
      if (i >= len) break;
      size_t j = i + 1;
      while (j < len && data[j] != 0x7D) j++;
      if (j >= len) break;

      const uint8_t* f = data + i;
      const size_t flen = (j - i) + 1;
      i = j + 1;

      if (flen < 12) continue;
      const uint8_t cmd = f[5];

      if (cmd == 0xDB && flen >= 16 && f[6] == 0xAA) {
        sess->gotDbCount = true;
        continue;
      }

      if ((cmd == 0x16 || cmd == 0xDD || cmd == 0xDE) && flen >= 23 && f[6] == 0xAA && f[7] == 0x00 && f[8] == 0x09) {
        GkRecord r;
        if (!decodeGkRecordSnippet9(f + 9, 9, &r)) continue;
        if (r.kind == GkRecord::UNKNOWN) continue;
        if (!isfinite(r.value)) continue;
        sess->raw.push_back(r);
        if (sess->raw.size() > 16) {
          sess->raw.erase(sess->raw.begin(), sess->raw.begin() + (sess->raw.size() - 16));
        }

        const int windowMin = 15;
        const GkRecord* bestG = nullptr;
        const GkRecord* bestK = nullptr;
        int64_t bestKey = -1;
        for (const auto& g : sess->raw) {
          if (g.kind != GkRecord::GLUCOSE) continue;
          for (const auto& k : sess->raw) {
            if (k.kind != GkRecord::KETONE) continue;
            const int64_t dt = (k.tMin > g.tMin) ? (k.tMin - g.tMin) : (g.tMin - k.tMin);
            if (dt > windowMin) continue;
            const int64_t key = (int64_t)max(g.whenKey, k.whenKey);
            if (key > bestKey) {
              bestKey = key;
              bestG = &g;
              bestK = &k;
            }
          }
        }

        if (bestG && bestK) {
          const float glucose = bestG->value;
          const float ketone = bestK->value;
          const float gki = computeShownGki(glucose, ketone);
          const int stage = ketosisStageFromGki(gki);

          portENTER_CRITICAL(&gUiMux);
          gGluMgDl = glucose;
          gKetMmolL = ketone;
          gGkiOverride = true;
          gGkiValue = gki;
          snprintf(gGkWhenLocal, sizeof(gGkWhenLocal), "%02d:%02d", bestG->hour, bestG->minute);
          gStatusStripDirty = true;
          gOverlayDirty = true;
          portEXIT_CRITICAL(&gUiMux);

          requestUiStep(stage);
          if (!isAnyDemoGateActive()) {
            startOverlayReveal();
          }
          sess->gotReading = true;
        }
      }
    }
  };

  NimBLEClient* client = NimBLEDevice::createClient();
  if (!client) return false;
  client->setConnectTimeout(8000);

  if (!client->connect(target.address)) {
    Serial.printf("GK+: connect failed err=%d\n", client->getLastError());
    NimBLEDevice::deleteClient(client);
    return false;
  }

  setBleConnected(true);
  Serial.printf("GK+: connected to %s\n", target.name.empty() ? target.address.toString().c_str() : target.name.c_str());

  NimBLERemoteService* svc = client->getService(kSvc);
  if (!svc) {
    setBleConnected(false);
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  NimBLERemoteCharacteristic* chNotify = svc->getCharacteristic(kNotify);
  NimBLERemoteCharacteristic* chWrite = svc->getCharacteristic(kWrite);
  if (!chNotify || !chWrite) {
    setBleConnected(false);
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  Session sess;
  sess.raw.reserve(8);
  sSession = &sess;

  if (!chNotify->subscribe(true, onNotify)) {
    sSession = nullptr;
    setBleConnected(false);
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  auto writeFrame = [&](const uint8_t* buf, size_t n, bool withResponse, const char* label) {
    if (!buf || n == 0) return;
    const bool ok = chWrite->writeValue(buf, n, withResponse);
    Serial.printf("GK+: TX %s (%u bytes) => %s\n", label ? label : "", (unsigned)n, ok ? "OK" : "FAIL");
  };

  writeFrame(kReqInfo66, sizeof(kReqInfo66), false, "0x66");
  vTaskDelay(pdMS_TO_TICKS(120));
  writeFrame(kReqInitAa, sizeof(kReqInitAa), false, "0xAA");
  vTaskDelay(pdMS_TO_TICKS(120));
  writeFrame(kReqInfo77, sizeof(kReqInfo77), false, "0x77");
  vTaskDelay(pdMS_TO_TICKS(120));
  writeFrame(kReqInfo66, sizeof(kReqInfo66), false, "0x66 (again)");
  vTaskDelay(pdMS_TO_TICKS(120));
  writeFrame(kReqCountDb, sizeof(kReqCountDb), false, "0xDB (records count)");

  const uint32_t t0 = millis();
  std::vector<uint8_t> cmd16;
  bool sentXfer = false;
  while (client->isConnected()) {
    if (isAnyDemoGateActive()) break;
    if (gGkDisconnectRequested) {
      gGkDisconnectRequested = false;
      Serial.println("GK+: disconnect requested (touch)");
      break;
    }
    if (sess.gotReading) break;

    const uint32_t elapsed = millis() - t0;
    if (elapsed > 8000) {
      Serial.println("GK+: timeout waiting for reading; retry");
      break;
    }

    if (sess.gotDbCount && !sentXfer) {
      sentXfer = true;
      buildLatestRecords16Command(2, &cmd16);
      if (!cmd16.empty()) {
        writeFrame(cmd16.data(), cmd16.size(), false, "0x16 (latest records n=2)");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }

  const bool okReading = sess.gotReading;
  sSession = nullptr;
  setBleConnected(false);
  client->disconnect();
  NimBLEDevice::deleteClient(client);
  return okReading;
}

static void gkplusTask(void* /*param*/) {
  Serial.println("BLE: sensor task start");

  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEScan* scan = NimBLEDevice::getScan();
  for (;;) {
    if (isAnyDemoGateActive()) {
      pulseox_demo_lvgl_set_live_mode(false);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // If both device kinds are blocked, idle until reboot/wake.
    if (gBlockGkPlusConnections && gBlockPulseOxConnections) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    requestUiStep(4);
    BleTargetCandidate target;
    if (!scanForSupportedTarget(scan, &target)) {
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    if (target.kind == kTargetGkPlus && gBlockGkPlusConnections) {
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }
    if (target.kind == kTargetPulseOx && gBlockPulseOxConnections) {
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    if (target.kind == kTargetPulseOx && isPulseOxReconnectCooldownActive()) {
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    const char* kindStr = (target.kind == kTargetPulseOx) ? "PO3" : "GK+";
    Serial.printf("BLE: found %s addr=%s name='%s' rssi=%d\n", kindStr, target.address.toString().c_str(),
                  target.name.c_str(), (int)target.rssi);

    bool ok = false;
    if (target.kind == kTargetPulseOx) {
      ok = runPulseOxSession(target);
    } else if (target.kind == kTargetGkPlus) {
      ok = runGkPlusSession(target);
      if (ok) {
        Serial.println("GK+: reading acquired; task done");
        vTaskDelete(nullptr);
        return;
      }
    }

    if (!ok) {
      vTaskDelay(pdMS_TO_TICKS(800));
    }
  }
}

static uint32_t gDemoStartMs = 0;

static size_t gLoopStartFrame = 0;
static size_t gLoopFrameCount = 0;

static std::vector<std::string> gLayerNames;

static constexpr size_t kLottieLoopFrames = 26;

static void setLoopSegmentFrames(size_t startFrame, size_t frameCount) {
  gLoopStartFrame = startFrame;
  gLoopFrameCount = frameCount;
  gAnimStartMs = millis();
}

static void setLoopToFlameBurn() {
  if (!gAnim) return;
  const size_t total = max<size_t>(1, gAnimTotalFrames);
  if (assets::kHasMarkerFlameBurn && assets::kMarkerFlameBurnFrameCount > 0) {
    const size_t start = (size_t)max(0, assets::kMarkerFlameBurnStartFrame);
    const size_t count = (size_t)max(0, assets::kMarkerFlameBurnFrameCount);
    if (start < total && count > 0) {
      setLoopSegmentFrames(start, min(count, total - start));
      return;
    }
  }
  const size_t fallback = (size_t)max<size_t>(1, min(kLottieLoopFrames, total));
  setLoopSegmentFrames(0, fallback);
}

static void setLoopToLogLoading() {
  if (!gAnim) return;
  const size_t total = max<size_t>(1, gAnimTotalFrames);
  if (assets::kHasMarkerLogLoading && assets::kMarkerLogLoadingFrameCount > 0) {
    const size_t start = (size_t)max(0, assets::kMarkerLogLoadingStartFrame);
    const size_t count = (size_t)max(0, assets::kMarkerLogLoadingFrameCount);
    if (start < total && count > 0) {
      setLoopSegmentFrames(start, min(count, total - start));
      return;
    }
  }
  const size_t fallback = (size_t)max<size_t>(1, min((size_t)19, total));
  setLoopSegmentFrames(0, fallback);
}

static void drawOverlay() {
  if (gShowGoodbye) {
    setupKetosisLabelColorsOnce();
    initPanelFxSpriteOnce();

    TFT_eSprite* spr = gPanelFxSpriteOk ? &gPanelFxSprite : nullptr;
    applyPanelViewport();
    const uint32_t now = millis();
    const uint16_t bg0 = rgb888To565(0x04, 0x09, 0x0F);

    // Avoid visible flashing/tearing: draw the full frame into the sprite when
    // possible, then push once.
    if (spr) {
      spr->fillSprite(bg0);
    } else {
      tft.fillRect(0, 0, kPanelW, kPanelH, bg0);
    }

    auto easeIn01 = [](float x) {
      x = max(0.0f, min(1.0f, x));
      // Cubic ease-in.
      return x * x * x;
    };

    auto drawPixel = [&](int x, int y, uint16_t c) {
      if ((unsigned)x >= (unsigned)kPanelW || (unsigned)y >= (unsigned)kPanelH) return;
      if (spr) spr->drawPixel(x, y, c);
      else tft.drawPixel(x, y, c);
    };

    // Ensure animation clock is initialized.
    if (gGoodbyeAnimStartMs == 0) {
      gGoodbyeAnimStartMs = now;
      gGoodbyeBannerStartMs = 0;
      gGoodbyePhase = kGoodbyeConfetti;
      gGoodbyePhaseStartMs = now;
      gGoodbyeAnimDone = false;
    }

    const uint32_t tAnim = (now >= gGoodbyeAnimStartMs) ? (now - gGoodbyeAnimStartMs) : 0;

    static const uint16_t kConfC0 = gKetosisRed565;
    static const uint16_t kConfC1 = gKetosisOrange565;
    static const uint16_t kConfC2 = gKetosisYellow565;
    static const uint16_t kConfC3 = rgb888To565(0x00, 0xB0, 0xA0);
    static const uint16_t kConfC4 = rgb888To565(0x50, 0x70, 0xFF);

    // Animation timing.
    static constexpr uint32_t kConfettiMs = 2500;
    static constexpr uint32_t kBannerExpandMs = 500;
    const uint32_t totalMs = kConfettiMs + kBannerExpandMs;

    // Phase is driven purely from time (stable, no flashing on state changes).
    if (tAnim < kConfettiMs) {
      gGoodbyePhase = kGoodbyeConfetti;
    } else if (tAnim < totalMs) {
      gGoodbyePhase = kGoodbyeBannerExpand;
    } else {
      gGoodbyePhase = kGoodbyeDone;
      gGoodbyeAnimDone = true;
    }

    // Confetti fall: single continuous "gravity" across the entire screen.
    // - Starts with none visible (spawns above the top).
    // - New confetti keeps spawning the entire time.
    // - One motion model (no staged piecewise motion).
    const uint32_t tSpawn = min(tAnim, totalMs);
    const int midY = kPanelH / 2;
    static constexpr uint32_t kConfettiBottomMs = 1500;  // smaller = heavier/faster
    static constexpr float kGravityScale = 1.15f;        // additional heaviness

    // Confetti field:
    // - Start with *no* confetti visible at t=0.
    // - Spawn from the top only.
    // - Keep spawning new confetti throughout the whole animation window.
    // - Density ramps up naturally as more spawns accumulate.
    auto confettiFallDistancePx = [&](uint32_t ageMs) -> int {
      // y(t) = 0.5 * g * t^2, with g chosen so y(bottomMs) ~= bottom.
      const float bottomPx = (float)max(1, (kPanelH - 1));
      const float t = (float)ageMs;
      const float denom = (float)max<uint32_t>(1, kConfettiBottomMs);
      const float g = kGravityScale * (2.0f * bottomPx) / (denom * denom);
      const float y = 0.5f * g * t * t;
      return (int)lroundf(y);
    };

    // +50% more confetti.
    static constexpr int kMaxConfetti = 315;
    static constexpr uint32_t kSpawnIntervalMs = 25;  // 40 spawns/sec
    // Small spawn band keeps pieces near the top so they appear quickly.
    const int kSpawnBand = max(2, (kPanelH / 10));

    if (tSpawn > 0) {
      const int totalSpawns = 1 + (int)(tSpawn / kSpawnIntervalMs);
      const int count = min(kMaxConfetti, totalSpawns);
      const int firstSpawn = totalSpawns - count;

      for (int s = firstSpawn; s < totalSpawns; s++) {
        const uint32_t spawnMs = (uint32_t)s * kSpawnIntervalMs;
        const uint32_t ageMs = (tAnim >= spawnMs) ? (tAnim - spawnMs) : 0;
        const int dPx = confettiFallDistancePx(ageMs);

        const uint32_t h = 0x9E3779B9u ^ (uint32_t)(s * 2654435761u);
        const int x = 2 + (int)(h % (uint32_t)(kPanelW - 4));
        // Start above the visible area so at t=0 nothing is on-screen.
        const int y0 = -1 - (int)((h >> 8) % (uint32_t)kSpawnBand);

        const int y = y0 + dPx;
        if (y < 0 || y >= kPanelH) continue;

        const int sel = (int)((h >> 16) % 5u);
        const uint16_t c = (sel == 0) ? kConfC0 : (sel == 1) ? kConfC1 : (sel == 2) ? kConfC2 : (sel == 3) ? kConfC3 : kConfC4;
        drawPixel(x, y, c);
        if ((h & 1u) == 0) drawPixel(x + 1, y + 1, c);
      }
    }

    // Heart balloon: visible from the start, but stationary until the confetti
    // (using the same gravity model) reaches the middle, then rises while
    // confetti keeps falling.
    const float bottomPx = (float)max(1, (kPanelH - 1));
    const float denom = (float)max<uint32_t>(1, kConfettiBottomMs);
    const float g = kGravityScale * (2.0f * bottomPx) / (denom * denom);
    const float tTrig = (g > 0.0f) ? sqrtf((2.0f * (float)midY) / g) : (float)kConfettiBottomMs;
    const uint32_t kBalloonTriggerMs = (uint32_t)max(0.0f, min((float)totalMs, tTrig));
    // Slow the balloon rise a bit by giving it the whole remaining animation
    // window (including the banner expand) to get off-screen.
    const uint32_t balloonEndMs = totalMs;
    const float bP = (tAnim <= kBalloonTriggerMs || balloonEndMs <= kBalloonTriggerMs)
              ? 0.0f
              : easeIn01((float)(min(tAnim, balloonEndMs) - kBalloonTriggerMs) / (float)(balloonEndMs - kBalloonTriggerMs));
    const int cx = kPanelW / 2;
    const int startY = 104;
    // Ensure the balloon and string are fully off-screen before the banner expansion begins.
    const int endY = -170;
    const int cy = (int)lroundf((1.0f - bP) * (float)startY + bP * (float)endY);
    const uint16_t heart = gKetosisRed565;
    const uint16_t heartHi = rgb888To565(0xFF, 0xA0, 0xA0);
    if (spr) {
      spr->fillCircle(cx - 8, cy - 4, 8, heart);
      spr->fillCircle(cx + 8, cy - 4, 8, heart);
      spr->fillTriangle(cx - 16, cy - 2, cx + 16, cy - 2, cx, cy + 22, heart);
      spr->fillCircle(cx - 5, cy - 6, 2, heartHi);
      spr->drawLine(cx, cy + 22, cx - 6, cy + 40, TFT_LIGHTGREY);
      spr->drawLine(cx - 6, cy + 40, cx - 3, cy + 58, TFT_LIGHTGREY);
    } else {
      tft.fillCircle(cx - 8, cy - 4, 8, heart);
      tft.fillCircle(cx + 8, cy - 4, 8, heart);
      tft.fillTriangle(cx - 16, cy - 2, cx + 16, cy - 2, cx, cy + 22, heart);
      tft.fillCircle(cx - 5, cy - 6, 2, heartHi);
      tft.drawLine(cx, cy + 22, cx - 6, cy + 40, TFT_LIGHTGREY);
      tft.drawLine(cx - 6, cy + 40, cx - 3, cy + 58, TFT_LIGHTGREY);
    }

    // Banner: full-width stripe is visible at all times, and expands vertically
    // to fill the whole screen over the last 0.5s.
    const uint32_t tB = (tAnim > kConfettiMs) ? min(tAnim - kConfettiMs, kBannerExpandMs) : 0;
    const float bT = (kBannerExpandMs > 0) ? ((float)tB / (float)kBannerExpandMs) : 1.0f;
    const float bannerP = (tAnim >= totalMs) ? 1.0f : easeIn01(bT);

    const int h0 = 20;
    const int h1 = kPanelH;
    const int bh = (int)lroundf((1.0f - bannerP) * (float)h0 + bannerP * (float)h1);
    const int by = (kPanelH - bh) / 2;

    const uint16_t bannerC = rgb888To565(0x00, 0xB0, 0xA0);
    const uint16_t bannerText = TFT_WHITE;

    if (spr) {
      spr->fillRect(0, by, kPanelW, bh, bannerC);
      spr->setTextDatum(MC_DATUM);
      spr->setTextSize(1);
      spr->setTextColor(bannerText, bannerC);
      spr->drawString("GOODBYE", kPanelW / 2, kPanelH / 2);
    } else {
      tft.fillRect(0, by, kPanelW, bh, bannerC);
      tft.setTextDatum(MC_DATUM);
      tft.setTextSize(1);
      tft.setTextColor(bannerText, bannerC);
      tft.drawString("GOODBYE", kPanelW / 2, kPanelH / 2);
    }

    // Present the sprite in one blit.
    if (spr) {
      int ax = 0;
      int ay = 0;
      getPanelAbsXY(&ax, &ay);
      tft.setSwapBytes(kSwapBytesForPushImage);
      spr->pushSprite(ax, ay);
    }

    // Avoid spamming FB trace during animation; emit at most ~2 FPS, and always on phase changes.
    static uint8_t sLastTracePhase = 0;
    static uint32_t sLastTraceMs = 0;
    const bool phaseChanged = (sLastTracePhase != gGoodbyePhase);
    if (phaseChanged || (now - sLastTraceMs) >= 250) {
      sLastTracePhase = gGoodbyePhase;
      sLastTraceMs = now;
      emitStaticScreenTrace();
    }
    return;
  }

  if (gShowLowBattery) {
    setupKetosisLabelColorsOnce();
    initPanelFxSpriteOnce();

    // Draw into sprite for trace capture, then push to TFT.
    TFT_eSprite* spr = gPanelFxSpriteOk ? &gPanelFxSprite : nullptr;
    // Also draw directly to TFT as fallback / final output.
    applyPanelViewport();
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    tft.fillRect(0, 0, kPanelW, kPanelH, TFT_BLACK);
    if (spr) { spr->fillSprite(TFT_BLACK); }

    // Red banner
    const int bannerH = 16;
    tft.fillRect(0, 0, kPanelW, bannerH, gKetosisRed565);
    tft.setTextColor(TFT_WHITE, gKetosisRed565);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.drawString("LOW BATTERY", kPanelW / 2, bannerH / 2);
    if (spr) {
      spr->fillRect(0, 0, kPanelW, bannerH, gKetosisRed565);
      spr->setTextColor(TFT_WHITE, gKetosisRed565);
      spr->setTextDatum(MC_DATUM);
      spr->setTextSize(1);
      spr->drawString("LOW BATTERY", kPanelW / 2, bannerH / 2);
    }

    // Battery icon: 40x22 body + 4x10 terminal nub, centered
    const int bw = 40, bh = 22, nubW = 4, nubH = 10;
    const int bx = (kPanelW - bw - nubW) / 2;
    const int by = (kPanelH - bannerH - 12) / 2 + bannerH - bh / 2;
    const uint16_t outlineColor = TFT_WHITE;
    const uint16_t fillColor = gKetosisRed565;
    // Body outline
    tft.drawRect(bx, by, bw, bh, outlineColor);
    tft.drawRect(bx + 1, by + 1, bw - 2, bh - 2, outlineColor);
    // Positive terminal nub
    tft.fillRect(bx + bw, by + (bh - nubH) / 2, nubW, nubH, outlineColor);
    // Red fill: ~15% charge (single sliver on the left)
    const int fillInset = 3;
    const int fillH = bh - fillInset * 2;
    const int fillMaxW = bw - fillInset * 2;
    const int fillW = max(3, fillMaxW * 15 / 100);
    tft.fillRect(bx + fillInset, by + fillInset, fillW, fillH, fillColor);
    if (spr) {
      spr->drawRect(bx, by, bw, bh, outlineColor);
      spr->drawRect(bx + 1, by + 1, bw - 2, bh - 2, outlineColor);
      spr->fillRect(bx + bw, by + (bh - nubH) / 2, nubW, nubH, outlineColor);
      spr->fillRect(bx + fillInset, by + fillInset, fillW, fillH, fillColor);
    }

    // Warning text
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("CHARGE", kPanelW / 2, by + bh + 14);
    if (spr) {
      spr->setTextDatum(TC_DATUM);
      spr->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      spr->drawString("CHARGE", kPanelW / 2, by + bh + 14);
    }

    tft.setTextDatum(TL_DATUM);
    drawStatusDots();
    emitStaticScreenTrace();
    return;
  }

  if (gShowNoWifi) {
    const bool demoNoWifi = isDemoNoWifiScreen();

    initPanelFxSpriteOnce();
    TFT_eSprite* spr = gPanelFxSpriteOk ? &gPanelFxSprite : nullptr;

    // Full-screen static screen.
    setupKetosisLabelColorsOnce();
    applyPanelViewport();
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    tft.fillRect(0, 0, kPanelW, kPanelH, TFT_BLACK);
    if (spr) { spr->fillSprite(TFT_BLACK); }

    // Title banner (like the HIGH KETOSIS bar, but at the top)
    const int bannerH = 16;
    tft.fillRect(0, 0, kPanelW, bannerH, gKetosisRed565);
    tft.setTextColor(TFT_WHITE, gKetosisRed565);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.drawString("NO WIFI", kPanelW / 2, bannerH / 2);
    if (spr) {
      spr->fillRect(0, 0, kPanelW, bannerH, gKetosisRed565);
      spr->setTextColor(TFT_WHITE, gKetosisRed565);
      spr->setTextDatum(MC_DATUM);
      spr->setTextSize(1);
      spr->drawString("NO WIFI", kPanelW / 2, bannerH / 2);
    }

    if (demoNoWifi) {
      const int quiet = 2;
      const int scale = 2;
      const int qrPx = (29 + 2 * quiet) * scale;
      const int qrX = (kPanelW - qrPx) / 2;
      const int qrY = 104;
      drawQrCodeFromText("WIFI:T:nopass;S:ble-health-hub;;", qrX, qrY, scale, quiet, spr);

      tft.setTextDatum(TC_DATUM);
      tft.setTextSize(1);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString("SCAN QR", kPanelW / 2, qrY + qrPx + 8);
      if (spr) {
        spr->setTextDatum(TC_DATUM);
        spr->setTextSize(1);
        spr->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        spr->drawString("SCAN QR", kPanelW / 2, qrY + qrPx + 8);
      }
    } else {
      // WiFiManager setup QR (join the portal AP).
      const int quiet = 2;
      const int scale = 2;
      const int qrPx = (29 + 2 * quiet) * scale;
      const int qrX = (kPanelW - qrPx) / 2;
      const int qrY = 120;
      drawQrCodeFromText(gPortalQrPayload, qrX, qrY, scale, quiet, spr);

      tft.setTextDatum(TC_DATUM);
      tft.setTextSize(1);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString("SCAN QR", kPanelW / 2, qrY + qrPx + 8);
      if (spr) {
        spr->setTextDatum(TC_DATUM);
        spr->setTextSize(1);
        spr->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        spr->drawString("SCAN QR", kPanelW / 2, qrY + qrPx + 8);
      }
    }

    tft.setTextDatum(TL_DATUM);
    drawStatusDots();
    emitStaticScreenTrace();
    return;
  }

  // Keep it simple and legible: opaque black background.
  setupKetosisLabelColorsOnce();
  applyPanelViewport();
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  // Full-screen label FX (runs after the slide-in completes).
  if (!gShowLoading && !gShowNoWifi && isLabelFxActive() && (gKetosisLabel.length() > 0)) {
    initPanelFxSpriteOnce();
    const uint32_t now = millis();
    static constexpr uint32_t kExpandMs = 140;
    static constexpr uint32_t kHoldMs = 1000;

    uint8_t state = gLabelFxState;
    uint32_t phaseStart = gLabelFxPhaseStartMs;

    // Advance phases.
    if (state == kLabelFxExpand && (now - phaseStart) >= kExpandMs) {
      state = kLabelFxHold;
      phaseStart = now;
      gLabelFxState = state;
      gLabelFxPhaseStartMs = phaseStart;
    } else if (state == kLabelFxHold && (now - phaseStart) >= kHoldMs) {
      gLabelFxState = kLabelFxNone;
      gOverlayDirty = true;
      gStatusStripDirty = true;
      // Snap back to normal bar: fall through and draw the regular overlay.
      state = kLabelFxNone;
    }

    // Compute vertical extent.
    const int barH = 16;
    const int barY = kOverlayH - barH;
    const float centerY = (float)barY + ((float)barH * 0.5f);

    // Keep label text pinned to its normal position (does not move during FX).
    const int labelCy = barY + (barH / 2);

    float p = 0.0f;
    if (state == kLabelFxExpand) {
      p = (float)(now - phaseStart) / (float)kExpandMs;
      p = max(0.0f, min(1.0f, p));
      // Ease-in so it feels like it accelerates.
      p = p * p * p;
    } else if (state == kLabelFxHold) {
      p = 1.0f;
    } else {
      // Snapped back.
      p = 0.0f;
    }
    const int h = (int)lroundf(((float)barH) + (((float)kPanelH - (float)barH) * p));
    int y0 = (int)lroundf(centerY - ((float)h * 0.5f));
    int y1 = y0 + h;
    if (y0 < 0) {
      y1 -= y0;
      y0 = 0;
    }
    if (y1 > kPanelH) {
      const int overflow = y1 - kPanelH;
      y0 -= overflow;
      y1 = kPanelH;
      if (y0 < 0) y0 = 0;
    }

    if (gPanelFxSpriteOk) {
      gPanelFxSprite.fillSprite(TFT_BLACK);
      gPanelFxSprite.fillRect(0, y0, kPanelW, max(0, y1 - y0), gKetosisLabelBg);

      gPanelFxSprite.setTextSize(1);
      gPanelFxSprite.setTextColor(gKetosisLabelFg, gKetosisLabelBg);
      gPanelFxSprite.setTextDatum(MC_DATUM);
      gPanelFxSprite.drawString(gKetosisLabel, kPanelW / 2, labelCy);
      gPanelFxSprite.setTextDatum(TL_DATUM);

      gPanelFxSprite.pushSprite(0, 0);
    } else {
      // Fallback (no sprite): draw directly.
      tft.fillRect(0, 0, kPanelW, kPanelH, TFT_BLACK);
      tft.fillRect(0, y0, kPanelW, max(0, y1 - y0), gKetosisLabelBg);
      tft.setTextSize(1);
      tft.setTextColor(gKetosisLabelFg, gKetosisLabelBg);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(gKetosisLabel, kPanelW / 2, labelCy);
      tft.setTextDatum(TL_DATUM);
    }

    // Keep animating smoothly.
    if (state != kLabelFxNone) {
      gOverlayDirty = true;
      return;
    }
  }

  // If we're revealing, prefer an offscreen sprite to avoid visible partial
  // redraw flicker.
  const bool willUseSprite = (!gShowLoading && gOverlayRevealActive && gOverlaySpriteOk);
  if (!gShowLoading) {
    if (!willUseSprite) {
      tft.fillRect(0, 0, kPanelW, kOverlayH, TFT_BLACK);
    }

    // Ensure the area below the overlay starts black at least once.
    static bool clearedAnimArea = false;
    if (!clearedAnimArea) {
      clearedAnimArea = true;
      if (kPanelH > kOverlayH) {
        tft.fillRect(0, kOverlayH, kPanelW, kPanelH - kOverlayH, TFT_BLACK);
      }
    }
  } else {
    // LOADING: only clear a thin status strip; let the animation occupy the rest.
    tft.fillRect(0, 0, kPanelW, kLoadingStatusH, TFT_BLACK);
  }

  const float gki = getDisplayedGki();

  const bool reveal = (!gShowLoading && gOverlayRevealActive);
  const uint32_t revealT = reveal ? (millis() - (uint32_t)gOverlayRevealStartMs) : 0;
  // Three groups: GLU, then KET, then (GKI + ketosis label).
  static constexpr uint32_t kRevealDurMs = 180;
  static constexpr uint32_t kRevealGapMs = 80;
  const uint32_t dGlu = 0;
  const uint32_t dKet = dGlu + kRevealDurMs + kRevealGapMs;
  const uint32_t dGki = dKet + kRevealDurMs + kRevealGapMs;

  auto easeOutBack01 = [&](float p) -> float {
    // Ease-out with a small overshoot ("bounce") at the end.
    // p in [0..1] -> returns roughly [0..1].
    const float c1 = 1.35f;
    const float c3 = c1 + 1.0f;
    const float x = p - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
  };

  auto slideX = [&](uint32_t delayMs) -> int {
    if (!reveal) return 0;
    if (revealT <= delayMs) return -kPanelW;
    const uint32_t t = revealT - delayMs;
    if (t >= kRevealDurMs) return 0;
    float p = (float)t / (float)kRevealDurMs;
    p = max(0.0f, min(1.0f, p));
    const float e = easeOutBack01(p);
    // Start at -W, end at 0, with small overshoot.
    const float x = (-((float)kPanelW)) * (1.0f - e);
    // Clamp overshoot so text doesn't fly too far.
    return (int)lroundf(max(-((float)kPanelW), min(8.0f, x)));
  };

  // When revealing, render the full overlay into an offscreen sprite and push
  // it in one go (prevents visible partial redraw flicker).
  const bool useSprite = (reveal && gOverlaySpriteOk);

  auto drawMetricTft = [&](int y, int x, const char* label, const String& value) {
    y += kOverlayTextPadTopPx;
    tft.setTextSize(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(label, x, y);

    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(value, x, y + 14);
  };

  auto drawMetricSprite = [&](int y, int x, const char* label, const String& value) {
    y += kOverlayTextPadTopPx;
    gOverlaySprite.setTextSize(1);
    gOverlaySprite.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    gOverlaySprite.drawString(label, x, y);

    gOverlaySprite.setTextSize(2);
    gOverlaySprite.setTextColor(TFT_WHITE, TFT_BLACK);
    gOverlaySprite.setTextDatum(TL_DATUM);
    gOverlaySprite.drawString(value, x, y + 14);
  };

  if (useSprite) {
    gOverlaySprite.setTextDatum(TL_DATUM);
    gOverlaySprite.setTextSize(1);
    gOverlaySprite.fillSprite(TFT_BLACK);
  }

  if (!gShowLoading) {
    const int xGlu = 2 + slideX(dGlu);
    const int xKet = 2 + slideX(dKet);
    const int xGki = 2 + slideX(dGki);

    if (!reveal || revealT >= dGlu) {
      if (useSprite) {
        drawMetricSprite(2, xGlu, "GLU", String(gGluMgDl, 0));
      } else {
        drawMetricTft(2, xGlu, "GLU", String(gGluMgDl, 0));
      }
    }
    if (!reveal || revealT >= dKet) {
      if (useSprite) {
        drawMetricSprite(58, xKet, "KET", String(gKetMmolL, 1));
      } else {
        drawMetricTft(58, xKet, "KET", String(gKetMmolL, 1));
      }
    }
    if (!reveal || revealT >= dGki) {
      if (useSprite) {
        drawMetricSprite(114, xGki, "GKI", String(gki, 1));
      } else {
        drawMetricTft(114, xGki, "GKI", String(gki, 1));
      }
    }
  }

  if (gTopStatus.length() > 0) {
    if (useSprite) {
      gOverlaySprite.setTextSize(1);
      gOverlaySprite.setTextColor(TFT_WHITE, TFT_BLACK);
      gOverlaySprite.setTextDatum(TC_DATUM);
      gOverlaySprite.drawString(gTopStatus, kPanelW / 2, 2 + kOverlayTextPadTopPx);
      gOverlaySprite.setTextDatum(TL_DATUM);
    } else {
      tft.setTextSize(1);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextDatum(TC_DATUM);
      tft.drawString(gTopStatus, kPanelW / 2, 2 + kOverlayTextPadTopPx);
      tft.setTextDatum(TL_DATUM);
    }
  }

  // Ketosis label bar: full width background rectangle.
  if (gKetosisLabel.length() > 0) {
    const int barH = 16;
    const int barY = kOverlayH - barH;
    if (!reveal || revealT >= dGki) {
      const int xBar = slideX(dGki);
      const int l = max(0, xBar);
      const int r = min(kPanelW, xBar + kPanelW);
      if (r > l) {
        if (useSprite) {
          gOverlaySprite.fillRect(l, barY, r - l, barH, gKetosisLabelBg);
        } else {
          tft.fillRect(l, barY, r - l, barH, gKetosisLabelBg);
        }
      }
      if (useSprite) {
        gOverlaySprite.setTextSize(1);
        gOverlaySprite.setTextColor(gKetosisLabelFg, gKetosisLabelBg);
        gOverlaySprite.setTextDatum(MC_DATUM);
        gOverlaySprite.drawString(gKetosisLabel, (kPanelW / 2) + xBar, barY + (barH / 2));
        gOverlaySprite.setTextDatum(TL_DATUM);
      } else {
        tft.setTextSize(1);
        tft.setTextColor(gKetosisLabelFg, gKetosisLabelBg);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(gKetosisLabel, (kPanelW / 2) + xBar, barY + (barH / 2));
        tft.setTextDatum(TL_DATUM);
      }
    }
  }

  if (useSprite) {
    // Push the overlay sprite into the panel viewport.
    gOverlaySprite.pushSprite(0, 0);
  }

  if (reveal && revealT >= (dGki + kRevealDurMs + 5)) {
    gOverlayRevealActive = false;
    startLabelFxIfPossible();
    gOverlayDirty = true;
  }

  drawStatusDots();
}

static void fillTestPattern565(uint16_t* buf) {
  // High-contrast stripes + a gradient footer. Should be visible even if byte
  // swapping is wrong (colors may change, but pattern should appear).
  const int w = kPanelW;
  const int h = kPanelH;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint16_t c;
      if (y < (h - 20)) {
        const int band = (x / 8) % 3;
        if (band == 0) {
          c = TFT_RED;
        } else if (band == 1) {
          c = TFT_GREEN;
        } else {
          c = TFT_BLUE;
        }
      } else {
        // 20px footer: horizontal grayscale gradient
        const uint8_t v = (uint8_t)((x * 255) / max(1, w - 1));
        const uint32_t argb = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
        c = rgba8888ToRgb565(argb);
      }
      buf[(size_t)y * (size_t)w + (size_t)x] = c;
    }
  }
}

static void pushTestPatternOnce() {
  if (!gTest565) {
    const size_t px = (size_t)kPanelW * (size_t)kPanelH;
    gTest565 = (uint16_t*)mallocFrameBuffer(px * sizeof(uint16_t));
    if (!gTest565) {
      Serial.println("TEST: failed to allocate gTest565");
      return;
    }
  }
  fillTestPattern565(gTest565);

  // 1) Viewport-relative (if pushImage honors viewport datum)
  Serial.println("TEST: pushImage viewport-relative @ (0,0)");
  applyPanelViewport();
  tft.setSwapBytes(kSwapBytesForPushImage);
  tft.startWrite();
  tft.pushImage(0, 0, kPanelW, kPanelH, gTest565);
  tft.endWrite();
  delay(500);

  // 2) Absolute (if pushImage ignores viewport datum)
  int ax = 0;
  int ay = 0;
  getPanelAbsXY(&ax, &ay);
  Serial.printf("TEST: pushImage absolute @ (%d,%d)\n", ax, ay);
  tft.setSwapBytes(kSwapBytesForPushImage);
  tft.startWrite();
  tft.pushImage(ax, ay, kPanelW, kPanelH, gTest565);
  tft.endWrite();
  delay(500);
}

static void applyPanelViewport() {
  // Rotation must be set before calling width()/height() because it changes them.
  tft.setRotation(0);
  tft.resetViewport();
  const int canvasW = (int)tft.width();
  const int canvasH = (int)tft.height();
  const int x = max(0, min(kPanelXOff, max(0, canvasW - kPanelW)));
  const int maxY = max(0, canvasH - kPanelH);
  const int y = max(0, min(kPanelYOff, maxY));
  tft.setViewport(x, y, kPanelW, kPanelH, true);
}

static void getPanelAbsXY(int* outX, int* outY) {
  // Compute the physical window location in controller coordinates.
  // Note: pushImage() may ignore viewport datum, so animation blits should use
  // absolute coordinates.
  tft.setRotation(0);
  tft.resetViewport();
  const int canvasW = (int)tft.width();
  const int canvasH = (int)tft.height();
  const int x = max(0, min(kPanelXOff, max(0, canvasW - kPanelW)));
  const int maxY = max(0, canvasH - kPanelH);
  const int y = max(0, min(kPanelYOff, maxY));
  *outX = x;
  *outY = y;
}

static void* mallocFrameBuffer(size_t bytes) {
#if defined(MALLOC_CAP_SPIRAM)
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  return malloc(bytes);
}

static void showLottieStatus(const char* msg, uint16_t color) {
  applyPanelViewport();
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  // Draw below the boot screen lines.
  tft.fillRect(0, 62, kPanelW, 16, TFT_BLACK);
  tft.drawString(msg, 2, 62);
}

static void initLottie() {
  gLastLottieInitAttemptMs = millis();
  gLottieReady = false;
  gAnim.reset();

  if (gFrameArgb) {
    free(gFrameArgb);
    gFrameArgb = nullptr;
  }
  if (gFrame565) {
    free(gFrame565);
    gFrame565 = nullptr;
  }

  // Build a std::string from the generated bytes.
  const char* data = reinterpret_cast<const char*>(assets::kFlameLottieJsonBytes);
  std::string json(data, data + assets::kFlameLottieJsonSize);

  gAnim = rlottie::Animation::loadFromData(json, "flame");
  if (!gAnim) {
    Serial.println("RLottie: failed to load animation");
    showLottieStatus("RLottie: load FAIL", TFT_RED);
    return;
  }

  // Try to respect source frame rate.
  // (If the API differs, the build will fail and we'll adjust.)
  gAnimFps = (double)gAnim->frameRate();
  gAnimTotalFrames = (size_t)gAnim->totalFrame();
  if (gAnimFps <= 1.0) gAnimFps = 30.0;
  if (gAnimTotalFrames == 0) gAnimTotalFrames = 1;

  const size_t px = (size_t)kPanelW * (size_t)kPanelH;
  gFrameArgb = (uint32_t*)mallocFrameBuffer(px * sizeof(uint32_t));
  gFrame565 = (uint16_t*)mallocFrameBuffer(px * sizeof(uint16_t));
  if (!gFrameArgb || !gFrame565) {
    Serial.println("RLottie: framebuffer alloc failed");
    showLottieStatus("RLottie: alloc FAIL", TFT_RED);
    return;
  }

  gAnimStartMs = millis();
  Serial.printf("RLottie: loaded (fps=%.2f frames=%u)\n", gAnimFps, (unsigned)gAnimTotalFrames);
  showLottieStatus("RLottie: OK", TFT_GREEN);
  gLottieReady = true;
  gOverlayDirty = true;

  // Cache layer names for demo-mode visibility toggles.
  gLayerNames.clear();
  for (const auto& info : gAnim->layers()) {
    const std::string& name = std::get<0>(info);
    if (!name.empty()) gLayerNames.push_back(name);
  }

  // Default loop segment for demo mode.
  setLoopToFlameBurn();
}

static void setLayerOpacityPct(const std::string& layerName, float opacityPct) {
  if (!gAnim) return;

  auto normalize = [](const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
      const unsigned char uch = (unsigned char)ch;
      if (uch <= 0x20) continue;  // drop whitespace/control
      if (ch == '_' || ch == '-') continue;
      if (ch >= 'A' && ch <= 'Z') out.push_back((char)(ch - 'A' + 'a'));
      else out.push_back(ch);
    }
    return out;
  };

  const std::string wantNorm = normalize(layerName);
  const std::string* resolved = nullptr;
  for (const auto& name : gLayerNames) {
    if (name == layerName) {
      resolved = &name;
      break;
    }
    if (normalize(name) == wantNorm) {
      resolved = &name;
      break;
    }
  }

  const std::string& key = resolved ? *resolved : layerName;
  if (!resolved) {
    static std::set<std::string> sWarned;
    if (sWarned.insert(layerName).second) {
      Serial.printf("DEMO: layer not found (using raw keypath): '%s'\n", layerName.c_str());
    }
  }

  // TrOpacity expects [0..100]
  // Resolve from the root reliably (regardless of root container naming).
  gAnim->setValue<rlottie::Property::TrOpacity>("**." + key, opacityPct);
}

static void setAllLayersOpacityPct(float opacityPct) {
  if (!gAnim) return;
  if (gLayerNames.empty()) {
    // Fallback if layer list is empty.
    gAnim->setValue<rlottie::Property::TrOpacity>("**", opacityPct);
    return;
  }
  for (const auto& name : gLayerNames) {
    setLayerOpacityPct(name, opacityPct);
  }
}

static void applyUiStepVisual(int step) {
  // Visual-only stage applier (no measurement values). Called from renderTask()
  // so RLottie setValue() calls are serialized.
  gShowLoading = false;
  gShowNoWifi = false;
  gShowLowBattery = false;
  gShowPulseOx = false;
  gShowGoodbye = false;
  gSuppressStatusTime = false;
  gLabelFxState = kLabelFxNone;

  switch (step) {
    case 0: {
      gTopStatus = "";
      gKetosisLabel = "NOT KETOSIS";
      gKetosisLabelBg = gKetosisBrown565;
      gKetosisLabelFg = TFT_WHITE;

      setAllLayersOpacityPct(0.0f);
      setLayerOpacityPct("Logs", 100.0f);
      setLoopToFlameBurn();
      break;
    }
    case 1: {
      gTopStatus = "";
      gKetosisLabel = "LOW KETOSIS";
      gKetosisLabelBg = gKetosisOrange565;
      gKetosisLabelFg = TFT_WHITE;

      setAllLayersOpacityPct(0.0f);
      setLayerOpacityPct("Logs", 100.0f);
      setLayerOpacityPct("Orange Container", 100.0f);
      setLayerOpacityPct("Yellow Container", 100.0f);
      setLoopToFlameBurn();
      break;
    }
    case 2: {
      gTopStatus = "";
      gKetosisLabel = "MID KETOSIS";
      gKetosisLabelBg = gKetosisYellow565;
      gKetosisLabelFg = TFT_BLACK;

      setAllLayersOpacityPct(100.0f);
      setLayerOpacityPct("BigRed Container", 0.0f);
      setLayerOpacityPct("SideRed Container", 0.0f);
      setLoopToFlameBurn();
      break;
    }
    case 3: {
      gTopStatus = "";
      gKetosisLabel = "HIGH KETOSIS";
      gKetosisLabelBg = gKetosisRed565;
      gKetosisLabelFg = TFT_WHITE;

      setAllLayersOpacityPct(100.0f);
      setLoopToFlameBurn();
      break;
    }
    case 6: {
      gTopStatus = "";
      gKetosisLabel = "!!! KETOSIS";
      gKetosisLabelBg = gKetosisRed565;
      gKetosisLabelFg = TFT_WHITE;

      setAllLayersOpacityPct(100.0f);
      setLoopToFlameBurn();
      break;
    }
    case 4: {
      gShowLoading = true;
      gTopStatus = "CONNECTING";
      gKetosisLabel = "";
      gKetosisLabelBg = TFT_BLACK;
      gKetosisLabelFg = TFT_WHITE;

      setAllLayersOpacityPct(0.0f);
      setLayerOpacityPct("Logs", 100.0f);
      setLoopToLogLoading();
      break;
    }
    case 5: {
      gShowNoWifi = true;
      gTopStatus = "NO WIFI";
      gKetosisLabel = "";
      gKetosisLabelBg = TFT_BLACK;
      gKetosisLabelFg = TFT_WHITE;

      setAllLayersOpacityPct(0.0f);
      break;
    }
    case 7: {
      // Pulse Ox demo screen (LVGL).
      gShowPulseOx = true;
      gSuppressStatusTime = true;
      gTopStatus = "";
      gKetosisLabel = "";
      gKetosisLabelBg = TFT_BLACK;
      gKetosisLabelFg = TFT_WHITE;
      setAllLayersOpacityPct(0.0f);
      break;
    }
    case 8: {
      gShowLowBattery = true;
      gTopStatus = "LOW BATTERY";
      gKetosisLabel = "";
      gKetosisLabelBg = TFT_BLACK;
      gKetosisLabelFg = TFT_WHITE;
      setAllLayersOpacityPct(0.0f);
      break;
    }
    case 9: {
      gShowGoodbye = true;
      gTopStatus = "";
      gKetosisLabel = "";
      gKetosisLabelBg = TFT_BLACK;
      gKetosisLabelFg = TFT_WHITE;
      setAllLayersOpacityPct(0.0f);

      // Start the GOODBYE animation sequence.
      gGoodbyePhase = kGoodbyeConfetti;
      gGoodbyeAnimStartMs = millis();
      gGoodbyeBannerStartMs = 0;
      gGoodbyePhaseStartMs = gGoodbyeAnimStartMs;
      gGoodbyeAnimDone = false;
      gOverlayDirty = true;
      break;
    }
    default:
      break;
  }

  gOverlayDirty = true;
  gStatusStripDirty = true;

  pulseox_demo_lvgl_set_active(gShowPulseOx);
}

static void applyDemoStep(int step) {
  // Demo mode shows a sample time so we can preview bottom-row timestamp.
  snprintf(gGkWhenLocal, sizeof(gGkWhenLocal), "%02d:%02d", 10, 26);

  // Step meanings per user spec.
  switch (step) {
    case 0: {
      gGluMgDl = 81.0f;
      gKetMmolL = 0.3f;
      gGkiOverride = true;
      gGkiValue = 15.0f;
      // Visuals handled by applyUiStepVisual().
      break;
    }
    case 1: {
      gGluMgDl = 109.0f;
      gKetMmolL = 1.0f;
      gGkiOverride = true;
      gGkiValue = 6.0f;
      // Visuals handled by applyUiStepVisual().
      break;
    }
    case 2: {
      gGluMgDl = 110.0f;
      gKetMmolL = 1.2f;
      gGkiOverride = true;
      gGkiValue = 5.1f;
      // Visuals handled by applyUiStepVisual().
      break;
    }
    case 3: {
      gGluMgDl = 82.0f;
      gKetMmolL = 3.6f;
      gGkiOverride = true;
      gGkiValue = 1.3f;
      // Visuals handled by applyUiStepVisual().
      break;
    }
    case 4: {
      // Visuals handled by applyUiStepVisual().
      break;
    }
    case 5: {
      // Visuals handled by applyUiStepVisual().
      break;
    }
    case 7: {
      // Pulse ox demo has its own visuals; metrics are drawn by LVGL.
      break;
    }
    case 8: {
      // Visuals handled by applyUiStepVisual().
      break;
    }
    case 9: {
      // GOOD BYE (visual-only)
      break;
    }
    default:
      break;
  }

  applyUiStepVisual(step);

  // Give demo stages some life too: slide values in for ketosis stages.
  // (Loading has no metrics; NO WIFI is a separate full-screen layout.)
  if (step == 0 || step == 1 || step == 2 || step == 3) {
    startOverlayReveal();
  }
}

static void renderTask(void* /*param*/) {
  // RLottie's render pipeline can be stack-hungry. Running it inside Arduino's
  // default loopTask has been observed to overflow the stack on ESP32-S3.
  // This task is created with a larger stack.
  int appliedStep = -1;
  for (;;) {
    const uint32_t now = millis();

    if (!gLottieReady && (now - gLastLottieInitAttemptMs) > 3000) {
      initLottie();
    }

    // Serial command: respond to "PINS?" with current gate states.
    while (Serial.available()) {
      static char sCmdBuf[16];
      static int sCmdLen = 0;
      char ch = (char)Serial.read();
      if (ch == '\n' || ch == '\r') {
        sCmdBuf[sCmdLen] = '\0';
        if (strcmp(sCmdBuf, "PINS?") == 0) {
          Serial.printf("PINS: GK(GPIO%d)=%s  PulseOx(GPIO%d)=%s  Util(GPIO%d)=%s  FBTrace(GPIO%d)=%s\n",
                        kGkDemoGateGpio, isGkDemoGateActive() ? "ON" : "OFF",
                        kPulseOxDemoGateGpio, isPulseOxDemoGateActive() ? "ON" : "OFF",
                        kUtilityDemoGateGpio, isUtilityDemoGateActive() ? "ON" : "OFF",
                        kFbTraceGpio, fbTraceGateActive() ? "ON" : "OFF");
        }
        sCmdLen = 0;
      } else if (sCmdLen < (int)sizeof(sCmdBuf) - 1) {
        sCmdBuf[sCmdLen++] = ch;
      }
    }

    const bool gkGate = isGkDemoGateActive();
    const bool pulseGate = isPulseOxDemoGateActive();
    const bool utilGate = isUtilityDemoGateActive();
    const bool anyGate = gkGate || pulseGate || utilGate;
    static bool sLastGkGate = false;
    static bool sLastPulseGate = false;
    static bool sLastUtilGate = false;
    if (gkGate != sLastGkGate || pulseGate != sLastPulseGate || utilGate != sLastUtilGate) {
      sLastGkGate = gkGate;
      sLastPulseGate = pulseGate;
      sLastUtilGate = utilGate;
      // Reset demo timing when switching gates/modes.
      gDemoStartMs = 0;
      appliedStep = -1;
      Serial.printf("DEMO: GK(GPIO%d)=%s  PulseOx(GPIO%d)=%s  Util(GPIO%d)=%s\n",
                    kGkDemoGateGpio, gkGate ? "ON" : "OFF",
                    kPulseOxDemoGateGpio, pulseGate ? "ON" : "OFF",
                    kUtilityDemoGateGpio, utilGate ? "ON" : "OFF");
    }

    // PulseOx demo should run even if RLottie isn't ready.
    // If both demo gates are grounded, PulseOx takes precedence.
    if (kDemoMode && pulseGate) {
      const int nextStep = 7;
      if (nextStep != appliedStep) {
        appliedStep = nextStep;
        Serial.printf("DEMO: step=%d\n", appliedStep);
        applyDemoStep(appliedStep);
      }
    }

    // Utility demo carousel should run even if RLottie isn't ready.
    // Priority: PulseOx > Utility > GK+.
    if (kDemoMode && utilGate && !pulseGate) {
      if (gDemoStartMs == 0) {
        gDemoStartMs = now;
      }
      static constexpr int kUtilSteps = 3;
      static constexpr int kUtilOrder[kUtilSteps] = {5, 8, 9};
      const int idx = (int)(((now - gDemoStartMs) / kDemoStepMs) % kUtilSteps);
      const int nextStep = kUtilOrder[idx];
      if (nextStep != appliedStep) {
        appliedStep = nextStep;
        Serial.printf("DEMO: step=%d\n", appliedStep);
        applyDemoStep(appliedStep);
      }
    }

    if (gLottieReady) {
      if (kDemoMode && gkGate && !pulseGate && !utilGate) {
        if (gDemoStartMs == 0) {
          gDemoStartMs = now;
        }
        static constexpr int kDemoSteps = 5;
        // GK+ demo cycle: Loading -> Not -> Low -> Mid -> High
        static constexpr int kDemoOrder[kDemoSteps] = {4, 0, 1, 2, 3};
        const int idx = (int)(((now - gDemoStartMs) / kDemoStepMs) % kDemoSteps);
        const int nextStep = kDemoOrder[idx];
        if (nextStep != appliedStep) {
          appliedStep = nextStep;
          Serial.printf("DEMO: step=%d\n", appliedStep);
          applyDemoStep(appliedStep);
        }
      } else if (kDemoMode && anyGate) {
        // A demo gate is active, so ignore background-requested UI steps.
        // (Otherwise normal mode would fight with demo selection and immediately
        // override e.g. PulseOx step 7 back to LOADING step 4.)
      } else {
        // Normal mode: apply the step requested by background tasks (e.g. GK+).
        int req = 4;
        bool dirty = false;
        portENTER_CRITICAL(&gUiMux);
        req = gUiRequestedStep;
        dirty = gUiRequestedStepDirty;
        gUiRequestedStepDirty = false;
        portEXIT_CRITICAL(&gUiMux);

        if (gForceNoWifiScreen) {
          req = 5;
        }

        if (dirty || req != appliedStep) {
          appliedStep = req;
          Serial.printf("UI: step=%d\n", appliedStep);
          applyUiStepVisual(appliedStep);
        }
      }
    }

    // Pulse Ox LVGL demo can run even if RLottie isn't ready.
    if (gShowPulseOx) {
      // Ensure LVGL is initialized.
      pulseox_demo_lvgl_init(&tft, kPanelW, kPanelH, kPanelXOff, kPanelYOff, kSwapBytesForPushImage, kSwapRedBlueInConvert);
      pulseox_demo_lvgl_pump(now);
      // Reuse the same bottom status strip (time + WiFi/BLE dots) as GK+.
      drawStatusDots();
      vTaskDelay(pdMS_TO_TICKS(16));
      continue;
    }

    if (gLottieReady) {
      if (gShowNoWifi || gShowLowBattery || gShowGoodbye) {
        // Static screens: avoid continuously overwriting them with animation pushes.
        // GOODBYE is animated, so redraw it while its animation is running.
        const bool goodbyeAnimating = gShowGoodbye && (gGoodbyePhase != kGoodbyeDone);
        if (gOverlayDirty || goodbyeAnimating) {
          gOverlayDirty = false;
          drawOverlay();
        }
        vTaskDelay(pdMS_TO_TICKS(goodbyeAnimating ? 33 : 50));
        continue;
      }

      renderAndPushLottieFrame();
      // Throttle a bit; actual frame selection is time-based in renderAndPushLottieFrame().
      const uint32_t minFrameMs = (uint32_t)max(5.0, (1000.0 / max(1.0, gAnimFps)));
      vTaskDelay(pdMS_TO_TICKS(minFrameMs));
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

static void renderAndPushLottieFrame() {
  if (!gAnim || !gFrameArgb || !gFrame565) return;

  const bool labelFxActive = isLabelFxActive();
  const bool suppressAnimForReveal = (!gShowLoading && (gOverlayRevealActive || labelFxActive));
  static bool sAnimPausedForReveal = false;
  static bool sAnimAreaClearedForReveal = false;
  static uint32_t sAnimPauseStartMs = 0;

  // Pause the animation timeline entirely while the reveal or label wipe is
  // active so the flames neither advance off-screen nor require repeated clears.
  if (suppressAnimForReveal) {
    if (!sAnimPausedForReveal) {
      sAnimPausedForReveal = true;
      sAnimPauseStartMs = millis();
    }
  } else if (sAnimPausedForReveal) {
    const uint32_t pausedMs = millis() - sAnimPauseStartMs;
    gAnimStartMs += pausedMs;
    sAnimPausedForReveal = false;
    sAnimAreaClearedForReveal = false;
  }

  // Push to display.
  int x = 0;
  int y = 0;
  getPanelAbsXY(&x, &y);

  // Only push the animation below the overlay header so UI text persists.
  // For LOADING, reserve a small status strip at the top so the label remains
  // stable without needing per-frame redraws.
  const int animY0 = gShowLoading ? kLoadingStatusH : max(0, min(kOverlayH, kPanelH));
  // Also reserve a bottom status strip for time + dots.
  const int animH = max(0, (kPanelH - kBottomStatusH) - animY0);

  if (suppressAnimForReveal) {
    if (!sAnimAreaClearedForReveal && animH > 0) {
      tft.fillRect(x, y + animY0, kPanelW, animH, TFT_BLACK);
      sAnimAreaClearedForReveal = true;
    }

    if (gOverlayDirty || gOverlayRevealActive || labelFxActive) {
      gOverlayDirty = false;
      drawOverlay();
    }

    if (!labelFxActive) {
      drawStatusDots();
    }
    return;
  }

  const uint32_t now = millis();
  const uint32_t elapsedMs = now - gAnimStartMs;
  const double frameF = (elapsedMs * gAnimFps) / 1000.0;
  const size_t total = (size_t)max<size_t>(1, gAnimTotalFrames);
  const size_t loopFrames = (size_t)max<size_t>(1, min((gLoopFrameCount > 0) ? gLoopFrameCount : kLottieLoopFrames, total));
  const size_t base = (gLoopStartFrame < total) ? gLoopStartFrame : 0;
  const size_t frame = base + (size_t)((uint64_t)frameF % (uint64_t)loopFrames);

  // Render into ARGB8888 (premultiplied).
  // Clear to black first; RLottie doesn't necessarily overwrite every pixel.
  const size_t px = (size_t)kPanelW * (size_t)kPanelH;
  for (size_t i = 0; i < px; i++) {
    gFrameArgb[i] = 0xFF000000u;
  }
  rlottie::Surface surface(gFrameArgb, (size_t)kPanelW, (size_t)kPanelH, (size_t)kPanelW * sizeof(uint32_t));
  gAnim->renderSync(frame, surface, true);

  if (kKeyOutOpaqueWhiteBackground) {
    // If the animation includes an opaque "white matte" background, key it out.
    // We detect this by checking if all four corners are near-white.
    const size_t w = (size_t)kPanelW;
    const size_t h = (size_t)kPanelH;
    const uint32_t c00 = gFrameArgb[0];
    const uint32_t c10 = gFrameArgb[w - 1];
    const uint32_t c01 = gFrameArgb[(h - 1) * w + 0];
    const uint32_t c11 = gFrameArgb[(h - 1) * w + (w - 1)];

    const bool cornersNearWhite = isNearWhiteRgb(c00, 250) && isNearWhiteRgb(c10, 250) &&
                                 isNearWhiteRgb(c01, 250) && isNearWhiteRgb(c11, 250);

    if (cornersNearWhite) {
      // Use the top-left corner as the background RGB to key out *exactly*.
      const uint32_t bg = c00;
      static bool printed = false;
      if (!printed) {
        printed = true;
        Serial.printf("KeyOut: detected white background rgb=0x%06x\n", (unsigned)(bg & 0x00FFFFFFu));
      }

      // Replace exact background color with black. Keep highlights that aren't exact #FFFFFF.
      const size_t px = (size_t)kPanelW * (size_t)kPanelH;
      for (size_t i = 0; i < px; i++) {
        const uint32_t p = gFrameArgb[i];
        if (rgbExactly(p, bg) && isNearWhiteRgb(p, 250)) {
          gFrameArgb[i] = 0xFF000000u;
        }
      }
    }
  }

  // Enforce black background: any near-transparent pixel becomes black.
  // This avoids "white matte" artifacts from transparent pixels.
  const uint32_t kAlphaCutoff = 12; // keep anti-aliased edges, but kill near-zero alpha
  for (size_t i = 0; i < px; i++) {
    const uint32_t p = gFrameArgb[i];
    const uint32_t a = (p >> 24) & 0xFF;
    if (a <= kAlphaCutoff) {
      gFrameArgb[i] = 0xFF000000u;
    }
  }

  // Convert to RGB565.
  for (size_t i = 0; i < px; i++) {
    gFrame565[i] = rgba8888ToRgb565(gFrameArgb[i]);
  }

  // Lightweight sanity stats (prints ~2x/sec) to confirm RLottie is producing pixels.
  if (kDebugPixelStats) {
    static uint32_t lastStatMs = 0;
    if (lastStatMs == 0 || (now - lastStatMs) > 500) {
      lastStatMs = now;
      uint32_t nonZeroAlpha = 0;
      uint32_t nonBlack = 0;
      uint32_t maxRgb = 0;
      for (size_t i = 0; i < px; i++) {
        const uint32_t p = gFrameArgb[i];
        const uint32_t a = (p >> 24) & 0xFF;
        const uint32_t rgb = p & 0x00FFFFFFu;
        if (a) nonZeroAlpha++;
        if (rgb) nonBlack++;
        if (rgb > maxRgb) maxRgb = rgb;
      }
      Serial.printf("RLottie frame=%u stats: nonZeroA=%u/%u nonBlack=%u/%u maxRgb=0x%06x\n",
                    (unsigned)frame, (unsigned)nonZeroAlpha, (unsigned)px, (unsigned)nonBlack, (unsigned)px,
                    (unsigned)(maxRgb & 0x00FFFFFFu));
    }
  }

  if (!labelFxActive && animH > 0) {
    tft.setSwapBytes(kSwapBytesForPushImage);
    tft.startWrite();
    tft.pushImage(x, y + animY0, kPanelW, animH, gFrame565 + (size_t)animY0 * (size_t)kPanelW);
    tft.endWrite();
  }

  // Refresh overlay only when needed (avoids a visible periodic "blip").
  if (gOverlayDirty || gOverlayRevealActive || labelFxActive) {
    gOverlayDirty = false;
    drawOverlay();
  }

  // During the full-screen label FX, don't draw the bottom status strip,
  // otherwise it won't truly fill the screen.
  if (!labelFxActive) {
    // Always draw the Wi-Fi status dot last so it stays visible on top.
    drawStatusDots();
  }
}

static void setBacklight(bool on) {
#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  const int onLevel = TFT_BL_ON;
  digitalWrite(TFT_BL, on ? onLevel : !onLevel);
#else
  (void)on;
#endif
}

static void setBacklightRaw(bool levelHigh) {
#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, levelHigh ? HIGH : LOW);
#else
  (void)levelHigh;
#endif
}

static void blinkBacklight(uint8_t times) {
#ifdef TFT_BL
  for (uint8_t i = 0; i < times; i++) {
    setBacklight(false);
    delay(120);
    setBacklight(true);
    delay(120);
  }
#else
  (void)times;
#endif
}

static void printDisplayConfig() {
  Serial.println("Display config:");
  Serial.println("  (pin numbers are ESP32 GPIO/IO numbers, not physical header positions)");
#ifdef TFT_WIDTH
  Serial.printf("  TFT_WIDTH=%d TFT_HEIGHT=%d\n", TFT_WIDTH, TFT_HEIGHT);
#endif
#ifdef TFT_MOSI
  Serial.printf("  TFT_MOSI=%d TFT_SCLK=%d\n", TFT_MOSI, TFT_SCLK);
#endif
#ifdef TFT_CS
  Serial.printf("  TFT_CS=%d\n", TFT_CS);
#endif
#ifdef TFT_DC
  Serial.printf("  TFT_DC=%d\n", TFT_DC);
#endif
#ifdef TFT_RST
  Serial.printf("  TFT_RST=%d\n", TFT_RST);
#endif
#ifdef TFT_BL
  Serial.printf("  TFT_BL=%d TFT_BL_ON=%s\n", TFT_BL, (TFT_BL_ON == HIGH) ? "HIGH" : "LOW");
#else
  Serial.println("  TFT_BL not defined");
#endif
}

static void drawSweepWindow(int xOff) {
  // Deterministic alignment test (simple + slow):
  // Draw a 76px-wide white window across the controller canvas.
  // When this window aligns with the *physical* 76px visible slice, the entire
  // display should become solid white (with a black border + 2 colored lines).
  const int h = (int)tft.height();
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(xOff, 0, kPanelW, h, TFT_WHITE);

  // Thick-ish border inside the window.
  tft.drawRect(xOff, 0, kPanelW, h, TFT_BLACK);
  if (kPanelW > 4 && h > 4) {
    tft.drawRect(xOff + 1, 1, kPanelW - 2, h - 2, TFT_BLACK);
  }

  // Two unmistakable lines.
  const int y1 = min(12, h - 1);
  const int y2 = max(0, (h / 2));
  tft.drawFastHLine(xOff, y1, kPanelW, TFT_RED);
  tft.drawFastHLine(xOff, y2, kPanelW, TFT_BLUE);
}

static void drawBootScreen() {
  drawOverlay();
}


void setup() {
  // First priority: keep the panel dark and held in reset before any boot-time
  // logging or delays, otherwise controller junk can be briefly visible.
  setBacklight(false);
#ifdef TFT_RST
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
#endif

  Serial.begin(115200);
  // Give the host time to attach a monitor so we don't miss boot logs.
  const uint32_t serialWaitMs = 1500;
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < serialWaitMs) {
    delay(10);
  }
  delay(50);

  Serial.println();
  Serial.println("--- boot ---");

#if __has_include(<esp_sleep.h>)
  {
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    Serial.printf("WAKE: cause=%d\n", (int)cause);
  }
#endif

  // Demo gate inputs (active-low). Ground the pin to enable the respective demo.
  pinMode(kGkDemoGateGpio, INPUT_PULLUP);
  pinMode(kPulseOxDemoGateGpio, INPUT_PULLUP);
  pinMode(kUtilityDemoGateGpio, INPUT_PULLUP);
#if defined(BOARD_HAS_PSRAM)
  Serial.printf("psramFound()=%s, psram=%u bytes\n", psramFound() ? "true" : "false", (unsigned)ESP.getPsramSize());
#else
  Serial.println("PSRAM not enabled (BOARD_HAS_PSRAM not defined)");
#endif
  Serial.printf("flash=%u bytes\n", (unsigned)ESP.getFlashChipSize());

  setupKetosisLabelColorsOnce();
  updatePortalQrPayload();

  // Configure touch wake early so we can deep sleep from any state.
  configureTouchWakeIfNeeded();

  printDisplayConfig();

  // Keep the panel dark until the controller is initialized and we've drawn
  // a known-black first frame.
  delay(20);

#ifdef TFT_RST
  // Release the panel only once we're ready to start talking to it.
  digitalWrite(TFT_RST, HIGH);
  delay(20);
#endif

  // Ensure the SPI peripheral is initialized before TFT_eSPI starts using
  // transactions. This avoids a null SPI bus handle on some ESP32-S3 setups.
  // Note: TFT_eSPI will also call begin(), but we do it explicitly here.
  TFT_eSPI::getSPIinstance().begin(TFT_SCLK, TFT_MISO, TFT_MOSI, (TFT_CS >= 0) ? TFT_CS : -1);

  tft.init();
  // If blacks look white ("negative" image), this is the fix.
  tft.invertDisplay(kInvertDisplay);
  applyPanelViewport();

  initOverlaySpriteOnce();

  // Post-init sanity fill: keep it black (avoid white background flashes).
  Serial.println("post-init: fill black");
  tft.fillScreen(TFT_BLACK);
  tft.fillScreen(TFT_BLACK);
  delay(80);

  if (kDisplayBringupMode) {
    Serial.println("Display init done (bring-up mode): slow xOff sweep");
  } else {
    drawBootScreen();
    Serial.println("Display init done. If screen is blank, check pins/driver in include/User_Setup.h");
  }

  // Prove pushImage() placement behavior (viewport-relative vs absolute) before RLottie.
  // This should briefly show colored stripes in the visible window.
  if (kDebugTestPattern) {
    pushTestPatternOnce();
    if (!kDisplayBringupMode) {
      drawBootScreen();
    }
  }

  // Only light the panel after we've initialized the controller and painted a
  // deterministic first frame; this avoids showing controller junk at boot.
  setBacklight(true);
  delay(20);

  initLottie();

  // Start Wi-Fi connection management in the background. This allows the
  // display/render loop to keep running while WiFiManager is active.
  if (!gWifiTask) {
    const uint32_t stackDepthWords = 8192;  // 32KB
    const BaseType_t ok = xTaskCreatePinnedToCore(
        wifiTask, "wifiTask", stackDepthWords, nullptr, 1, &gWifiTask, 0);
    Serial.printf("wifiTask create: %s\n", (ok == pdPASS) ? "OK" : "FAIL");
  }

  // Start GK+ BLE download in the background (normal mode when demo gate is OFF).
  if (!gGkTask) {
    const uint32_t stackDepthWords = 12288;  // 48KB
    const BaseType_t ok = xTaskCreatePinnedToCore(
        gkplusTask, "gkplusTask", stackDepthWords, nullptr, 1, &gGkTask, 0);
    Serial.printf("gkplusTask create: %s\n", (ok == pdPASS) ? "OK" : "FAIL");
  }

  // Run rendering in a separate task with a larger stack to avoid loopTask
  // stack overflow.
  if (!gRenderTask) {
    // stackDepth is in 32-bit words on FreeRTOS.
    const uint32_t stackDepthWords = 24576;  // 96KB (RLottie render can be stack-hungry)
    const BaseType_t ok = xTaskCreatePinnedToCore(
        renderTask, "renderTask", stackDepthWords, nullptr, 1, &gRenderTask, 1);
    Serial.printf("renderTask create: %s\n", (ok == pdPASS) ? "OK" : "FAIL");
  }
}

void loop() {
  if (kDisplayBringupMode) {
    const uint32_t now = millis();
    // Keep backlight asserted during bring-up (and optionally calibrate polarity).
    static uint32_t lastBlToggleMs = 0;
    static bool blHigh = true;

    if (kBacklightCalMode) {
      if (lastBlToggleMs == 0 || (now - lastBlToggleMs) > 4000) {
        lastBlToggleMs = now;
        blHigh = !blHigh;
        setBacklightRaw(blHigh);
        Serial.printf("BL raw=%s (ignoring TFT_BL_ON)\n", blHigh ? "HIGH" : "LOW");
      }
    } else {
      setBacklight(true);
    }

    // Keep rotation fixed to reduce confusion.
    tft.setRotation(0);
    const int controllerW = (int)tft.width();
    const int maxX = max(0, controllerW - kPanelW);

    // Sweep slowly, in steps (faster to find alignment).
    const int step = 4;
    const uint32_t dwellMs = 900;
    const int xOff = (int)((((now / dwellMs) * (uint32_t)step) % (uint32_t)(maxX + 1)));

    static int lastX = -1;
    if (xOff != lastX) {
      lastX = xOff;
      drawSweepWindow(xOff);
      Serial.printf("SWEEP: rot=0 xOff=%d/%d (look for full-white display)\n", xOff, maxX);
    }

    delay(20);
    return;
  }

  // Rendering runs in renderTask(). loop() implements the sleep + touch rules.
  static SleepPolicyState sLastPolicy = kSleepNone;
  static uint32_t sPolicySinceMs = 0;

  // Touch press debounce.
  static bool sTouchWasActive = false;
  static uint8_t sTouchActiveStreak = 0;
  static uint32_t sLastTouchPressMs = 0;

  const uint32_t now = millis();

  // If both device kinds are blocked, show GOODBYE and sleep after the GOODBYE
  // animation completes (confetti fall -> banner expand).
  // Never sleep during demo gates.
  if (!isAnyDemoGateActive()) {
    if (gBlockGkPlusConnections && gBlockPulseOxConnections) {
      requestUiStep(9);
      gGoodbyeSleepAfterAnim = true;
    } else {
      gGoodbyeSleepAfterAnim = false;
    }
  } else {
    gGoodbyeSleepAfterAnim = false;
  }

  if (gGoodbyeSleepAfterAnim && gGoodbyeAnimDone && computeSleepPolicyState() != kSleepDisabled) {
    enterDeepSleepNow("goodbye");
  }

  // Touch handling: rising-edge press with simple debounce.
  // Note: we use the same threshold as deep-sleep wake.
  const bool touchActive = readTouchActiveNow();
  if (touchActive) {
    if (sTouchActiveStreak < 10) sTouchActiveStreak++;
  } else {
    sTouchActiveStreak = 0;
  }

  const bool touchPress = (!sTouchWasActive && (sTouchActiveStreak >= 2) && (now - sLastTouchPressMs) > 500);
  sTouchWasActive = touchActive;
  if (touchPress) {
    sLastTouchPressMs = now;
    handleTouchPress();
  }

  const SleepPolicyState st = computeSleepPolicyState();
  const uint32_t timeoutMs = sleepTimeoutForPolicyState(st);

  if (st != sLastPolicy) {
    sLastPolicy = st;
    sPolicySinceMs = now;
  }

  if (timeoutMs > 0 && st != kSleepDisabled) {
    const uint32_t elapsed = (now >= sPolicySinceMs) ? (now - sPolicySinceMs) : 0;
    if (elapsed >= timeoutMs) {
      const char* why = (st == kSleepWaitingWifiSetup) ? "wifi setup timeout"
                        : (st == kSleepWaitingBle) ? "ble wait timeout"
                        : (st == kSleepShowingReading) ? "reading timeout"
                        : "timeout";
      enterDeepSleepNow(why);
    }
  }

  delay(50);
}
