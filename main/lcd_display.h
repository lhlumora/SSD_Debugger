/*
 * Display of the system
 *
 * The module drives the ST7735S panel of the board with LVGL:
 *
 *   - it initializes the panel and creates the LVGL display,
 *   - it runs the LVGL handler in its own task,
 *   - it shows the test screen of the panel (see lcd_ui.c).
 *
 * The objects of LVGL are not thread safe, the other tasks of the
 * application have to take the lock of the display before they touch them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialize the panel and start the LVGL task
 *
 * @return ESP_OK on success
 */
esp_err_t lcd_display_init(void);

/**
 * @brief  Stop the LVGL task and release the display
 */
void lcd_display_deinit(void);

/**
 * @brief  Whether the display is ready
 */
bool lcd_display_is_ready(void);

/**
 * @brief  Get the number of the frames drawn since the last call
 *
 * The counter is reset by the call, it is used to measure the number of
 * frames per second.
 */
uint32_t lcd_display_take_frame_count(void);

/**
 * @brief  Get the LVGL display, NULL when the display is not initialized
 */
lv_display_t *lcd_display_get(void);

/**
 * @brief  Take the lock of the display
 *
 * It must be called before the LVGL objects are used from a task other than
 * the LVGL task.
 */
void lcd_display_lock(void);

/**
 * @brief  Release the lock of the display
 */
void lcd_display_unlock(void);

/**
 * @brief  Get the size of the visible area of the screen
 *
 * @param[out] width  width of the screen, may be NULL
 * @param[out] height height of the screen, may be NULL
 */
void lcd_display_get_size(int *width, int *height);

/**
 * @brief  Change the orientation of the screen
 *
 * @param rotation 0, 90, 180 or 270
 *
 * @return ESP_OK on success
 */
esp_err_t lcd_display_set_rotation(int rotation);

/**
 * @brief  Get the orientation of the screen
 */
int lcd_display_get_rotation(void);

/**
 * @brief  Draw the whole screen again
 *
 * It is useful after a test pattern was drawn directly on the panel: the
 * test screen is drawn again over it.
 */
void lcd_display_refresh(void);

/**
 * @brief  Switch the backlight of the display on or off
 */
void lcd_display_set_backlight(bool on);

/**
 * @brief  Whether the backlight of the display is on
 */
bool lcd_display_backlight_is_on(void);

/**
 * @brief  Set the brightness of the backlight of the display
 *
 * @param percent the brightness, from 0 (the backlight is off) to 100 (the
 *                backlight is at its maximum brightness)
 *
 * @return ESP_OK on success
 */
esp_err_t lcd_display_set_brightness(int percent);

/**
 * @brief  Get the brightness of the backlight of the display
 *
 * @return the brightness, in percent
 */
int lcd_display_get_brightness(void);

#ifdef __cplusplus
}
#endif
