/*
 * LVGL configuration
 *
 * The settings that are not defined here keep the default value of
 * lv_conf_internal.h.
 *
 * The file is found by the LVGL component because the directory of the main
 * component is added to the include path of the LVGL library (see the
 * CMakeLists.txt of the main component) and CONFIG_LV_CONF_SKIP is disabled
 * in the sdkconfig.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* The ST7735S panel is a 16 bits per pixel panel, RGB565 is used */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565

/* LVGL takes its memory from the C library instead of a static pool, the
   memory can then be allocated in the PSRAM */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* FreeRTOS is used, lv_lock() and lv_unlock() protect the LVGL objects
   against the other tasks of the application */
#define LV_USE_OS LV_OS_FREERTOS

/* Fonts: the dashboard of an 80x160 screen needs a readable text */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* The widgets that are not used by the dashboard are removed, they take a
   lot of space in the flash */
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGE 1
#define LV_USE_KEYBOARD 0
#define LV_USE_LIST 0
#define LV_USE_SCALE 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABLE 0
#define LV_USE_TILEVIEW 0
#define LV_USE_VIDEO 0

/* The log of LVGL is printed on the console */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#endif /* LV_CONF_H */
