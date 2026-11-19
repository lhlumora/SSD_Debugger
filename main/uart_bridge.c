/*
 * UART peripherals
 *
 * See uart_bridge.h for the description of the module.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "uart_bridge.h"

static const char *TAG = "uart_bridge";

/* Size of the receive and of the transmit buffer of the UART driver, in bytes.
   The receive buffer must be large enough to hold the data that arrives
   between two reads by the TCP server. */
#define UART_RX_BUFFER_SIZE 1024
#define UART_TX_BUFFER_SIZE 1024

/* Settings of one UART peripheral */
typedef struct {
    int  uart_num;
    int  tx_gpio;
    int  rx_gpio;
    int  baud_rate;
    bool driver_installed;
} uart_info_t;

/* The default settings come from the menuconfig, they can be changed at
   runtime with the 'uart_pins' and 'uart_baud' console commands. */
static uart_info_t s_uarts[UART_BRIDGE_COUNT] = {
    {
        .uart_num = 0,
        .tx_gpio = CONFIG_EXAMPLE_UART0_TX_GPIO,
        .rx_gpio = CONFIG_EXAMPLE_UART0_RX_GPIO,
        .baud_rate = CONFIG_EXAMPLE_UART_BAUD,
        .driver_installed = false,
    },
    {
        .uart_num = 1,
        .tx_gpio = CONFIG_EXAMPLE_UART1_TX_GPIO,
        .rx_gpio = CONFIG_EXAMPLE_UART1_RX_GPIO,
        .baud_rate = CONFIG_EXAMPLE_UART_BAUD,
        .driver_installed = false,
    },
    {
        .uart_num = 2,
        .tx_gpio = CONFIG_EXAMPLE_UART2_TX_GPIO,
        .rx_gpio = CONFIG_EXAMPLE_UART2_RX_GPIO,
        .baud_rate = CONFIG_EXAMPLE_UART_BAUD,
        .driver_installed = false,
    },
};

/* Return the settings of a UART, NULL if the number is out of range */
static uart_info_t *uart_get(int uart_num)
{
    if (uart_num < 0 || uart_num >= UART_BRIDGE_COUNT) {
        return NULL;
    }
    return &s_uarts[uart_num];
}

/* Apply the baud rate and the GPIO pins of a UART */
static esp_err_t uart_apply_settings(const uart_info_t *info)
{
    const uart_config_t config = {
        .baud_rate = info->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(info->uart_num, &config), TAG,
                        "uart_param_config() failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(info->uart_num, info->tx_gpio, info->rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG,
                        "uart_set_pin() failed");

    return ESP_OK;
}

esp_err_t uart_bridge_init(void)
{
    ESP_LOGI(TAG, "UART0: TX=%d RX=%d, UART1: TX=%d RX=%d, UART2: TX=%d RX=%d, %d baud",
             s_uarts[0].tx_gpio, s_uarts[0].rx_gpio,
             s_uarts[1].tx_gpio, s_uarts[1].rx_gpio,
             s_uarts[2].tx_gpio, s_uarts[2].rx_gpio,
             s_uarts[0].baud_rate);
    return ESP_OK;
}

esp_err_t uart_bridge_open(int uart_num)
{
    uart_info_t *info = uart_get(uart_num);
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (info->driver_installed) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(uart_driver_install(info->uart_num, UART_RX_BUFFER_SIZE,
                                            UART_TX_BUFFER_SIZE, 0, NULL, 0), TAG,
                        "uart_driver_install() failed");
    info->driver_installed = true;

    esp_err_t err = uart_apply_settings(info);
    if (err != ESP_OK) {
        uart_driver_delete(info->uart_num);
        info->driver_installed = false;
        return err;
    }

    ESP_LOGI(TAG, "UART%d opened: TX=%d RX=%d %d baud", info->uart_num,
             info->tx_gpio, info->rx_gpio, info->baud_rate);
    return ESP_OK;
}

esp_err_t uart_bridge_close(int uart_num)
{
    uart_info_t *info = uart_get(uart_num);
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!info->driver_installed) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(uart_driver_delete(info->uart_num), TAG,
                        "uart_driver_delete() failed");
    info->driver_installed = false;

    ESP_LOGI(TAG, "UART%d closed", info->uart_num);
    return ESP_OK;
}

bool uart_bridge_is_open(int uart_num)
{
    uart_info_t *info = uart_get(uart_num);
    return (info != NULL) && info->driver_installed;
}

esp_err_t uart_bridge_set_baudrate(int uart_num, int baud_rate)
{
    uart_info_t *info = uart_get(uart_num);
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (baud_rate <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    info->baud_rate = baud_rate;
    if (info->driver_installed) {
        return uart_apply_settings(info);
    }
    return ESP_OK;
}

int uart_bridge_get_baudrate(int uart_num)
{
    uart_info_t *info = uart_get(uart_num);
    return (info != NULL) ? info->baud_rate : 0;
}

esp_err_t uart_bridge_set_pins(int uart_num, int tx_gpio, int rx_gpio)
{
    uart_info_t *info = uart_get(uart_num);
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tx_gpio < 0 || rx_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    info->tx_gpio = tx_gpio;
    info->rx_gpio = rx_gpio;
    if (info->driver_installed) {
        return uart_apply_settings(info);
    }
    return ESP_OK;
}

void uart_bridge_get_pins(int uart_num, int *tx_gpio, int *rx_gpio)
{
    uart_info_t *info = uart_get(uart_num);
    if (info == NULL) {
        return;
    }
    if (tx_gpio != NULL) {
        *tx_gpio = info->tx_gpio;
    }
    if (rx_gpio != NULL) {
        *rx_gpio = info->rx_gpio;
    }
}

int uart_bridge_write(int uart_num, const uint8_t *data, size_t len)
{
    uart_info_t *info = uart_get(uart_num);
    if (info == NULL || !info->driver_installed) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    /* The data is written to the transmit buffer of the driver, the function
       must not block: the TCP task would stop reading the client while the
       UART is stuck. Only the data that fits in the buffer is written, the
       rest is dropped. */
    size_t free_size = 0;
    if (uart_get_tx_buffer_free_size(info->uart_num, &free_size) != ESP_OK) {
        return -1;
    }
    if (free_size == 0) {
        return 0;
    }
    if (len > free_size) {
        len = free_size;
    }

    return uart_write_bytes(info->uart_num, data, len);
}

int uart_bridge_read(int uart_num, uint8_t *buf, size_t max_len)
{
    uart_info_t *info = uart_get(uart_num);
    if (info == NULL || !info->driver_installed) {
        return -1;
    }
    if (max_len == 0) {
        return 0;
    }
    /* A timeout of 0 ticks: return the data that is already buffered */
    return uart_read_bytes(info->uart_num, buf, max_len, 0);
}
