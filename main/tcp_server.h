/*
 * TCP servers - public API
 *
 * One TCP server per UART peripheral of the ESP32:
 *
 *   UART0   ->   port CONFIG_EXAMPLE_PORT      (3333 by default)
 *   UART1   ->   port CONFIG_EXAMPLE_PORT + 1  (3334 by default)
 *   UART2   ->   port CONFIG_EXAMPLE_PORT + 2  (3335 by default)
 *
 * The data received from the client is written to the UART of the server and
 * the data received from that UART is sent to the client.
 *
 * The servers are not started automatically any more. They are started and
 * stopped on demand from the console (see app_console.c), so that only the
 * UARTs that are really used are running.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Statistics of one TCP server */
typedef struct {
    int      port;           /* TCP port of the server */
    bool     running;        /* the server task is running */
    bool     client;         /* a client is connected */
    uint32_t rx_bytes;       /* bytes received from the client (sent to the UART) */
    uint32_t tx_bytes;       /* bytes sent to the client (received from the UART) */
    uint32_t dropped_bytes;  /* bytes that could not be forwarded (no client connected, or the UART was busy) */
} tcp_server_stats_t;

/**
 * @brief  Start the TCP server of a UART (creates the task of the server)
 *
 * The driver of the UART is installed if it is not already.
 *
 * @param uart_num number of the UART (0, 1 or 2)
 *
 * @return ESP_OK              on success
 *         ESP_ERR_INVALID_ARG  if the UART number is out of range
 *         ESP_ERR_INVALID_STATE if the server is already running
 *         ESP_ERR_NO_MEM       if the task could not be created
 */
esp_err_t tcp_server_start(int uart_num);

/**
 * @brief  Stop the TCP server of a UART
 *
 * The task closes the listening socket and exits by itself, the function
 * returns once the task is gone (or after a timeout). The driver of the UART
 * is uninstalled afterwards.
 *
 * @param uart_num number of the UART (0, 1 or 2)
 *
 * @return ESP_OK              on success
 *         ESP_ERR_INVALID_ARG  if the UART number is out of range
 *         ESP_ERR_INVALID_STATE if the server was not running
 *         ESP_ERR_TIMEOUT      if the task did not exit in time
 */
esp_err_t tcp_server_stop(int uart_num);

/**
 * @brief  Start the TCP servers of all the UARTs
 *
 * The UARTs whose server is already running are left untouched.
 *
 * @return ESP_OK if at least one server was started
 */
esp_err_t tcp_server_start_all(void);

/**
 * @brief  Stop the TCP servers of all the UARTs
 *
 * @return ESP_OK if at least one server was stopped
 */
esp_err_t tcp_server_stop_all(void);

/**
 * @brief  Whether the TCP server of a UART is currently running
 */
bool tcp_server_is_running(int uart_num);

/**
 * @brief  Whether at least one TCP server is currently running
 */
bool tcp_server_any_running(void);

/**
 * @brief  Port the TCP server of a UART listens on
 *
 * The port of UART0 is CONFIG_EXAMPLE_PORT, the port of UART1 and UART2 is
 * CONFIG_EXAMPLE_PORT + 1 and CONFIG_EXAMPLE_PORT + 2.
 */
int tcp_server_port(int uart_num);

/**
 * @brief  Get the statistics of the TCP server of a UART
 */
void tcp_server_get_stats(int uart_num, tcp_server_stats_t *stats);

#ifdef __cplusplus
}
#endif
