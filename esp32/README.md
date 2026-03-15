# ESP32 app (PlatformIO)

Goal: a standalone ESP32 app that shows GK+ + pulse-ox values on a **76x284 TFT** (via **TFT_eSPI**) and later talks BLE to the devices.

## Status

- Current milestone: **display bring-up** (draw a sample dashboard + blinking status dot)
- BLE is not wired up yet.

## Prereqs

- VS Code + PlatformIO extension
- ESP32 toolchain installed via PlatformIO

## Build / Upload

From the repo root:

```sh
cd esp32
pio run -e esp32-s3-n16r8
pio run -e esp32-s3-n16r8 -t upload
pio device monitor
```

If `pio` is not on your PATH, use the default PlatformIO venv install path:

```sh
~/.platformio/penv/bin/pio run -e esp32-s3-n16r8
```

## Display configuration

Edit:

- `esp32/include/User_Setup.h`

You must set:

- correct TFT driver (`ST7789_DRIVER` vs other)
- correct SPI pins (`TFT_MOSI`, `TFT_SCLK`, `TFT_CS`, `TFT_DC`, `TFT_RST`, `TFT_BL`)
- correct `TFT_WIDTH` / `TFT_HEIGHT`

On boot, the firmware shows a quick RGB flash + color bars (test pattern), then a boot screen and sample dashboard. Use that to debug whether you have “backlight only” vs “SPI/driver is wrong”.

Driver note: if your module reports **ST7789P3**, it is typically compatible with TFT_eSPI's `ST7789_DRIVER`.

If the image is shifted/cropped, the panel likely needs ST7789 offsets (TFT_eSPI supports offsets in some setups) — we’ll add those once we know what the panel is doing.

### Recommended wiring (LB ESP32-S3 N16R8 board)

These match the defaults in `esp32/include/User_Setup.h`:

- Display `SDA` → `GPIO11` (MOSI)
- Display `SCL` → `GPIO12` (SCLK)
- Display `CS` → `GPIO10` (or set `TFT_CS=-1` if your breakout truly omits CS)
- Display `DC` → `GPIO9`
- Display `RST` → `GPIO14`
- Display `BL` → `GPIO15` (optional; some breakouts tie backlight high)
- Display `VCC` → `3V3`
- Display `GND` → `GND`

## Known-good TFT config (76x284 narrow panel)

As of 2026-03-14, the display is working reliably with a **logical 240x320 ST7789 canvas** and an explicit **76x284 viewport**.

Why this matters:

- The physical visible area is **76x284**, but many ST7789-based modules are a *cropped window* into the controller’s **240x320 GRAM**.
- When we configured TFT_eSPI as **240x280**, the firmware could never write the last ~4 physical rows, which showed up as **weird stale rectangles at the bottom**.

### 1) TFT_eSPI setup (`include/User_Setup.h`)

Key points:

- Use `ST7789_DRIVER`
- Use `TFT_WIDTH=240`, `TFT_HEIGHT=320`
- Do **not** use `CGRAM_OFFSET` for this panel (we handle the visible window via `setViewport()`)
- ESP32-S3: provide a dummy `TFT_MISO` pin (even if the display is write-only) to avoid SPI init edge cases

Current working defaults are in `esp32/include/User_Setup.h`.

### 2) Firmware viewport (`src/main.cpp`)

We render into the visible slice by setting a viewport on each draw:

- `kPanelW = 76`
- `kPanelH = 284`
- `kPanelXOff = 82` (horizontal alignment within the 240-wide canvas)
- `kPanelYOff = 18` (centers 284 within 320: `(320-284)/2`)

If anything is shifted, tune these:

- If content is shifted left/right: adjust `kPanelXOff`
- If the top/bottom are clipped: adjust `kPanelYOff` by small steps (e.g. `16`, `18`, `20`)

Important:

- Avoid using negative `tft.setOrigin()` shifts for alignment on this setup; it caused text to disappear due to clipping interactions. The stable method is `setViewport(x, y, w, h, ...)`.

### 3) ESP32-S3 SPI bring-up note

To avoid rare crashes during display init on ESP32-S3, the firmware explicitly initializes the SPI peripheral before `tft.init()`:

```cpp
TFT_eSPI::getSPIinstance().begin(TFT_SCLK, TFT_MISO, TFT_MOSI, (TFT_CS >= 0) ? TFT_CS : -1);
```

If you remove that line and see boot-time instability, put it back.

## Next

## Troubleshooting (common)

- Blank screen but ESP32 is running:
	- Try flipping backlight polarity in `esp32/include/User_Setup.h`:
		- `#define TFT_BL_ON LOW`
	- If your breakout has no CS pin (or it is not connected), set `TFT_CS` to `-1`.
	- Drop SPI speed: set `SPI_FREQUENCY` to `27000000` or `20000000`.
- Image shows but is shifted/cropped:
	- This panel likely needs ST7789 RAM offsets; capture a photo of what you see and we’ll add the appropriate TFT_eSPI offset defines.


Once the display is confirmed working:

- Decide which ESP32 board variant we’re targeting (`board = ...` in `platformio.ini`)
- Add BLE plumbing (NimBLE/Arduino) and then implement PO3 + GK+ protocols
