// Project-local TFT_eSPI setup
//
// IMPORTANT:
// - You MUST edit this file to match your specific TFT driver + SPI pins.
// - This is intentionally minimal and meant to be the single place you touch
//   to get the screen working.
//
// Display: 76 x 284 TFT (driver reported as ST7789P3)
// Library: TFT_eSPI (Bodmer)

#pragma once

// ---- Driver selection ----
// ST7789P3 panels are generally compatible with TFT_eSPI's ST7789 driver.
// If your panel is not ST7789-class, change this.
#define ST7789_DRIVER

// ESP32-S3 note: Arduino-ESP32's SPI implementation strongly prefers FSPI.
// If the SPI bus fails to start, SPI transactions will crash.
#define USE_FSPI_PORT

// Some ST7789 panels have different offsets; adjust if your image is shifted.
// #define TFT_RGB_ORDER TFT_BGR

// ---- SPI mode ----
// Some ST7789 modules require SPI mode 0 (CPOL=0, CPHA=0). TFT_eSPI defaults
// to mode 3 for ST77xx unless overridden. Use numeric value here because this
// header is force-included before <SPI.h> (so SPI_MODE0 may not be defined yet).
#define TFT_SPI_MODE 3

// ---- Display size ----
// Bring-up strategy:
// Many ST7789 panels are wired as a cropped window into a 240x320 internal GRAM.
// Using the full 240x320 logical size makes fill/test patterns much more likely
// to land in the visible area even if we haven't nailed the offsets yet.
// Once we can see pixels, we can tighten this back down and add offsets.
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// For some ST7789 panel sizes (e.g. 240x280), TFT_eSPI needs to apply RAM
// offsets (colstart/rowstart) so writes land in the visible area.
// We keep the logical canvas at 240x320 and handle the visible window using
// setViewport() in firmware, so no built-in CGRAM offset is needed.
// #define CGRAM_OFFSET

// ---- SPI pins (recommended for LB ESP32-S3 N16R8 board) ----
// IMPORTANT: These numbers are ESP32 **GPIO** numbers (aka the "IOxx" labels
// on most dev boards), NOT physical header pin positions.
// Note on labels:
// - In I2C, SDA/SCL mean "data"/"clock".
// - Many small TFT breakouts are SPI but still label their pins SDA/SCL.
//   In that common (mis)labeling:
//     - TFT "SDA" == SPI MOSI (a.k.a. DIN/SDI, data from MCU -> display)
//     - TFT "SCL" == SPI SCLK (SPI clock)
// This display is wired for SPI (not I2C).
//
// This board conveniently exposes a contiguous GPIO9..GPIO15 block.
// These defaults avoid the USB pins (GPIO19/20) and the flash/PSRAM bus pins.
#define TFT_MOSI 11
#define TFT_SCLK 12

// The display is write-only; MISO is not wired. On ESP32-S3, TFT_eSPI may
// rewrite TFT_MISO to TFT_MOSI when set to -1, which can break SPI init.
// Provide a valid (but unwired) dummy input pin instead.
#define TFT_MISO 13

// Chip select:
// If CS is tied to GND (common on some breakouts) or not wired, set TFT_CS=-1.
// If CS is wired, use the correct GPIO instead.
#define TFT_CS   10

#define TFT_DC   9

// Reset:
// If RST is not wired (or you’re unsure), set TFT_RST=-1 to use software reset.
#define TFT_RST  14

// Backlight pin (optional). If your breakout has BL tied high, you can omit it.
#define TFT_BL   15

// Backlight polarity. Many breakouts are active-HIGH, but some are active-LOW.
// If the screen stays dark but you suspect the rest is working, try:
//   #define TFT_BL_ON LOW
#ifndef TFT_BL_ON
#define TFT_BL_ON LOW
#endif

// ---- SPI frequency ----
// Bring-up tip: start conservative; increase later once stable.
#define SPI_FREQUENCY  10000000

// If the image is shifted/cropped, this panel likely needs ST7789 offsets.
// We can add those once you confirm what you see on the screen.

// ---- Fonts ----
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT
