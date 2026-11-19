/*
 * Dashboard of the system
 *
 * The screen shows the state of the system: the Wi-Fi connection, the TCP
 * servers, the CPU load, the free memory, the running time and the state of
 * the status LED.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Create the dashboard
 *
 * The screen is created and a timer refreshes its content every second.
 *
 * @return ESP_OK on success
 */
esp_err_t lcd_ui_create(void);

/**
 * @brief  Delete the dashboard and create it again
 *
 * It is used when the orientation of the screen changes.
 */
void lcd_ui_recreate(void);

#ifdef __cplusplus
}
#endif
