#pragma once

// Minimal LVGL configuration for this project.
// Kept intentionally small; extend as we add widgets.

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include <stddef.h>

/* Color */
#define LV_COLOR_DEPTH 16

/* Memory */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC malloc
#define LV_MEM_CUSTOM_FREE free
#define LV_MEM_CUSTOM_REALLOC realloc

/* HAL */
#define LV_TICK_CUSTOM 0

/* Logging */
#define LV_USE_LOG 0

/* Fonts (keep flash usage low) */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_UNSCII_8 1
#define LV_FONT_UNSCII_16 1
#define LV_FONT_DEFAULT &lv_font_unscii_16

/* Features used by the demo */
#define LV_USE_LABEL 1
#define LV_USE_FLEX 1
#define LV_USE_ANIMATION 1

#endif  // LV_CONF_H
