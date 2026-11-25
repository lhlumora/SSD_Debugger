idf_component_register(SRCS "sys_stats.c" "led_status.c" "app_main.c"
                       "app_console.c"
                       "wifi_manager.c"
                       "tcp_server.c"
                       "uart_bridge.c"
                       "tft_panel.c"
                       "lcd_display.c"
                       "lcd_ui.c"
                    INCLUDE_DIRS "."
                    REQUIRES console
                             esp_wifi
                             esp_netif
                             esp_event
                             esp_driver_gpio
                             esp_driver_spi
                             esp_driver_uart
                             esp_system
                             esp_timer
                             heap
                             lvgl__lvgl
                             nvs_flash
                             lwip)

# The LVGL library has to find the lv_conf.h of the project, the file is in
# the directory of this component.
target_include_directories(__idf_lvgl__lvgl PUBLIC ${CMAKE_CURRENT_LIST_DIR})
