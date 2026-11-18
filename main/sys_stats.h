/*
 * System statistics for the console
 *
 *   tasks   list the FreeRTOS tasks (state, priority, stack usage, run time)
 *   mem     show the memory usage (internal RAM, PSRAM)
 *   cpu     show the CPU load, measured over one second
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Register the statistics commands in the console
 *
 * @return ESP_OK on success
 */
esp_err_t sys_stats_register(void);

#ifdef __cplusplus
}
#endif
