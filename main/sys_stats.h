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

/**
 * @brief  Get the CPU load of the system, in percent
 *
 * The load is measured in the background, every second, by the statistics
 * task. The value is the load of one core, it can be higher than 100% when
 * the two cores of the ESP32-S3 are busy.
 */
unsigned sys_stats_get_cpu_load(void);

/**
 * @brief  Get the CPU load of one core, in percent
 *
 * @param core number of the core
 */
unsigned sys_stats_get_core_load(int core);

#ifdef __cplusplus
}
#endif
