/*
 * Application entry point
 *
 * Two modes are available (menuconfig -> Example Configuration Options ->
 * "Connect and start the server automatically"):
 *
 *  - console mode (default): the Wi-Fi connection and the TCP server are
 *    controlled from the interactive console (see app_console.c)
 *  - automatic mode: the Wi-Fi connection is established automatically and
 *    the TCP server is started right away
 */
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "app_console.h"
#include "led_status.h"
#include "lcd_display.h"
#include "tcp_server.h"
#include "uart_bridge.h"
#include "wifi_manager.h"

#if CONFIG_EXAMPLE_AUTOMATIC_MODE
#include "protocol_examples_common.h"
#endif

static const char *TAG = "app";

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Prepare the UART peripherals. Their drivers are installed later, when
       the server of a UART is started from the console. */
    ESP_ERROR_CHECK(uart_bridge_init());

#if CONFIG_EXAMPLE_LED_GPIO >= 0
    /* The status LED shows the state of the system, it is started first so
       that it already blinks while the rest of the system comes up. */
    ESP_ERROR_CHECK(led_status_init(CONFIG_EXAMPLE_LED_GPIO));
#endif

#if CONFIG_EXAMPLE_LCD_ENABLE
    /* The display shows the test screen of the panel. A failure of the
       display must not stop the application, the error is only logged. */
    if (lcd_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "The display could not be initialized");
    }
#endif

#if CONFIG_EXAMPLE_AUTOMATIC_MODE
    /* Automatic mode: connect to the configured AP and start the servers */
    ESP_ERROR_CHECK(example_connect());
    ESP_ERROR_CHECK(tcp_server_start_all());
#else
    /* The log output and the REPL prompt share the same console port,
       so keep the Wi-Fi driver quiet to keep the prompt readable.
       It also matters for the terminal capability probe done by the
       console (DSR request, 500 ms timeout): the reply cannot get
       through while the boot logs are flooding the serial port. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);

    /* Start the Wi-Fi driver in STA mode. No AP is connected yet, the user
       selects one from the console with the 'scan'/'connect' commands. */
    ESP_ERROR_CHECK(wifi_manager_init());

    /* Start the interactive console */
    ESP_ERROR_CHECK(app_console_start());

    ESP_LOGI(TAG, "Ready. Type 'help' for the list of commands.");
#endif
}
