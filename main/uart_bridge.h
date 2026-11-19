/*
 * UART peripherals
 *
 * The three UART peripherals of the ESP32 (UART0, UART1 and UART2) are used
 * to exchange data with the outside world:
 *
 *   - the data received from the TCP client is written to the UART,
 *   - the data received from the UART is sent to the TCP client.
 *
 * The TCP servers are implemented in tcp_server.c, this module only takes
 * care of the UART peripherals themselves.
 *
 * The driver of a UART is installed only when its server is started, so that
 * the GPIOs of the UARTs that are not used stay available for something else.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of UART peripherals used by the bridge (UART0, UART1 and UART2) */
#define UART_BRIDGE_COUNT 3

/**
 * @brief  Initialize the UART peripherals
 *
 * Only the settings are prepared here, the drivers are installed by
 * uart_bridge_open() when the server of a UART is started.
 *
 * @return ESP_OK on success
 */
esp_err_t uart_bridge_init(void);

/**
 * @brief  Install the driver of a UART and apply its settings
 *
 * @param uart_num number of the UART (0, 1 or 2)
 *
 * @return ESP_OK              on success
 *         ESP_ERR_INVALID_ARG  if the UART number is out of range
 *         ESP_ERR_INVALID_STATE if the driver is already installed
 */
esp_err_t uart_bridge_open(int uart_num);

/**
 * @brief  Uninstall the driver of a UART
 *
 * @param uart_num number of the UART (0, 1 or 2)
 *
 * @return ESP_OK              on success
 *         ESP_ERR_INVALID_ARG  if the UART number is out of range
 *         ESP_ERR_INVALID_STATE if the driver was not installed
 */
esp_err_t uart_bridge_close(int uart_num);

/**
 * @brief  Whether the driver of a UART is installed
 */
bool uart_bridge_is_open(int uart_num);

/**
 * @brief  Change the baud rate of a UART
 *
 * The new baud rate is applied at once if the driver is installed.
 *
 * @param uart_num  number of the UART (0, 1 or 2)
 * @param baud_rate baud rate in bits per second
 */
esp_err_t uart_bridge_set_baudrate(int uart_num, int baud_rate);

/**
 * @brief  Get the baud rate of a UART
 */
int uart_bridge_get_baudrate(int uart_num);

/**
 * @brief  Change the GPIO pins of a UART
 *
 * The new pins are applied at once if the driver is installed.
 *
 * @param uart_num number of the UART (0, 1 or 2)
 * @param tx_gpio  GPIO the TX signal is routed to
 * @param rx_gpio  GPIO the RX signal is routed to
 */
esp_err_t uart_bridge_set_pins(int uart_num, int tx_gpio, int rx_gpio);

/**
 * @brief  Get the GPIO pins of a UART
 *
 * @param[out] tx_gpio GPIO the TX signal is routed to (may be NULL)
 * @param[out] rx_gpio GPIO the RX signal is routed to (may be NULL)
 */
void uart_bridge_get_pins(int uart_num, int *tx_gpio, int *rx_gpio);

/**
 * @brief  Send data to a UART
 *
 * The data is written to the transmit buffer of the driver, the function does
 * not wait for the transmission to be done. If the buffer is full, only the
 * data that fits in it is written.
 *
 * @param uart_num number of the UART (0, 1 or 2)
 * @param data     data to send
 * @param len      number of bytes to send
 *
 * @return the number of bytes written, 0 if the buffer was full, -1 on error
 */
int uart_bridge_write(int uart_num, const uint8_t *data, size_t len);

/**
 * @brief  Read the data buffered by the driver of a UART
 *
 * The function does not wait for the data, it returns what is already in the
 * receive buffer of the driver.
 *
 * @param uart_num number of the UART (0, 1 or 2)
 * @param buf      destination buffer
 * @param max_len  size of the destination buffer
 *
 * @return the number of bytes read, -1 on error
 */
int uart_bridge_read(int uart_num, uint8_t *buf, size_t max_len);

#ifdef __cplusplus
}
#endif
