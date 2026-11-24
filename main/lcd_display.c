/*
 * Display of the system
 *
 * See lcd_display.h for the description of the module.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lcd_display.h"
#include "lcd_ui.h"
#include "st7735.h"

static const char *TAG = "lcd_display";

/* Number of lines of the draw buffer of the display */
#define LCD_BUFFER_LINES 12

/* The SPI controller cannot transfer more than 4092 bytes at once, the draw
   buffer has to fit in one transfer */
#define LCD_MAX_TRANSFER 4092

/* Size of the stack of the LVGL task */
#define LCD_TASK_STACK_SIZE 6144

/* Priority of the LVGL task */
#define LCD_TASK_PRIORITY 4

static lv_display_t   *s_display;
static void            *s_buffer;
static TaskHandle_t     s_task;
static bool             s_ready;

/* The number of the frames drawn on the screen since the last call to
   lcd_display_take_frame_count() */
static uint32_t         s_frame_count;

/* The tick of LVGL is driven by the esp_timer */
static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* The pixels are sent to the controller in the RGB565 format, the most
   significant byte of a pixel first. LVGL stores them in the opposite order,
   the bytes are swapped here, in place, in the draw buffer. */
static void swap_bytes(uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i += 2) {
        uint8_t tmp = data[i];
        data[i] = data[i + 1];
        data[i + 1] = tmp;
    }
}

/* Send the area to the panel */
static void flush_callback(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const int width = area->x2 - area->x1 + 1;
    const int height = area->y2 - area->y1 + 1;

    /* The first flushes are logged, they show that the display works */
    static int flush_count;
    if (flush_count < 5) {
        flush_count++;
        ESP_LOGI(TAG, "Flush %d: %dx%d at %d,%d", flush_count, width, height,
                 area->x1, area->y1);
    }

    swap_bytes(pixels, (size_t)width * height * 2);
    st7735_draw_pixels(area->x1, area->y1, width, height, pixels);

    s_frame_count++;

    lv_display_flush_ready(display);
}

/* The task that runs the LVGL handler */
static void lvgl_task(void *arg)
{
    while (true) {
        uint32_t delay = lv_timer_handler();
        if (delay < 5) {
            delay = 5;
        } else if (delay > 100) {
            delay = 100;
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

esp_err_t lcd_display_init(void)
{
    ESP_RETURN_ON_FALSE(s_display == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "the display is already initialized");

    /* The value of the menuconfig is the number of the peripheral (1, 2 or
       3), the enumeration of the driver starts at 0. On the ESP32-S3 the
       SPI3 is the bus of the flash and of the PSRAM, it must not be used. */
    const st7735_config_t panel_config = {
        .spi_host = (spi_host_device_t)(CONFIG_EXAMPLE_LCD_SPI_HOST - 1),
        .sclk_gpio = CONFIG_EXAMPLE_LCD_SCLK_GPIO,
        .mosi_gpio = CONFIG_EXAMPLE_LCD_MOSI_GPIO,
        .cs_gpio = CONFIG_EXAMPLE_LCD_CS_GPIO,
        .dc_gpio = CONFIG_EXAMPLE_LCD_DC_GPIO,
        .rst_gpio = CONFIG_EXAMPLE_LCD_RST_GPIO,
        .bl_gpio = CONFIG_EXAMPLE_LCD_BL_GPIO,
#if CONFIG_EXAMPLE_LCD_BL_ACTIVE_LOW
        .bl_active_low = true,
#else
        .bl_active_low = false,
#endif
        .clock_speed_hz = CONFIG_EXAMPLE_LCD_SPI_CLOCK_HZ,
        .rotation = CONFIG_EXAMPLE_LCD_ROTATION,
        .width = CONFIG_EXAMPLE_LCD_WIDTH,
        .height = CONFIG_EXAMPLE_LCD_HEIGHT,
        .col_start = CONFIG_EXAMPLE_LCD_COL_START,
        .row_start = CONFIG_EXAMPLE_LCD_ROW_START,
    };

    ESP_RETURN_ON_ERROR(st7735_init(&panel_config), TAG, "st7735_init() failed");

    lv_init();
    lv_tick_set_cb(tick_get_cb);

    int width = 0;
    int height = 0;
    st7735_get_size(&width, &height);

    s_display = lv_display_create(width, height);
    ESP_RETURN_ON_FALSE(s_display != NULL, ESP_ERR_NO_MEM, TAG, "lv_display_create() failed");

    /* The draw buffer is sent to the panel by the SPI driver, it has to be
       in a memory that the DMA controller can access. It also has to fit in
       one SPI transfer. */
    int lines = LCD_MAX_TRANSFER / (width * sizeof(uint16_t));
    if (lines > LCD_BUFFER_LINES) {
        lines = LCD_BUFFER_LINES;
    }
    if (lines < 1) {
        lines = 1;
    }
    const size_t buffer_size = (size_t)width * lines * sizeof(uint16_t);
    s_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_buffer != NULL, ESP_ERR_NO_MEM, TAG,
                        "the draw buffer could not be allocated");

    lv_display_set_buffers(s_display, s_buffer, NULL, buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush_callback);

    /* The dashboard of the system */
    ESP_RETURN_ON_ERROR(lcd_ui_create(), TAG, "lcd_ui_create() failed");

    if (xTaskCreate(lvgl_task, "lvgl", LCD_TASK_STACK_SIZE, NULL, LCD_TASK_PRIORITY,
                    &s_task) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create the LVGL task");
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Display ready (%dx%d)", width, height);
    return ESP_OK;
}

void lcd_display_deinit(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_display != NULL) {
        lv_display_delete(s_display);
        s_display = NULL;
    }
    if (s_buffer != NULL) {
        heap_caps_free(s_buffer);
        s_buffer = NULL;
    }
    s_ready = false;
}

bool lcd_display_is_ready(void)
{
    return s_ready;
}

uint32_t lcd_display_take_frame_count(void)
{
    const uint32_t count = s_frame_count;
    s_frame_count = 0;
    return count;
}

lv_display_t *lcd_display_get(void)
{
    return s_display;
}

void lcd_display_lock(void)
{
    lv_lock();
}

void lcd_display_unlock(void)
{
    lv_unlock();
}

void lcd_display_get_size(int *width, int *height)
{
    st7735_get_size(width, height);
}

esp_err_t lcd_display_set_rotation(int rotation)
{
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The LVGL objects are protected by the lock of the display, the command
       can be called from another task than the LVGL one */
    lv_lock();

    esp_err_t err = st7735_set_rotation((st7735_rotation_t)rotation);
    if (err != ESP_OK) {
        lv_unlock();
        ESP_RETURN_ON_ERROR(err, TAG, "st7735_set_rotation() failed");
    }

    /* The size of the screen changed, the draw buffer and the dashboard have
       to be rebuilt */
    int width = 0;
    int height = 0;
    st7735_get_size(&width, &height);

    int lines = LCD_MAX_TRANSFER / (width * sizeof(uint16_t));
    if (lines > LCD_BUFFER_LINES) {
        lines = LCD_BUFFER_LINES;
    }
    if (lines < 1) {
        lines = 1;
    }
    const size_t buffer_size = (size_t)width * lines * sizeof(uint16_t);
    void *buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buffer == NULL) {
        lv_unlock();
        ESP_LOGE(TAG, "The draw buffer could not be allocated");
        return ESP_ERR_NO_MEM;
    }

    heap_caps_free(s_buffer);
    s_buffer = buffer;

    lv_display_set_resolution(s_display, width, height);
    lv_display_set_buffers(s_display, s_buffer, NULL, buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lcd_ui_recreate();

    lv_unlock();
    return ESP_OK;
}

int lcd_display_get_rotation(void)
{
    return (int)st7735_get_rotation();
}

void lcd_display_refresh(void)
{
    if (s_display == NULL) {
        return;
    }

    /* The whole screen is invalidated, it is drawn again by the task of
       LVGL. It is useful after a test pattern was drawn directly on the
       panel. */
    lv_lock();
    lv_obj_invalidate(lv_screen_active());
    lv_unlock();
}

void lcd_display_set_backlight(bool on)
{
    st7735_set_backlight(on);
}

bool lcd_display_backlight_is_on(void)
{
    return st7735_backlight_is_on();
}
