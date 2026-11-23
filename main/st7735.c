/*
 * ST7735S panel driver
 *
 * See st7735.h for the description of the module.
 */
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "st7735.h"

static const char *TAG = "st7735";

/* Commands of the controller */
#define ST7735_CMD_SWRESET  0x01
#define ST7735_CMD_SLPOUT   0x11
#define ST7735_CMD_INVON    0x21
#define ST7735_CMD_INVOFF   0x20
#define ST7735_CMD_DISPOFF  0x28
#define ST7735_CMD_DISPON   0x29
#define ST7735_CMD_CASET    0x2A
#define ST7735_CMD_RASET    0x2B
#define ST7735_CMD_RAMWR    0x2C
#define ST7735_CMD_MADCTL   0x36
#define ST7735_CMD_IDMOFF   0x38
#define ST7735_CMD_IDMON    0x39
#define ST7735_CMD_COLMOD   0x3A
#define ST7735_CMD_MCTR1    0xB0
#define ST7735_CMD_MCTR2    0xB1
#define ST7735_CMD_MCTR3    0xB2
#define ST7735_CMD_MCTR4    0xB3
#define ST7735_CMD_MCTR5    0xB4
#define ST7735_CMD_PCTR1    0xC0
#define ST7735_CMD_PCTR2    0xC1
#define ST7735_CMD_PCTR3    0xC2
#define ST7735_CMD_PCTR4    0xC3
#define ST7735_CMD_PCTR5    0xC4
#define ST7735_CMD_VMCTR1   0xC5
#define ST7735_CMD_VMCTR2   0xC6
#define ST7735_CMD_VMCTR3   0xC7
#define ST7735_CMD_PCTR6    0xC8
#define ST7735_CMD_PCLCTRL  0xC9
#define ST7735_CMD_GCTRL    0xD5
#define ST7735_CMD_VCOMS    0xDB
#define ST7735_CMD_LCCTRL   0xE0
#define ST7735_CMD_LCMCTR   0xE1
#define ST7735_CMD_VCVS     0xE2
#define ST7735_CMD_VRHS     0xE3
#define ST7735_CMD_TMRD     0xE9
#define ST7735_CMD_PWSET    0xF0
#define ST7735_CMD_PWSET2   0xF1
#define ST7735_CMD_PWSET3   0xF2
#define ST7735_CMD_EVSET    0xF3

/* Bits of the MADCTL command */
#define ST7735_MADCTL_MY  0x80
#define ST7735_MADCTL_MX  0x40
#define ST7735_MADCTL_MV  0x20
#define ST7735_MADCTL_ML  0x10
#define ST7735_MADCTL_BGR 0x08
#define ST7735_MADCTL_MH  0x04

/* Default SPI clock of the panel */
#define ST7735_DEFAULT_CLOCK_HZ (2 * 1000 * 1000)

/* The SPI controller cannot transfer more than 4092 bytes at once */
#define SPI_MAX_TRANSFER 4092

/* The colors of the screen are inverted. Most of the ST7735 modules need it,
   the setting is in the menuconfig (see CONFIG_EXAMPLE_LCD_INVERT_COLORS). */
#if CONFIG_EXAMPLE_LCD_INVERT_COLORS
#define ST7735_INVERT_COLORS true
#else
#define ST7735_INVERT_COLORS false
#endif

/* State of the driver */
static spi_device_handle_t s_spi;
static spi_host_device_t s_spi_host;
static int   s_cs_gpio = -1;
static int   s_dc_gpio = -1;
static int   s_rst_gpio = -1;
static int   s_clock_speed_hz;
static int   s_bl_gpio = -1;
static bool  s_bl_active_low;
static bool  s_bl_on;
static st7735_rotation_t s_rotation;
static uint8_t s_madctl;
static int   s_col_start;
static int   s_row_start;
static int   s_width = 128;
static int   s_height = 160;
static int   s_col_start_portrait;
static int   s_row_start_portrait;

/* The panel is used by the task of LVGL and by the tasks of the console,
   the accesses are serialized with this mutex. */
static SemaphoreHandle_t s_mutex;

/* Take the lock of the panel */
static void panel_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

/* Release the lock of the panel */
static void panel_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

/* The offset of the visible area in the memory of the controller. In the
   landscape mode the offsets are swapped. */
static void set_offsets(st7735_rotation_t rotation, int col_start, int row_start)
{
    if (rotation == ST7735_ROTATION_90 || rotation == ST7735_ROTATION_270) {
        s_col_start = row_start;
        s_row_start = col_start;
    } else {
        s_col_start = col_start;
        s_row_start = row_start;
    }
}

/* Send a command to the controller */
static esp_err_t write_command(uint8_t command)
{
    gpio_set_level(s_dc_gpio, 0);

    spi_transaction_t transaction = {
        .length = 8,
        .tx_buffer = &command,
    };
    return spi_device_polling_transmit(s_spi, &transaction);
}

/* Send data to the controller */
static esp_err_t write_data(const void *data, size_t length)
{
    if (length == 0) {
        return ESP_OK;
    }

    gpio_set_level(s_dc_gpio, 1);

    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &transaction);
}

/* Send a command followed by its parameters */
static esp_err_t write_command_with_data(uint8_t command, const uint8_t *data, size_t length)
{
    ESP_RETURN_ON_ERROR(write_command(command), TAG, "write_command() failed");
    return write_data(data, length);
}

/* Reset the controller */
static void reset_panel(void)
{
    gpio_set_level(s_dc_gpio, 1);
    gpio_set_level(s_cs_gpio, 1);

    gpio_set_level(s_rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(s_rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* The initialization sequence of the controller */
static esp_err_t init_sequence(void)
{
    /* Software reset */
    ESP_RETURN_ON_ERROR(write_command(ST7735_CMD_SWRESET), TAG, "SWRESET failed");
    vTaskDelay(pdMS_TO_TICKS(150));

    /* Sleep out and wait for the controller to be ready */
    ESP_RETURN_ON_ERROR(write_command(ST7735_CMD_SLPOUT), TAG, "SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Frame rate */
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_MCTR1, (uint8_t[]){0x01, 0x2C, 0x2C}, 3), TAG, "MCTR1 failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_MCTR2, (uint8_t[]){0x01, 0x2C, 0x2C}, 3), TAG, "MCTR2 failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_MCTR3, (uint8_t[]){0x01, 0x2C, 0x2C}, 3), TAG, "MCTR3 failed");

    /* Power control, the values are the ones of the 1.8 inch modules */
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_PCTR1, (uint8_t[]){0xA2, 0x02, 0x84}, 3), TAG, "PCTR1 failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_PCTR2, (uint8_t[]){0xC5}, 1), TAG, "PCTR2 failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_PCTR3, (uint8_t[]){0x0A, 0x00}, 2), TAG, "PCTR3 failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_PCTR4, (uint8_t[]){0x8A, 0x2A}, 2), TAG, "PCTR4 failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_PCTR5, (uint8_t[]){0x8A, 0xEE, 0x8E}, 3), TAG, "PCTR5 failed");

    /* VCOM */
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_VMCTR1, (uint8_t[]){0x0E}, 1), TAG, "VMCTR1 failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_VMCTR2, (uint8_t[]){0x01}, 1), TAG, "VMCTR2 failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_VMCTR3, (uint8_t[]){0x03}, 1), TAG, "VMCTR3 failed");

    /* Gate control and inversion of the colors */
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_GCTRL, (uint8_t[]){0x03}, 1), TAG, "GCTRL failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_VCOMS, (uint8_t[]){0x03}, 1), TAG, "VCOMS failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_LCCTRL, (uint8_t[]){0x00}, 1), TAG, "LCCTRL failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_VCVS, (uint8_t[]){0x03}, 1), TAG, "VCVS failed");

    /* Gamma */
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_EVSET, (uint8_t[]){
                                  0x0A, 0x0B, 0x06, 0x08, 0x09, 0x0A, 0x0F, 0x08,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 16), TAG, "EVSET failed");

    /* The pixel format is 16 bits per pixel */
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_COLMOD, (uint8_t[]){0x05}, 1), TAG, "COLMOD failed");

    /* The colors are inverted and the screen is switched on */
#if ST7735_INVERT_COLORS
    ESP_RETURN_ON_ERROR(write_command(ST7735_CMD_INVON), TAG, "INVON failed");
#else
    ESP_RETURN_ON_ERROR(write_command(ST7735_CMD_INVOFF), TAG, "INVOFF failed");
#endif
    ESP_RETURN_ON_ERROR(write_command(ST7735_CMD_DISPON), TAG, "DISPON failed");

    return ESP_OK;
}

esp_err_t st7735_init(const st7735_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                            "the mutex of the panel could not be created");
    }

    s_cs_gpio = config->cs_gpio;
    s_dc_gpio = config->dc_gpio;
    s_rst_gpio = config->rst_gpio;
    s_width = config->width > 0 ? config->width : 128;
    s_height = config->height > 0 ? config->height : 160;
    s_col_start_portrait = config->col_start;
    s_row_start_portrait = config->row_start;
    s_bl_gpio = config->bl_gpio;
    s_bl_active_low = config->bl_active_low;
    s_bl_on = false;

    /* The GPIOs of the panel. The CS is driven by the SPI driver, it is not
       configured here. */
    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << config->dc_gpio) | (1ULL << config->rst_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "gpio_config() failed");

    /* The backlight is switched off while the controller is initialized */
    if (s_bl_gpio >= 0) {
        const gpio_config_t bl_config = {
            .pin_bit_mask = 1ULL << s_bl_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&bl_config), TAG, "gpio_config() of the backlight failed");
        st7735_set_backlight(false);
    }

    /* The SPI bus. The maximum size of a transfer is the size of the whole
       memory of the controller, the test patterns use it. */
    const spi_bus_config_t bus_config = {
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = -1,
        .sclk_io_num = config->sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 132 * 162 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(config->spi_host, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize() failed");

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = config->clock_speed_hz > 0 ? config->clock_speed_hz
                                                     : ST7735_DEFAULT_CLOCK_HZ,
        .mode = 0,               /* the controller samples the clock on the rising edge */
        .spics_io_num = config->cs_gpio,
        .queue_size = 1,
    };
    s_spi_host = config->spi_host;
    s_clock_speed_hz = device_config.clock_speed_hz;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(config->spi_host, &device_config, &s_spi),
                        TAG, "spi_bus_add_device() failed");

    reset_panel();
    ESP_RETURN_ON_ERROR(init_sequence(), TAG, "the initialization sequence failed");

    ESP_RETURN_ON_ERROR(st7735_set_rotation(config->rotation), TAG, "st7735_set_rotation() failed");

    ESP_LOGI(TAG, "ST7735 %dx%d ready (offset %d/%d), SPI%d, %d Hz, rotation %d",
             s_width, s_height, s_col_start, s_row_start, config->spi_host + 1,
             config->clock_speed_hz > 0 ? config->clock_speed_hz : ST7735_DEFAULT_CLOCK_HZ,
             (int)config->rotation);

    st7735_set_backlight(true);
    return ESP_OK;
}

/* Write a rectangle of pixels in the memory of the controller. The
   coordinates are the ones of the memory, they are not the ones of the
   visible area. */
static esp_err_t draw_area(int x, int y, int width, int height, const void *data)
{
    const int x1 = x;
    const int y1 = y;
    const int x2 = x1 + width - 1;
    const int y2 = y1 + height - 1;

    const uint8_t columns[] = { (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
                                (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF) };
    const uint8_t rows[] = { (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
                              (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF) };

    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_CASET, columns, sizeof(columns)),
                        TAG, "CASET failed");
    ESP_RETURN_ON_ERROR(write_command_with_data(ST7735_CMD_RASET, rows, sizeof(rows)),
                        TAG, "RASET failed");
    ESP_RETURN_ON_ERROR(write_command(ST7735_CMD_RAMWR), TAG, "RAMWR failed");

    /* The pixels are sent in the RGB565 format, the most significant byte
       of a pixel first */
    const size_t length = (size_t)width * height * 2;
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    gpio_set_level(s_dc_gpio, 1);
    return spi_device_polling_transmit(s_spi, &transaction);
}

esp_err_t st7735_draw_pixels(int x, int y, int width, int height, const void *data)
{
    if (data == NULL || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The visible area is offset in the memory of the controller */
    panel_lock();
    const esp_err_t err = draw_area(s_col_start + x, s_row_start + y, width, height, data);
    panel_unlock();
    return err;
}

esp_err_t st7735_fill_raw(uint16_t color)
{
    /* The whole memory of the controller is filled, whatever the offset of
       the visible area is, the screen shows something. It is used to check
       that the panel is driven correctly.

       The memory is filled band by band, the SPI controller cannot transfer
       more than 4092 bytes at once. */
    const int width = ST7735_MEMORY_WIDTH;
    const int height = ST7735_MEMORY_HEIGHT;
    const int band_height = SPI_MAX_TRANSFER / (width * 2);

    const size_t band_pixels = (size_t)width * band_height;
    uint8_t *buffer = malloc(band_pixels * 2);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < band_pixels; i++) {
        buffer[i * 2] = color >> 8;
        buffer[i * 2 + 1] = color & 0xFF;
    }

    panel_lock();
    esp_err_t err = ESP_OK;
    for (int y = 0; y < height; y += band_height) {
        const int lines = (y + band_height <= height) ? band_height : height - y;
        err = draw_area(0, y, width, lines, buffer);
        if (err != ESP_OK) {
            break;
        }
    }
    panel_unlock();

    free(buffer);
    return err;
}

esp_err_t st7735_set_rotation(st7735_rotation_t rotation)
{
    /* The color order of the module, most of them are in the BGR order */
    uint8_t madctl = 0;
#if CONFIG_EXAMPLE_LCD_BGR_ORDER
    madctl |= ST7735_MADCTL_BGR;
#endif

    switch (rotation) {
    case ST7735_ROTATION_0:
        madctl |= ST7735_MADCTL_MX | ST7735_MADCTL_MY;
        break;
    case ST7735_ROTATION_90:
        madctl |= ST7735_MADCTL_MY;
        break;
    case ST7735_ROTATION_180:
        madctl |= ST7735_MADCTL_MX;
        break;
    case ST7735_ROTATION_270:
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    set_offsets(rotation, s_col_start_portrait, s_row_start_portrait);

    panel_lock();
    const esp_err_t err = write_command_with_data(ST7735_CMD_MADCTL, &madctl, 1);
    panel_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "MADCTL failed");

    s_madctl = madctl;
    s_rotation = rotation;
    return ESP_OK;
}

st7735_rotation_t st7735_get_rotation(void)
{
    return s_rotation;
}

void st7735_get_size(int *width, int *height)
{
    const bool landscape = (s_rotation == ST7735_ROTATION_90 ||
                            s_rotation == ST7735_ROTATION_270);
    if (width != NULL) {
        *width = landscape ? s_height : s_width;
    }
    if (height != NULL) {
        *height = landscape ? s_width : s_height;
    }
}

void st7735_set_backlight(bool on)
{
    if (s_bl_gpio < 0) {
        return;
    }

    s_bl_on = on;
    if (s_bl_active_low) {
        gpio_set_level(s_bl_gpio, on ? 0 : 1);
    } else {
        gpio_set_level(s_bl_gpio, on ? 1 : 0);
    }
}

bool st7735_backlight_is_on(void)
{
    return s_bl_on;
}

esp_err_t st7735_set_madctl(uint8_t value)
{
    panel_lock();
    const esp_err_t err = write_command_with_data(ST7735_CMD_MADCTL, &value, 1);
    panel_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "MADCTL failed");

    s_madctl = value;
    ESP_LOGI(TAG, "MADCTL set to 0x%02X", value);
    return ESP_OK;
}

uint8_t st7735_get_madctl(void)
{
    return s_madctl;
}

esp_err_t st7735_set_offset(int col, int row)
{
    if (col < 0 || col > 132 || row < 0 || row > 162) {
        return ESP_ERR_INVALID_ARG;
    }

    s_col_start = col;
    s_row_start = row;
    ESP_LOGI(TAG, "Offset of the visible area set to %d/%d", col, row);
    return ESP_OK;
}

void st7735_get_offset(int *col, int *row)
{
    if (col != NULL) {
        *col = s_col_start;
    }
    if (row != NULL) {
        *row = s_row_start;
    }
}

esp_err_t st7735_fill(uint16_t color)
{
    int width = 0;
    int height = 0;
    st7735_get_size(&width, &height);

    /* The screen is filled band by band, the SPI controller cannot transfer
       more than 4092 bytes at once. */
    int band_height = SPI_MAX_TRANSFER / (width * 2);
    if (band_height > height) {
        band_height = height;
    }
    if (band_height < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t band_pixels = (size_t)width * band_height;
    uint8_t *buffer = malloc(band_pixels * 2);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < band_pixels; i++) {
        buffer[i * 2] = color >> 8;
        buffer[i * 2 + 1] = color & 0xFF;
    }

    esp_err_t err = ESP_OK;
    for (int y = 0; y < height; y += band_height) {
        const int lines = (y + band_height <= height) ? band_height : height - y;
        err = st7735_draw_pixels(0, y, width, lines, buffer);
        if (err != ESP_OK) {
            break;
        }
    }

    free(buffer);
    return err;
}

esp_err_t st7735_draw_map_pattern(void)
{
    int width = 0;
    int height = 0;
    st7735_get_size(&width, &height);

    /* A white background */
    ESP_RETURN_ON_ERROR(st7735_fill(0xFFFF), TAG, "st7735_fill() failed");

    /* A red block in the top left corner */
    const uint8_t red[] = { 0xF8, 0x00 };
    for (int y = 0; y < 16; y++) {
        ESP_RETURN_ON_ERROR(st7735_draw_pixels(0, y, 16, 1, red), TAG, "red block failed");
    }

    /* A blue block in the bottom right corner */
    const uint8_t blue[] = { 0x00, 0x1F };
    for (int y = 0; y < 16; y++) {
        ESP_RETURN_ON_ERROR(st7735_draw_pixels(width - 16, height - 16 + y, 16, 1, blue),
                            TAG, "blue block failed");
    }

    /* A green line in the middle of the height */
    const uint8_t green[] = { 0x07, 0xE0 };
    for (int x = 0; x < width; x++) {
        ESP_RETURN_ON_ERROR(st7735_draw_pixels(x, height / 2, 1, 1, green),
                            TAG, "green line failed");
    }

    /* An orange line in the middle of the width */
    const uint8_t orange[] = { 0xFF, 0xE0 };
    for (int y = 0; y < height; y++) {
        ESP_RETURN_ON_ERROR(st7735_draw_pixels(width / 2, y, 1, 1, orange),
                            TAG, "orange line failed");
    }

    ESP_LOGI(TAG, "Mapping pattern drawn (red=top left, blue=bottom right)");
    return ESP_OK;
}

esp_err_t st7735_draw_test_pattern(void)
{
    int width = 0;
    int height = 0;
    st7735_get_size(&width, &height);

    /* A black background */
    ESP_RETURN_ON_ERROR(st7735_fill(0x0000), TAG, "st7735_fill() failed");

    /* A white border around the visible area */
    const uint16_t border = 0xFFFF;
    for (int x = 0; x < width; x++) {
        ESP_RETURN_ON_ERROR(st7735_draw_pixels(x, 0, 1, 1, &border), TAG, "border failed");
        ESP_RETURN_ON_ERROR(st7735_draw_pixels(x, height - 1, 1, 1, &border), TAG, "border failed");
    }
    for (int y = 0; y < height; y++) {
        ESP_RETURN_ON_ERROR(st7735_draw_pixels(0, y, 1, 1, &border), TAG, "border failed");
        ESP_RETURN_ON_ERROR(st7735_draw_pixels(width - 1, y, 1, 1, &border), TAG, "border failed");
    }

    /* Four colored blocks, one in each corner */
    const struct {
        int      x;
        int      y;
        uint16_t color;
    } blocks[] = {
        { 2,          2,          0xF800 },   /* red */
        { width - 12, 2,          0x07E0 },   /* green */
        { 2,          height - 12, 0x001F },   /* blue */
        { width - 12, height - 12, 0xFFE0 },   /* yellow */
    };
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        const uint8_t color[] = { (uint8_t)(blocks[i].color >> 8),
                                  (uint8_t)(blocks[i].color & 0xFF) };
        for (int row = 0; row < 10; row++) {
            ESP_RETURN_ON_ERROR(st7735_draw_pixels(blocks[i].x, blocks[i].y + row, 10, 1, color),
                                TAG, "block failed");
        }
    }

    ESP_LOGI(TAG, "Test pattern drawn");
    return ESP_OK;
}

esp_err_t st7735_set_clock(int clock_speed_hz)
{
    if (s_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (clock_speed_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The device is removed and added again with the new speed */
    panel_lock();
    ESP_RETURN_ON_ERROR(spi_bus_remove_device(s_spi), TAG,
                        "spi_bus_remove_device() failed");
    s_spi = NULL;

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = clock_speed_hz,
        .mode = 0,
        .spics_io_num = s_cs_gpio,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(s_spi_host, &device_config, &s_spi),
                        TAG, "spi_bus_add_device() failed");
    panel_unlock();

    s_clock_speed_hz = clock_speed_hz;
    ESP_LOGI(TAG, "The SPI clock is %d Hz", clock_speed_hz);
    return ESP_OK;
}

int st7735_get_clock(void)
{
    return s_clock_speed_hz;
}
