/*
 * ST7735S panel driver
 *
 * The driver talks to a ST7735R controller (1.8 inch, 128x160 pixels) over
 * SPI. The controller has 132x162 pixels of memory, the visible area of the
 * module is offset in it, the offset depends on the module.
 *
 * The panel is driven in the RGB565 color format, the two bytes of a pixel
 * are sent in the order expected by the controller (the most significant
 * byte first).
 *
 * The initialization sequence and the settings of the controller are the
 * ones of the TFT_eSPI library, which drives the same modules.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Orientation of the screen */
typedef enum {
    ST7735_ROTATION_0 = 0,   /* portrait */
    ST7735_ROTATION_90,      /* landscape */
    ST7735_ROTATION_180,     /* portrait, upside down */
    ST7735_ROTATION_270,     /* landscape */
} st7735_rotation_t;

/* Size of the memory of the controller, the visible area of the module is
   a part of it */
#define ST7735_MEMORY_WIDTH  132
#define ST7735_MEMORY_HEIGHT 162

/* Settings of the panel */
typedef struct {
    spi_host_device_t spi_host;  /* SPI peripheral used by the panel */
    int   sclk_gpio;             /* SPI clock */
    int   mosi_gpio;             /* SPI data (SDA on the module) */
    int   cs_gpio;               /* chip select */
    int   dc_gpio;               /* data/command */
    int   rst_gpio;              /* reset */
    int   bl_gpio;               /* backlight, -1 when it is not connected */
    bool  bl_active_low;         /* the backlight is lit when the pin is low */
    int   clock_speed_hz;        /* SPI clock, 0 for the default (2 MHz) */
    st7735_rotation_t rotation;  /* orientation of the screen */
    int   width;                 /* width of the visible area */
    int   height;                /* height of the visible area */
    int   col_start;             /* offset of the visible area (portrait) */
    int   row_start;             /* offset of the visible area (portrait) */
} st7735_config_t;

/**
 * @brief  Initialize the panel (SPI, GPIOs and the controller)
 *
 * @param config settings of the panel
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_init(const st7735_config_t *config);

/**
 * @brief  Draw a rectangle of pixels on the screen
 *
 * @param x     x coordinate of the first pixel
 * @param y     y coordinate of the first pixel
 * @param width width of the rectangle
 * @param height height of the rectangle
 * @param data  the pixels, in the RGB565 format, two bytes per pixel
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_draw_pixels(int x, int y, int width, int height, const void *data);

/**
 * @brief  Change the orientation of the screen
 *
 * @param rotation the new orientation
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_set_rotation(st7735_rotation_t rotation);

/**
 * @brief  Get the orientation of the screen
 */
st7735_rotation_t st7735_get_rotation(void);

/**
 * @brief  Get the size of the visible area of the screen
 *
 * @param[out] width  width of the screen, may be NULL
 * @param[out] height height of the screen, may be NULL
 */
void st7735_get_size(int *width, int *height);

/**
 * @brief  Switch the backlight on or off
 *
 * Does nothing when the backlight is not connected to a GPIO.
 */
void st7735_set_backlight(bool on);

/**
 * @brief  Whether the backlight is on
 */
bool st7735_backlight_is_on(void);

/**
 * @brief  Set the value of the MADCTL register of the controller
 *
 * The register drives the orientation and the mirroring of the screen, it
 * depends on the module.
 *
 * @param value the value of the register
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_set_madctl(uint8_t value);

/**
 * @brief  Get the value of the MADCTL register of the controller
 */
uint8_t st7735_get_madctl(void);

/**
 * @brief  Change the offset of the visible area in the memory of the controller
 *
 * The controller has 132x162 pixels of memory, the visible area of the module
 * is smaller and offset in it. The offset depends on the module, it is 0/0
 * for the 128x160 modules of the RobotLCD type, 2/1 for the 1.8 inch green
 * modules, 26/1 for the 0.96 inch green modules and 24/0 for the 0.96 inch
 * red ones.
 *
 * The offset does not depend on the orientation of the screen.
 *
 * @param col offset of the first column
 * @param row offset of the first row
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_set_offset(int col, int row);

/**
 * @brief  Get the offset of the visible area in the memory of the controller
 */
void st7735_get_offset(int *col, int *row);

/**
 * @brief  Fill the visible area of the screen with a color
 *
 * It does not use LVGL, it is used to check that the panel is driven
 * correctly.
 *
 * @param color the color, in the RGB565 format
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_fill(uint16_t color);

/**
 * @brief  Change the speed of the SPI bus of the panel
 *
 * @param clock_speed_hz the new speed of the bus, in Hz
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_set_clock(int clock_speed_hz);

/**
 * @brief  Get the speed of the SPI bus of the panel
 */
int st7735_get_clock(void);

/**
 * @brief  Fill the whole memory of the controller with a color
 *
 * The visible area of the module is a part of the memory of the controller,
 * this function fills all of it. It is used to check that the panel is
 * driven correctly, whatever the offset of the visible area is.
 *
 * @param color the color, in the RGB565 format
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_fill_raw(uint16_t color);

/**
 * @brief  Draw a pattern that shows how the screen is mapped
 *
 * A red block in the top left corner, a blue block in the bottom right
 * corner, a green line in the middle of the height and an orange line in
 * the middle of the width. A photo of the screen shows if the image is
 * mirrored, turned or shifted.
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_draw_map_pattern(void);

/**
 * @brief  Draw a test pattern on the screen
 *
 * A border around the visible area and some colored blocks, it is used to
 * check the position of the visible area in the memory of the controller.
 *
 * @return ESP_OK on success
 */
esp_err_t st7735_draw_test_pattern(void);

#ifdef __cplusplus
}
#endif
