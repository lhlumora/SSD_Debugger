/*
 * Interactive console
 *
 * Starts the ESP-IDF console REPL and registers the commands used to
 * select a Wi-Fi AP and to start/stop the TCP server at runtime.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialize the console and start the REPL
 */
esp_err_t app_console_start(void);

#ifdef __cplusplus
}
#endif
