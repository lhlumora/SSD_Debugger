/*
 * Status LED
 *
 * See led_status.h for the description of the states.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#include "led_status.h"
#include "tcp_server.h"
#include "wifi_manager.h"

static const char *TAG = "led_status";

/* The LED is lit while the pin is driven low */
#define LED_LEVEL_ON  0
#define LED_LEVEL_OFF 1

/* Resolution of the state polling, in milliseconds */
#define LED_TICK_MS 20

/* One step of a blinking pattern */
typedef struct {
    uint16_t duration_ms;
    bool     on;
} led_step_t;

/* A blinking pattern, cycled over and over */
typedef struct {
    const led_step_t *steps;
    size_t            num_steps;
} led_pattern_t;

/* Fast blink: the system is starting up */
static const led_step_t s_steps_booting[] = {
    { 120, true  },
    { 120, false },
};

/* Slow blink: no AP connected */
static const led_step_t s_steps_wifi_down[] = {
    { 1000, true  },
    { 1000, false },
};

/* Solid on: connected to an AP */
static const led_step_t s_steps_wifi_up[] = {
    { 1000, true },
};

/* Double blink: the TCP server is listening */
static const led_step_t s_steps_server_run[] = {
    { 150, true  },
    { 150, false },
    { 150, true  },
    { 850, false },
};

/* Rapid blink: error */
static const led_step_t s_steps_error[] = {
    { 60, true  },
    { 60, false },
};

static const led_pattern_t s_patterns[LED_STATE_COUNT] = {
    [LED_STATE_BOOTING]    = { s_steps_booting,    sizeof(s_steps_booting)    / sizeof(s_steps_booting[0])    },
    [LED_STATE_WIFI_DOWN]  = { s_steps_wifi_down,  sizeof(s_steps_wifi_down)  / sizeof(s_steps_wifi_down[0])  },
    [LED_STATE_WIFI_UP]    = { s_steps_wifi_up,    sizeof(s_steps_wifi_up)    / sizeof(s_steps_wifi_up[0])    },
    [LED_STATE_SERVER_RUN] = { s_steps_server_run, sizeof(s_steps_server_run) / sizeof(s_steps_server_run[0]) },
    [LED_STATE_ERROR]      = { s_steps_error,      sizeof(s_steps_error)      / sizeof(s_steps_error[0])      },
};

static gpio_num_t s_gpio = GPIO_NUM_NC;
static volatile led_state_t s_state = LED_STATE_BOOTING;
static volatile bool s_auto = true;
static TaskHandle_t s_task;

/* Drive the LED, @p on is the logical state (the pin is inverted) */
static void led_write(bool on)
{
    gpio_set_level(s_gpio, on ? LED_LEVEL_ON : LED_LEVEL_OFF);
}

/* Derive the state from the actual status of the system */
static led_state_t detect_state(void)
{
    if (!wifi_manager_is_connected()) {
        return LED_STATE_WIFI_DOWN;
    }
    if (tcp_server_is_running()) {
        return LED_STATE_SERVER_RUN;
    }
    return LED_STATE_WIFI_UP;
}

/* The state the LED should show right now */
static led_state_t current_state(void)
{
    if (s_auto) {
        return detect_state();
    }
    return s_state;
}

static void led_task(void *arg)
{
    led_state_t shown = LED_STATE_BOOTING;
    size_t step = 0;

    while (true) {
        led_state_t wanted = current_state();
        if (wanted != shown) {
            ESP_LOGI(TAG, "State changed: %s -> %s",
                     led_status_state_name(shown), led_status_state_name(wanted));
            shown = wanted;
            step = 0;
        }

        const led_pattern_t *pattern = &s_patterns[shown];
        const led_step_t *current = &pattern->steps[step % pattern->num_steps];
        led_write(current->on);

        /* Wait for the duration of the step, but wake up early when the
           state changes */
        uint32_t waited = 0;
        while (waited < current->duration_ms) {
            uint32_t chunk = current->duration_ms - waited;
            if (chunk > LED_TICK_MS) {
                chunk = LED_TICK_MS;
            }
            vTaskDelay(pdMS_TO_TICKS(chunk));
            waited += chunk;
            if (current_state() != shown) {
                break;
            }
        }

        step++;
    }
}

esp_err_t led_status_init(gpio_num_t gpio)
{
    if (gpio == GPIO_NUM_NC || gpio < 0) {
        ESP_LOGE(TAG, "Invalid LED GPIO");
        return ESP_ERR_INVALID_ARG;
    }

    s_gpio = gpio;
    s_state = LED_STATE_BOOTING;
    s_auto = true;

    /* Set the idle level before the pin becomes an output, so that the LED
       does not flash when the GPIO is configured. */
    gpio_set_level(gpio, LED_LEVEL_OFF);

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "gpio_config() failed");

    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (xTaskCreate(led_task, "led_status", 3072, NULL, 3, &s_task) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create the LED task");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Status LED started on GPIO%d", gpio);
    return ESP_OK;
}

esp_err_t led_status_deinit(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_gpio != GPIO_NUM_NC) {
        led_write(false);
    }
    return ESP_OK;
}

esp_err_t led_status_set(led_state_t state)
{
    if (state >= LED_STATE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_auto = false;
    s_state = state;
    return ESP_OK;
}

void led_status_set_auto(void)
{
    s_auto = true;
}

led_state_t led_status_get(void)
{
    return current_state();
}

const char *led_status_state_name(led_state_t state)
{
    switch (state) {
    case LED_STATE_BOOTING:    return "booting";
    case LED_STATE_WIFI_DOWN:  return "wifi down";
    case LED_STATE_WIFI_UP:    return "wifi up";
    case LED_STATE_SERVER_RUN: return "server running";
    case LED_STATE_ERROR:      return "error";
    default:                   return "unknown";
    }
}
