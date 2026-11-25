/*
 * Driver of the TFT panels of the board
 *
 * The module drives a ST7735R (1.8 inch, 128x160 pixels) or a ST7789 (1.54
 * inch, 240x240 pixels) panel over SPI.
 *
 * The initialization sequence of the controller, the values of the power and
 * of the gamma registers and the orientation of the screen are the ones of
 * the TFT_eSPI library, which drives the same modules with the same
 * settings.
 *
 * The panel is driven in the RGB565 color format, the two bytes of a pixel
 * are sent in the order expected by the controller (the most significant
 * byte first).
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

/* The controller of the panel */
typedef enum {
    TFT_PANEL_ST7735 = 0,  /* 1.8 inch, 128x160 pixels */
    TFT_PANEL_ST7789,      /* 1.54 inch, 240x240 pixels */
} tft_panel_controller_t;

/* Orientation of the screen */
typedef enum {
    TFT_PANEL_ROTATION_0 = 0,   /* portrait */
    TFT_PANEL_ROTATION_90,      /* landscape */
    TFT_PANEL_ROTATION_180,     /* portrait, upside down */
    TFT_PANEL_ROTATION_270,     /* landscape */
} tft_panel_rotation_t;

/* Settings of the panel */
typedef struct {
    tft_panel_controller_t controller; /* the controller of the panel */
    spi_host_device_t spi_host;  /* SPI peripheral used by the panel */
    int   sclk_gpio;             /* SPI clock */
    int   mosi_gpio;             /* SPI data (SDA on the module) */
    int   cs_gpio;               /* chip select */
    int   dc_gpio;               /* data/command */
    int   rst_gpio;              /* reset */
    int   bl_gpio;               /* backlight, -1 when it is not connected */
    bool  bl_active_low;         /* the backlight is lit when the pin is low */
    int   clock_speed_hz;        /* SPI clock, 0 for the default (2 MHz) */
    tft_panel_rotation_t rotation;  /* orientation of the screen */
    int   width;                 /* width of the visible area */
    int   height;                /* height of the visible area */
    int   col_start;             /* offset of the visible area (portrait) */
    int   row_start;             /* offset of the visible area (portrait) */
    bool  bgr;                   /* the colors are in the BGR order */
    bool  invert;                /* the colors of the panel are inverted */
} tft_panel_config_t;

/**
 * @brief  Initialize the panel (SPI, GPIOs and the controller)
 *
 * @param config settings of the panel
 *
 * @return ESP_OK on success
 */
esp_err_t tft_panel_init(const tft_panel_config_t *config);

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
esp_err_t tft_panel_draw_pixels(int x, int y, int width, int height, const void *data);

/**
 * @brief  Change the orientation of the screen
 *
 * @param rotation the new orientation
 *
 * @return ESP_OK on success
 */
esp_err_t tft_panel_set_rotation(tft_panel_rotation_t rotation);

/**
 * @brief  Get the orientation of the screen
 */
tft_panel_rotation_t tft_panel_get_rotation(void);

/**
 * @brief  Get the size of the visible area of the screen
 *
 * @param[out] width  width of the screen, may be NULL
 * @param[out] height height of the screen, may be NULL
 */
void tft_panel_get_size(int *width, int *height);

/**
 * @brief  Switch the backlight on or off
 *
 * Does nothing when the backlight is not connected to a GPIO.
 */
void tft_panel_set_backlight(bool on);

/**
 * @brief  Whether the backlight is on
 */
bool tft_panel_backlight_is_on(void);

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
esp_err_t tft_panel_set_madctl(uint8_t value);

/**
 * @brief  Get the value of the MADCTL register of the controller
 */
uint8_t tft_panel_get_madctl(void);

/**
 * @brief  Change the offset of the visible area in the memory of the controller
 *
 * The controller has more pixels of memory than the visible area of the
 * module, the offset depends on the module, it is 0/0 for the 128x160
 * RobotLCD modules and for the 240x240 modules.
 *
 * The offset does not depend on the orientation of the screen.
 *
 * @param col offset of the first column
 * @param row offset of the first row
 *
 * @return ESP_OK on success
 */
esp_err_t tft_panel_set_offset(int col, int row);

/**
 * @brief  Get the offset of the visible area in the memory of the controller
 */
void tft_panel_get_offset(int *col, int *row);

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
esp_err_t tft_panel_fill(uint16_t color);

/**
 * @brief  Change the speed of the SPI bus of the panel
 *
 * @param clock_speed_hz the new speed of the bus, in Hz
 *
 * @return ESP_OK on success
 */
esp_err_t tft_panel_set_clock(int clock_speed_hz);

/**
 * @brief  Get the speed of the SPI bus of the panel
 */
int tft_panel_get_clock(void);

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
esp_err_t tft_panel_fill_raw(uint16_t color);

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
esp_err_t tft_panel_draw_map_pattern(void);

/**
 * @brief  Draw a test pattern on the screen
 *
 * A border around the visible area and some colored blocks, it is used to
 * check the position of the visible area in the memory of the controller.
 *
 * @return ESP_OK on success
 */
esp_err_t tft_panel_draw_test_pattern(void);

#ifdef __cplusplus
}
#endif
