/*
 * TCP echo server - public API
 *
 * The server task is not started automatically any more. It is started and
 * stopped on demand from the console (see app_console.c).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Start the TCP echo server (creates the "tcp_server" task)
 *
 * @return ESP_OK              on success
 *         ESP_ERR_INVALID_STATE if the server is already running
 *         ESP_ERR_NO_MEM       if the task could not be created
 */
esp_err_t tcp_server_start(void);

/**
 * @brief  Stop the TCP echo server
 *
 * The task closes the listening socket and exits by itself, the function
 * returns once the task is gone (or after a timeout).
 *
 * @return ESP_OK              on success
 *         ESP_ERR_INVALID_STATE if the server was not running
 *         ESP_ERR_TIMEOUT      if the task did not exit in time
 */
esp_err_t tcp_server_stop(void);

/**
 * @brief  Whether the TCP server task is currently running
 */
bool tcp_server_is_running(void);

/**
 * @brief  Port the TCP server listens on (CONFIG_EXAMPLE_PORT)
 */
int tcp_server_port(void);

#ifdef __cplusplus
}
#endif
