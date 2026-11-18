idf_component_register(SRCS "sys_stats.c" "led_status.c" "app_main.c"
                       "app_console.c"
                       "wifi_manager.c"
                       "tcp_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES console
                             esp_wifi
                             esp_netif
                             esp_event
                             esp_driver_gpio
                             esp_system
                             esp_timer
                             heap
                             nvs_flash
                             lwip)
