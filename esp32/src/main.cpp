#include <Arduino.h>
#include <TFT_eSPI.h>

static TFT_eSPI tft;

// Bring-up mode: continuously cycle a high-contrast pattern so we can
// confidently confirm the panel is rendering (and which rotation looks right)
// before we switch back to the dashboard UI.
static constexpr bool kDisplayBringupMode = false;
static constexpr bool kBacklightCalMode = false;

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

// Many of these narrow ST7789-based panels expose a cropped window of the
// controller's RAM. If the very top is not physically visible, shift all UI
// drawing down by this many pixels.
static constexpr int kUiYShift = 0;

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
  applyPanelViewport();

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("BLE Hub", 2, 2 + kUiYShift);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("TFT ok", 2, 14 + kUiYShift);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("xOff=", 2, 26 + kUiYShift);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(String(kPanelXOff), 34, 26 + kUiYShift);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("yOff=", 2, 38 + kUiYShift);
  tft.drawString(String(kPanelYOff), 38, 38 + kUiYShift);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("yShift=", 2, 50 + kUiYShift);
  tft.drawString(String(kUiYShift), 50, 50 + kUiYShift);
}

static void drawUi(uint32_t secondsUp) {
  applyPanelViewport();

  // Narrow panel UI: clear and redraw everything each tick.
  // This avoids leftover artifacts from partial overlaps/cropping.
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("BLE Hub", 2, 2 + kUiYShift);

  // Heartbeat / uptime marker.
  const bool tick = (secondsUp % 2) == 0;
  tft.fillCircle(tft.width() - 6, 8 + kUiYShift, 4, tick ? TFT_GREEN : TFT_DARKGREY);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("GLU", 2, 26 + kUiYShift);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("100", 2, 38 + kUiYShift);
  tft.setTextSize(1);
  tft.drawString("mg/dL", 2, 58 + kUiYShift);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("KET", 2, 78 + kUiYShift);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("1.1", 2, 90 + kUiYShift);
  tft.setTextSize(1);
  tft.drawString("mmol/L", 2, 110 + kUiYShift);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("GKI", 2, 130 + kUiYShift);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("5.0", 2, 142 + kUiYShift);
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
#if defined(BOARD_HAS_PSRAM)
  Serial.printf("psramFound()=%s, psram=%u bytes\n", psramFound() ? "true" : "false", (unsigned)ESP.getPsramSize());
#else
  Serial.println("PSRAM not enabled (BOARD_HAS_PSRAM not defined)");
#endif
  Serial.printf("flash=%u bytes\n", (unsigned)ESP.getFlashChipSize());

  printDisplayConfig();

  // Backlight bring-up: start full-bright, blink for polarity sanity,
  // then leave it ON.
  setBacklight(true);
  delay(80);
  blinkBacklight(3);
  setBacklight(true);
  delay(50);

  // Ensure the SPI peripheral is initialized before TFT_eSPI starts using
  // transactions. This avoids a null SPI bus handle on some ESP32-S3 setups.
  // Note: TFT_eSPI will also call begin(), but we do it explicitly here.
  TFT_eSPI::getSPIinstance().begin(TFT_SCLK, TFT_MISO, TFT_MOSI, (TFT_CS >= 0) ? TFT_CS : -1);

  tft.init();
  applyPanelViewport();

  // Post-init sanity flashes: if these ever show up, we know pixel writes are landing.
  Serial.println("post-init: fill white then black");
  tft.fillScreen(TFT_WHITE);
  delay(250);
  tft.fillScreen(TFT_BLACK);
  delay(120);

  // Also do a distinct backlight pattern to confirm we got past tft.init().
  // (If you only ever see the initial 3 blinks, we didn't make it here OR BL polarity is wrong.)
  setBacklight(false);
  delay(80);
  setBacklight(true);
  delay(80);
  setBacklight(false);
  delay(80);
  setBacklight(true);

  if (kDisplayBringupMode) {
    Serial.println("Display init done (bring-up mode): slow xOff sweep");
  } else {
    drawBootScreen();
    Serial.println("Display init done. If screen is blank, check pins/driver in include/User_Setup.h");
  }
}

void loop() {
  const uint32_t now = millis();
  const uint32_t secondsUp = now / 1000;

  if (kDisplayBringupMode) {
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

  static uint32_t lastMs = 0;
  if (now - lastMs < 1000) {
    delay(10);
    return;
  }
  lastMs = now;

  drawUi(secondsUp);
  Serial.printf("uptime=%lus\n", (unsigned long)secondsUp);
}
