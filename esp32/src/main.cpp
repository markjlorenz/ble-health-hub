#include <Arduino.h>
#include <TFT_eSPI.h>

#include <WiFi.h>
#include <WiFiSTA.h>

// WiFiManager (tzapu/WiFiManager)
#include <WiFiManager.h>

// QRCode (ricmoo/QRCode)
#include <qrcode.h>

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
static void drawWifiStatusDot();
static bool isDemoGateActive();

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

static void drawWifiStatusDot() {
  // Always draw (blue when connected, red when not) so we don't leave stale pixels.
  int ax = 0;
  int ay = 0;
  getPanelAbsXY(&ax, &ay);

  static constexpr int kDot = 4;  // 4px tall (and wide)
  const int x = ax + (kPanelW - kDot);
  const int y = ay + (kPanelH - kDot);
  // Use our calibrated RGB888->565 conversion (handles red/blue swap)
  // instead of TFT_* constants.
  static const uint16_t kDotBlue = rgb888To565(0x00, 0x00, 0xFF);
  static const uint16_t kDotRed = rgb888To565(0xFF, 0x00, 0x00);
  const uint16_t c = (WiFi.status() == WL_CONNECTED) ? kDotBlue : kDotRed;

  tft.startWrite();
  tft.fillRect(x, y, kDot, kDot, c);
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

static TaskHandle_t gWifiTask = nullptr;

static bool gShowLoading = false;
static bool gShowNoWifi = false;
static String gTopStatus = "";

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
    drawWifiStatusDot();
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

  drawWifiStatusDot();
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

static void applyDemoStep(int step) {
  gShowLoading = false;
  gShowNoWifi = false;
  gTopStatus = "";

  // Step meanings per user spec.
  switch (step) {
    case 0: {
      gGluMgDl = 81.0f;
      gKetMmolL = 0.3f;
      gGkiOverride = true;
      gGkiValue = 15.0f;
      gKetosisLabel = "NOT KETOSIS";
      gKetosisLabelBg = gKetosisBrown565;
      gKetosisLabelFg = TFT_WHITE;

      // Only Logs visible.
      setAllLayersOpacityPct(0.0f);
      setLayerOpacityPct("Logs", 100.0f);

      setLoopToFlameBurn();
      break;
    }
    case 1: {
      gGluMgDl = 109.0f;
      gKetMmolL = 1.0f;
      gGkiOverride = true;
      gGkiValue = 6.0f;
      gKetosisLabel = "LOW KETOSIS";
      gKetosisLabelBg = gKetosisOrange565;
      gKetosisLabelFg = TFT_WHITE;

      // Logs + Orange Container + Yellow Container.
      setAllLayersOpacityPct(0.0f);
      setLayerOpacityPct("Logs", 100.0f);
      setLayerOpacityPct("Orange Container", 100.0f);
      setLayerOpacityPct("Yellow Container", 100.0f);

      setLoopToFlameBurn();
      break;
    }
    case 2: {
      gGluMgDl = 110.0f;
      gKetMmolL = 1.2f;
      gGkiOverride = true;
      gGkiValue = 5.1f;
      gKetosisLabel = "MID KETOSIS";
      gKetosisLabelBg = gKetosisYellow565;
      gKetosisLabelFg = TFT_BLACK;

      // All visible except BigRed Container and the red whisp element.
      setAllLayersOpacityPct(100.0f);
      setLayerOpacityPct("BigRed Container", 0.0f);
      setLayerOpacityPct("SideRed Container", 0.0f);

      setLoopToFlameBurn();
      break;
    }
    case 3: {
      gGluMgDl = 82.0f;
      gKetMmolL = 3.6f;
      gGkiOverride = true;
      gGkiValue = 1.3f;
      gKetosisLabel = "HIGH KETOSIS";
      gKetosisLabelBg = gKetosisRed565;
      gKetosisLabelFg = TFT_WHITE;

      // All layers visible.
      setAllLayersOpacityPct(100.0f);

      setLoopToFlameBurn();
      break;
    }
    case 4: {
      // New stage: Log Loading
      gShowLoading = true;
      gTopStatus = "LOADING";
      gKetosisLabel = "";
      gKetosisLabelBg = TFT_BLACK;
      gKetosisLabelFg = TFT_WHITE;

      // Keep this stage simple: only Logs visible.
      setAllLayersOpacityPct(0.0f);
      setLayerOpacityPct("Logs", 100.0f);

      setLoopToLogLoading();
      break;
    }
    case 5: {
      // New stage: No Wi-Fi
      gShowNoWifi = true;
      gTopStatus = "NO WIFI";
      gKetosisLabel = "";
      gKetosisLabelBg = TFT_BLACK;
      gKetosisLabelFg = TFT_WHITE;

      // Hide all animation layers while this static screen is shown.
      setAllLayersOpacityPct(0.0f);
      break;
    }
    default:
      break;
  }

  gOverlayDirty = true;
}

static void renderTask(void* /*param*/) {
  // RLottie's render pipeline can be stack-hungry. Running it inside Arduino's
  // default loopTask has been observed to overflow the stack on ESP32-S3.
  // This task is created with a larger stack.
  int demoStep = -1;
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

    if (kDemoMode && gLottieReady) {
      if (gDemoStartMs == 0) {
        gDemoStartMs = now;
      }
      static constexpr int kDemoSteps = 6;
      static constexpr int kDemoOrder[kDemoSteps] = {4, 5, 0, 1, 2, 3};
      const int idx = (int)(((now - gDemoStartMs) / kDemoStepMs) % kDemoSteps);
      int nextStep = 4;
      if (demoGate) {
        nextStep = (gForceNoWifiScreen ? 5 : kDemoOrder[idx]);
      }
      if (nextStep != demoStep) {
        demoStep = nextStep;
        Serial.printf("DEMO: step=%d\n", demoStep);
        applyDemoStep(demoStep);
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
  const int animH = max(0, kPanelH - animY0);
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
  drawWifiStatusDot();
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
