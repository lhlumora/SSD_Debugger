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
#include "tcp_server.h"
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

#if CONFIG_EXAMPLE_AUTOMATIC_MODE
    /* Automatic mode: connect to the configured AP and start the server */
    ESP_ERROR_CHECK(example_connect());
    ESP_ERROR_CHECK(tcp_server_start());
#else
    /* The log output and the REPL prompt share the same console port,
       so keep the Wi-Fi driver quiet to keep the prompt readable. */
    esp_log_level_set("wifi", ESP_LOG_WARN);

    /* Start the Wi-Fi driver in STA mode. No AP is connected yet, the user
       selects one from the console with the 'scan'/'connect' commands. */
    ESP_ERROR_CHECK(wifi_manager_init());

    /* Start the interactive console */
    ESP_ERROR_CHECK(app_console_start());

    ESP_LOGI(TAG, "Ready. Type 'help' for the list of commands.");
#endif
}
