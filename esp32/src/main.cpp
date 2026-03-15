#include <Arduino.h>
#include <TFT_eSPI.h>

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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <rlottie.h>

#include "generated/flame_lottie.h"

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

// Demo gate: only run the full demo loop when this GPIO is grounded.
// Uses the internal pull-up, so leaving it floating will read HIGH (demo off).
// Picked to avoid common strapping pins and to avoid TFT pins.
static constexpr int kDemoGateGpio = 5;

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
static bool isDemoGateActive();
static void setBleConnected(bool v);

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

static bool isDemoGateActive() {
  // LOW means the pin is grounded.
  return digitalRead(kDemoGateGpio) == LOW;
}

static volatile bool gBleConnected = false;

// Latest GK+ reading time (from the meter): "HH:MM".
static char gGkWhenLocal[8] = {0};

// Forces a repaint of the reserved bottom status strip (time + dots). This is
// needed because some stage transitions clear parts of the screen while the
// placeholder values may remain constant.
static volatile bool gStatusStripDirty = true;

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

  const bool demoPlaceholders = (kDemoMode && isDemoGateActive());

  // In demo mode, show placeholder status values so the bottom strip is always
  // populated (time + connection indicators) while cycling stages.
  const bool wifiUp = demoPlaceholders ? true : (WiFi.status() == WL_CONNECTED);
  const uint16_t wifiC = wifiUp ? kBlue : kRed;
  const bool bleUp = demoPlaceholders ? true : gBleConnected;
  const uint16_t bleC = bleUp ? kGreen : TFT_BLACK;

  // Bottom status row: show time centered between dots.
  const char* timeStr = demoPlaceholders ? "10:26" : gGkWhenLocal;
  const bool haveTime = (timeStr && timeStr[0] != '\0');

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

static void setBleConnected(bool v) {
  if (gBleConnected == v) return;
  gBleConnected = v;
  gStatusStripDirty = true;
}

static TaskHandle_t gWifiTask = nullptr;
static TaskHandle_t gGkTask = nullptr;

static bool gShowLoading = false;
static bool gShowNoWifi = false;
static String gTopStatus = "";

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

static void drawQrCodeFromText(const char* text, int x0, int y0, int scale, int quiet) {
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
    return;
  }

  const int size = qr.size;
  const int px = (size + 2 * quiet) * scale;
  tft.fillRect(x0, y0, px, px, TFT_BLACK);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        tft.fillRect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale, scale, TFT_WHITE);
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
  Serial.println("WIFI: begin() using stored credentials");
  WiFi.begin();
  if (waitForWifiConnected(8000)) {
    Serial.printf("WIFI: connected ip=%s\n", WiFi.localIP().toString().c_str());
    rtcWifiCacheUpdateFromCurrent();
    vTaskDelete(nullptr);
    return;
  }

  // Fallback: WiFiManager portal.
  Serial.println("WIFI: launching WiFiManager portal");
  WiFiManager wm;
  wm.setAPCallback(onWmApStarted);
  // Keep portal name stable so your phone remembers it.
  const bool ok = wm.autoConnect("ble-health-hub");
  if (ok) {
    Serial.printf("WIFI: WiFiManager connected ip=%s\n", WiFi.localIP().toString().c_str());
    rtcWifiCacheUpdateFromCurrent();
    gForceNoWifiScreen = false;
    gOverlayDirty = true;
  } else {
    Serial.println("WIFI: WiFiManager failed or timed out");
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
  if (!(ketoneMmolL > 0.0f)) return NAN;
  const float gki = (glucoseMgDl / 18.0f) / ketoneMmolL;
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

static void gkplusTask(void* /*param*/) {
  Serial.println("GK+: task start");

  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Always start in LOADING.
  requestUiStep(4);

  // GK+ GATT UUIDs (pcap-derived, canonical).
  const NimBLEUUID kSvc("0003cdd0-0000-1000-8000-00805f9b0131");
  const NimBLEUUID kNotify("0003cdd1-0000-1000-8000-00805f9b0131");
  const NimBLEUUID kWrite("0003cdd2-0000-1000-8000-00805f9b0131");

  // Observed constant requests.
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

    // Split concatenated frames (0x7b..0x7d).
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

      // Records-count response: 7b 01 20 01 10 db aa 00 02 <count2> <crc4> 7d
      if (cmd == 0xDB && flen >= 16 && f[6] == 0xAA) {
        sess->gotDbCount = true;
        continue;
      }

      // Record transfer frame: 7b 01 20 01 10 (16|dd|de) aa 00 09 <record9> <crc4> 7d
      if ((cmd == 0x16 || cmd == 0xDD || cmd == 0xDE) && flen >= 23 && f[6] == 0xAA && f[7] == 0x00 && f[8] == 0x09) {
        GkRecord r;
        if (!decodeGkRecordSnippet9(f + 9, 9, &r)) continue;
        if (r.kind == GkRecord::UNKNOWN) continue;
        if (!isfinite(r.value)) continue;
        sess->raw.push_back(r);
        if (sess->raw.size() > 16) {
          sess->raw.erase(sess->raw.begin(), sess->raw.begin() + (sess->raw.size() - 16));
        }

        // Best-effort pairing: choose most recent glucose+ketone within 15 minutes.
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
          sess->gotReading = true;
        }
      }
    }
  };

  // Connect loop: connect directly to a known GK+ MAC address.
  // NOTE: This assumes the meter uses a stable public address (not a rotating
  // resolvable private address).
  static const char kGkPlusMac[] = "e4:33:bb:84:83:66";
  const NimBLEAddress gkAddrPublic(std::string(kGkPlusMac), 0 /* public */);
  const NimBLEAddress gkAddrRandom(std::string(kGkPlusMac), 1 /* random */);

  // Keep trying until we decode a reading.
  for (;;) {
    if (isDemoGateActive()) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    requestUiStep(4);

    // Create a fresh client each attempt.
    Serial.printf("GK+: connect %s\n", kGkPlusMac);

    NimBLEClient* client = NimBLEDevice::createClient();
    if (!client) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // Default is 30s; shorten so failures are less painful.
    client->setConnectTimeout(8000);

    // Some GK+ units use a stable address but advertise it as "random".
    // Try random first, then public, with minimal logging.
    int errRandom = 0;
    int errPublic = 0;
    bool okConn = client->connect(gkAddrRandom);
    if (!okConn) {
      errRandom = client->getLastError();
      okConn = client->connect(gkAddrPublic);
      if (!okConn) {
        errPublic = client->getLastError();
      }
    }

    if (!okConn) {
      Serial.printf("GK+: connect failed (random=%d public=%d)\n", errRandom, errPublic);
      NimBLEDevice::deleteClient(client);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    setBleConnected(true);
    Serial.println("GK+: connected");

    NimBLERemoteService* svc = client->getService(kSvc);
    if (!svc) {
      setBleConnected(false);
      client->disconnect();
      NimBLEDevice::deleteClient(client);
      vTaskDelay(pdMS_TO_TICKS(1200));
      continue;
    }

    NimBLERemoteCharacteristic* chNotify = svc->getCharacteristic(kNotify);
    NimBLERemoteCharacteristic* chWrite = svc->getCharacteristic(kWrite);
    if (!chNotify || !chWrite) {
      setBleConnected(false);
      client->disconnect();
      NimBLEDevice::deleteClient(client);
      vTaskDelay(pdMS_TO_TICKS(1200));
      continue;
    }

    Session sess;
    sess.raw.reserve(8);
    sSession = &sess;

    if (!chNotify->subscribe(true, onNotify)) {
      sSession = nullptr;
      setBleConnected(false);
      Serial.println("GK+: subscribe failed");
      client->disconnect();
      NimBLEDevice::deleteClient(client);
      vTaskDelay(pdMS_TO_TICKS(1200));
      continue;
    }

    auto writeFrame = [&](const uint8_t* buf, size_t n, bool withResponse, const char* label) {
      if (!buf || n == 0) return;
      const bool ok = chWrite->writeValue(buf, n, withResponse);
      Serial.printf("GK+: TX %s (%u bytes) => %s\n", label ? label : "", (unsigned)n, ok ? "OK" : "FAIL");
    };

    // Best-effort init sequence (from pcaps/web app). We intentionally do NOT
    // send set-time here because the ESP32 currently has no trusted clock.
    writeFrame(kReqInfo66, sizeof(kReqInfo66), false, "0x66");
    vTaskDelay(pdMS_TO_TICKS(120));
    writeFrame(kReqInitAa, sizeof(kReqInitAa), false, "0xAA");
    vTaskDelay(pdMS_TO_TICKS(120));
    writeFrame(kReqInfo77, sizeof(kReqInfo77), false, "0x77");
    vTaskDelay(pdMS_TO_TICKS(120));
    writeFrame(kReqInfo66, sizeof(kReqInfo66), false, "0x66 (again)");
    vTaskDelay(pdMS_TO_TICKS(120));

    // Request records count; the notify handler will cause us to request records.
    writeFrame(kReqCountDb, sizeof(kReqCountDb), false, "0xDB (records count)");

    Serial.println("GK+: waiting for records...");

    const uint32_t t0 = millis();
    std::vector<uint8_t> cmd16;
    bool sentXfer = false;
    for (;;) {
      if (isDemoGateActive()) break;
      if (!client->isConnected()) break;
      if (sess.gotReading) break;

      const uint32_t elapsed = millis() - t0;
      if (elapsed > 8000) {
        Serial.println("GK+: timeout waiting for reading; retry");
        break;
      }

      if (sess.gotDbCount && !sentXfer) {
        sentXfer = true;
        // For "latest complete reading" we request 2 records (glucose + ketone).
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

    if (okReading) {
      Serial.println("GK+: reading acquired; task done");
      vTaskDelete(nullptr);
      return;
    }

    vTaskDelay(pdMS_TO_TICKS(800));
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
  if (gShowNoWifi) {
    // Full-screen static screen.
    setupKetosisLabelColorsOnce();
    applyPanelViewport();
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    tft.fillRect(0, 0, kPanelW, kPanelH, TFT_BLACK);

    // Title banner (like the HIGH KETOSIS bar, but at the top)
    const int bannerH = 16;
    tft.fillRect(0, 0, kPanelW, bannerH, gKetosisRed565);
    tft.setTextColor(TFT_WHITE, gKetosisRed565);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.drawString("NO WIFI", kPanelW / 2, bannerH / 2);

    // Broken Wi-Fi icon (simple arcs + slash), drawn bold for legibility.
    const int cx = kPanelW / 2;
    const int cy = 72;
    const int thick = 2;
    auto drawThickPixel = [&](int x, int y, uint16_t c) {
      tft.fillRect(x - (thick - 1), y - (thick - 1), thick * 2 - 1, thick * 2 - 1, c);
    };

    tft.fillCircle(cx, cy, 4, TFT_WHITE);
    auto drawArcPoints = [&](int r, float a0, float a1) {
      const int steps = max(12, (int)(r * 3));
      for (int i = 0; i <= steps; i++) {
        const float t = (float)i / (float)steps;
        const float a = a0 + (a1 - a0) * t;
        const int x = cx + (int)lroundf(cosf(a) * (float)r);
        const int y = cy + (int)lroundf(sinf(a) * (float)r);
        drawThickPixel(x, y, TFT_WHITE);
      }
    };

    // Arcs above the dot
    drawArcPoints(14, -2.6f, -0.55f);
    drawArcPoints(22, -2.6f, -0.55f);
    drawArcPoints(30, -2.6f, -0.55f);

    // Slash to indicate broken (thicker)
    tft.drawLine(cx - 22, cy - 30, cx + 22, cy + 8, TFT_RED);
    tft.drawLine(cx - 21, cy - 30, cx + 23, cy + 8, TFT_RED);
    tft.drawLine(cx - 23, cy - 30, cx + 21, cy + 8, TFT_RED);
    tft.drawLine(cx - 22, cy - 29, cx + 22, cy + 9, TFT_RED);

    // WiFiManager setup QR (join the portal AP).
    const int quiet = 2;
    const int scale = 2;
    const int qrPx = (29 + 2 * quiet) * scale;
    const int qrX = (kPanelW - qrPx) / 2;
    const int qrY = 120;
    drawQrCodeFromText(gPortalQrPayload, qrX, qrY, scale, quiet);

    tft.setTextDatum(TC_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Scan to setup", kPanelW / 2, qrY + qrPx + 8);
    tft.setTextDatum(TL_DATUM);
    drawStatusDots();
    return;
  }

  // Keep it simple and legible: opaque black background.
  setupKetosisLabelColorsOnce();
  applyPanelViewport();
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  if (!gShowLoading) {
    tft.fillRect(0, 0, kPanelW, kOverlayH, TFT_BLACK);

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

  // Label + big number, three stacked blocks.
  auto drawMetric = [&](int y, const char* label, const String& value) {
    y += kOverlayTextPadTopPx;
    tft.setTextSize(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(label, 2, y);

    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(value, 2, y + 14);
  };

  if (!gShowLoading) {
    drawMetric(2, "GLU", String(gGluMgDl, 0));
    drawMetric(58, "KET", String(gKetMmolL, 1));
    drawMetric(114, "GKI", String(gki, 1));
  }

  if (gTopStatus.length() > 0) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(gTopStatus, kPanelW / 2, 2 + kOverlayTextPadTopPx);
    tft.setTextDatum(TL_DATUM);
  }

  // Ketosis label bar: full width background rectangle.
  if (gKetosisLabel.length() > 0) {
    const int barH = 16;
    const int barY = kOverlayH - barH;
    tft.fillRect(0, barY, kPanelW, barH, gKetosisLabelBg);
    tft.setTextSize(1);
    tft.setTextColor(gKetosisLabelFg, gKetosisLabelBg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(gKetosisLabel, kPanelW / 2, barY + (barH / 2));
    tft.setTextDatum(TL_DATUM);
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
      gTopStatus = "LOADING";
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
    default:
      break;
  }

  gOverlayDirty = true;
  gStatusStripDirty = true;
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
    default:
      break;
  }

  applyUiStepVisual(step);
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

    const bool demoGate = isDemoGateActive();
    static bool sLastDemoGate = false;
    if (demoGate != sLastDemoGate) {
      sLastDemoGate = demoGate;
      Serial.printf("DEMO: gate=%s (GPIO%d=%s)\n", demoGate ? "ON" : "OFF", kDemoGateGpio,
                    demoGate ? "LOW" : "HIGH");
    }

    if (gLottieReady) {
      if (kDemoMode && demoGate) {
        if (gDemoStartMs == 0) {
          gDemoStartMs = now;
        }
        static constexpr int kDemoSteps = 6;
        static constexpr int kDemoOrder[kDemoSteps] = {4, 5, 0, 1, 2, 3};
        const int idx = (int)(((now - gDemoStartMs) / kDemoStepMs) % kDemoSteps);
        const int nextStep = (gForceNoWifiScreen ? 5 : kDemoOrder[idx]);
        if (nextStep != appliedStep) {
          appliedStep = nextStep;
          Serial.printf("DEMO: step=%d\n", appliedStep);
          applyDemoStep(appliedStep);
        }
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

    if (gLottieReady) {
      if (gShowNoWifi) {
        // Static screen: avoid continuously overwriting it with animation pushes.
        if (gOverlayDirty) {
          gOverlayDirty = false;
          drawOverlay();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
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
  if (animH > 0) {
    tft.setSwapBytes(kSwapBytesForPushImage);
    tft.startWrite();
    tft.pushImage(x, y + animY0, kPanelW, animH, gFrame565 + (size_t)animY0 * (size_t)kPanelW);
    tft.endWrite();
  }

  // Refresh overlay only when needed (avoids a visible periodic "blip").
  if (gOverlayDirty) {
    gOverlayDirty = false;
    drawOverlay();
  }

  // Always draw the Wi-Fi status dot last so it stays visible on top.
  drawStatusDots();
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

  // Demo gate input (active-low). Ground GPIO5 to enable the full demo loop.
  pinMode(kDemoGateGpio, INPUT_PULLUP);
#if defined(BOARD_HAS_PSRAM)
  Serial.printf("psramFound()=%s, psram=%u bytes\n", psramFound() ? "true" : "false", (unsigned)ESP.getPsramSize());
#else
  Serial.println("PSRAM not enabled (BOARD_HAS_PSRAM not defined)");
#endif
  Serial.printf("flash=%u bytes\n", (unsigned)ESP.getFlashChipSize());

  setupKetosisLabelColorsOnce();
  updatePortalQrPayload();

  printDisplayConfig();

  // Backlight: leave it ON (no boot flashing).
  setBacklight(true);
  delay(50);

  // Ensure the SPI peripheral is initialized before TFT_eSPI starts using
  // transactions. This avoids a null SPI bus handle on some ESP32-S3 setups.
  // Note: TFT_eSPI will also call begin(), but we do it explicitly here.
  TFT_eSPI::getSPIinstance().begin(TFT_SCLK, TFT_MISO, TFT_MOSI, (TFT_CS >= 0) ? TFT_CS : -1);

  tft.init();
  // If blacks look white ("negative" image), this is the fix.
  tft.invertDisplay(kInvertDisplay);
  applyPanelViewport();

  // Post-init sanity fill: keep it black (avoid white background flashes).
  Serial.println("post-init: fill black");
  tft.fillScreen(TFT_BLACK);
  delay(80);

  // Keep backlight steady (no boot flashing).
  setBacklight(true);

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

  // Rendering runs in renderTask(). Keep loop() minimal.
  delay(1000);
}
