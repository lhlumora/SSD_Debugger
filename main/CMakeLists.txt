idf_component_register(SRCS "app_main.c"
                       "app_console.c"
                       "wifi_manager.c"
                       "tcp_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES console
                             esp_wifi
                             esp_netif
                             esp_event
                             nvs_flash
                             lwip)
