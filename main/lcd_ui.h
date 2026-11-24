/*
 * Test screen of the display
 *
 * The screen checks the driving of the panel:
 *
 *   - a row of eight colors shows if the order of the colors is the expected
 *     one,
 *   - a row of shades of gray shows the depth of the colors,
 *   - a ball moves in an area, it shows that the animations work,
 *   - an arc and a bar are animated,
 *   - a border and four marks in the corners show if the image is shifted or
 *     turned,
 *   - the size of the screen, the order of the colors, the speed of the SPI
 *     bus, the offset of the visible area and the number of frames per second
 *     are shown at the bottom of the screen.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Create the test screen
 *
 * The screen is created and a timer refreshes the information it shows.
 *
 * @return ESP_OK on success
 */
esp_err_t lcd_ui_create(void);

/**
 * @brief  Delete the test screen and create it again
 *
 * It is used when the orientation of the screen changes.
 */
void lcd_ui_recreate(void);

#ifdef __cplusplus
}
#endif
