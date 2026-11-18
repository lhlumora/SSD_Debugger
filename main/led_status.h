/*
 * Status LED
 *
 * A dedicated task drives a single LED to show the state of the system.
 * The LED is connected to the GPIO in active low configuration: the LED is
 * lit while the pin is driven low.
 *
 * The states and their patterns:
 *
 *   booting         fast blink       the system is starting up
 *   wifi down       slow blink       no AP connected
 *   wifi up         solid on         connected to an AP, server stopped
 *   server running  double blink     the TCP server is listening
 *   error           rapid blink      something went wrong
 *
 * The state is derived automatically from the Wi-Fi and TCP server status.
 * It can be overridden manually with led_status_set().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATE_BOOTING = 0,   /* the system is starting up */
    LED_STATE_WIFI_DOWN,     /* not connected to an AP */
    LED_STATE_WIFI_UP,       /* connected to an AP, server not running */
    LED_STATE_SERVER_RUN,    /* the TCP server is listening */
    LED_STATE_ERROR,         /* error */
    LED_STATE_COUNT,
} led_state_t;

/**
 * @brief  Configure the LED GPIO and start the status task
 *
 * @param gpio GPIO the LED is connected to (active low)
 *
 * @return ESP_OK on success
 */
esp_err_t led_status_init(gpio_num_t gpio);

/**
 * @brief  Stop the status task
 */
esp_err_t led_status_deinit(void);

/**
 * @brief  Force a state, this disables the automatic state detection
 *
 * @param state the state to show
 *
 * @return ESP_OK on success
 */
esp_err_t led_status_set(led_state_t state);

/**
 * @brief  Let the task derive the state from the Wi-Fi / TCP server status again
 */
void led_status_set_auto(void);

/**
 * @brief  Get the state currently shown by the LED
 */
led_state_t led_status_get(void);

/**
 * @brief  Get a printable name of a state
 */
const char *led_status_state_name(led_state_t state);

#ifdef __cplusplus
}
#endif
