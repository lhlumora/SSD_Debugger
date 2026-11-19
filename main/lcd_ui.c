/*
 * Dashboard of the system
 *
 * See lcd_ui.h for the description of the module.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "lcd_display.h"
#include "lcd_ui.h"
#include "led_status.h"
#include "sys_stats.h"
#include "tcp_server.h"
#include "uart_bridge.h"
#include "wifi_manager.h"

static const char *TAG = "lcd_ui";

/* Colors of the dashboard */
#define COLOR_BACKGROUND lv_color_hex(0x101820)
#define COLOR_HEADER     lv_color_hex(0x1B2A3A)
#define COLOR_TITLE      lv_color_hex(0xE8F1F8)
#define COLOR_LABEL      lv_color_hex(0x7A8CA0)
#define COLOR_VALUE      lv_color_hex(0xE8F1F8)
#define COLOR_OK         lv_color_hex(0x4CD964)
#define COLOR_WARN       lv_color_hex(0xFF9F0A)
#define COLOR_ERROR      lv_color_hex(0xFF453A)

/* Height of the header of the dashboard */
#define HEADER_HEIGHT 18

/* Height of a line of the dashboard */
#define ROW_HEIGHT 14

/* Number of rows of the dashboard */
#define ROW_COUNT 6

/* The rows of the dashboard */
typedef enum {
    ROW_WIFI = 0,
    ROW_IP,
    ROW_SERVER,
    ROW_CPU,
    ROW_MEMORY,
    ROW_UPTIME,
} row_id_t;

typedef struct {
    lv_obj_t *label;
    lv_obj_t *value;
} ui_row_t;

static lv_obj_t    *s_header;
static lv_obj_t    *s_title;
static lv_obj_t    *s_dot;
static ui_row_t     s_rows[ROW_COUNT];
static lv_timer_t  *s_timer;
static bool         s_created;

/* Format the running time as hh:mm:ss */
static void format_uptime(char *buf, size_t len)
{
    const int64_t seconds = esp_timer_get_time() / 1000000;
    const int hours = (int)(seconds / 3600);
    const int minutes = (int)((seconds % 3600) / 60);
    const int secs = (int)(seconds % 60);
    snprintf(buf, len, "%02d:%02d:%02d", hours, minutes, secs);
}

/* Format a number of bytes as a number of kilobytes */
static void format_memory(char *buf, size_t len)
{
    const uint32_t free_bytes = esp_get_free_heap_size();
    snprintf(buf, len, "%u kB", (unsigned)(free_bytes / 1024));
}

/* The state of the TCP servers, on one line */
static void format_servers(char *buf, size_t len)
{
    char servers[16] = { 0 };
    size_t used = 0;

    for (int uart = 0; uart < UART_BRIDGE_COUNT; uart++) {
        tcp_server_stats_t stats;
        tcp_server_get_stats(uart, &stats);
        if (!stats.running) {
            continue;
        }
        const int written = snprintf(servers + used, sizeof(servers) - used, "%s%d",
                                     used > 0 ? "," : "", uart);
        if (written < 0) {
            break;
        }
        used += written;
        if (used >= sizeof(servers)) {
            break;
        }
    }

    snprintf(buf, len, "%s", used > 0 ? servers : "none");
}

/* Update the content of the dashboard */
static void update_dashboard(lv_timer_t *timer)
{
    char text[32];

    /* The Wi-Fi connection */
    const bool connected = wifi_manager_is_connected();
    lv_label_set_text_fmt(s_rows[ROW_WIFI].value, "%s", connected ? "connected" : "offline");
    lv_obj_set_style_text_color(s_rows[ROW_WIFI].value,
                                connected ? COLOR_OK : COLOR_WARN, LV_PART_MAIN);

    /* The IP address */
    char ip[16] = { 0 };
    if (connected) {
        wifi_manager_get_ip(ip, sizeof(ip));
    }
    lv_label_set_text_fmt(s_rows[ROW_IP].value, "%s", ip[0] != '\0' ? ip : "-");

    /* The TCP servers */
    format_servers(text, sizeof(text));
    lv_label_set_text_fmt(s_rows[ROW_SERVER].value, "%s", text);

    /* The CPU load */
    lv_label_set_text_fmt(s_rows[ROW_CPU].value, "%u%%",
                          (unsigned)sys_stats_get_cpu_load());

    /* The free memory */
    format_memory(text, sizeof(text));
    lv_label_set_text_fmt(s_rows[ROW_MEMORY].value, "%s", text);

    /* The running time */
    format_uptime(text, sizeof(text));
    lv_label_set_text_fmt(s_rows[ROW_UPTIME].value, "%s", text);

    /* The dot shows the state of the status LED */
    led_state_t led_state = led_status_get();
    lv_color_t color;
    switch (led_state) {
    case LED_STATE_BOOTING:
    case LED_STATE_WIFI_DOWN:
        color = COLOR_WARN;
        break;
    case LED_STATE_ERROR:
        color = COLOR_ERROR;
        break;
    default:
        color = COLOR_OK;
        break;
    }
    lv_obj_set_style_bg_color(s_dot, color, LV_PART_MAIN);
}

/* The label and the value of a row */
static void create_row(lv_obj_t *parent, row_id_t id, const char *label, int y)
{
    s_rows[id].label = lv_label_create(parent);
    lv_label_set_text(s_rows[id].label, label);
    lv_obj_set_style_text_color(s_rows[id].label, COLOR_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_rows[id].label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(s_rows[id].label, 4, y);
    lv_obj_set_width(s_rows[id].label, 34);
    lv_label_set_long_mode(s_rows[id].label, LV_LABEL_LONG_CLIP);

    s_rows[id].value = lv_label_create(parent);
    lv_label_set_text(s_rows[id].value, "-");
    lv_obj_set_style_text_color(s_rows[id].value, COLOR_VALUE, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_rows[id].value, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(s_rows[id].value, 40, y);
    lv_obj_set_width(s_rows[id].value, 38);
    lv_label_set_long_mode(s_rows[id].value, LV_LABEL_LONG_DOT);
}

esp_err_t lcd_ui_create(void)
{
    if (s_created) {
        return ESP_OK;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, COLOR_BACKGROUND, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scroll_dir(screen, LV_DIR_NONE);

    /* The header shows the name of the application and the state of the LED */
    s_header = lv_obj_create(screen);
    lv_obj_set_size(s_header, LV_PCT(100), HEADER_HEIGHT);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_style_bg_color(s_header, COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_header, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_header, LV_DIR_NONE);

    s_title = lv_label_create(s_header);
    lv_label_set_text(s_title, "SSD Debugger");
    lv_obj_set_style_text_color(s_title, COLOR_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(s_title, 4, 2);

    /* The dot shows the state of the status LED */
    const int screen_width = lv_display_get_horizontal_resolution(lv_display_get_default());
    s_dot = lv_obj_create(s_header);
    lv_obj_set_size(s_dot, 8, 8);
    lv_obj_set_pos(s_dot, screen_width - 14, 5);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dot, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    /* The rows of the dashboard */
    int y = HEADER_HEIGHT + 4;
    create_row(screen, ROW_WIFI, "WiFi", y);
    y += ROW_HEIGHT;
    create_row(screen, ROW_IP, "IP", y);
    y += ROW_HEIGHT;
    create_row(screen, ROW_SERVER, "TCP", y);
    y += ROW_HEIGHT;
    create_row(screen, ROW_CPU, "CPU", y);
    y += ROW_HEIGHT;
    create_row(screen, ROW_MEMORY, "MEM", y);
    y += ROW_HEIGHT;
    create_row(screen, ROW_UPTIME, "UP", y);

    /* The timer that refreshes the content of the dashboard */
    s_timer = lv_timer_create(update_dashboard, 1000, NULL);
    lv_timer_ready(s_timer);

    s_created = true;
    ESP_LOGI(TAG, "Dashboard created");
    return ESP_OK;
}

void lcd_ui_recreate(void)
{
    if (!s_created) {
        return;
    }

    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    /* The screen of LVGL is emptied and the dashboard is built again with the
       new size of the screen */
    lv_obj_clean(lv_screen_active());
    s_created = false;

    lcd_ui_create();
}
