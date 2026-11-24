/*
 * Interactive console
 *
 * Commands:
 *   scan                      scan for APs
 *   connect <ssid|index> [pw] connect to an AP
 *   disconnect                disconnect from the AP
 *   wifi_pass [password|clear]  set/clear the default password
 *   status                    show Wi-Fi/IP/TCP server state
 *   server_start [uartN|all]   start the TCP server of one or all the UARTs
 *   server_stop [uartN|all]    stop the TCP server of one or all the UARTs
 *   uart_baud <uartN> <baud>   set the baud rate of a UART
 *   uart_pins <uartN> <tx> <rx> set the GPIO pins of a UART
 *   reboot                     restart the ESP32-S3
 *   help [command]            list the commands / show the help of one command
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_check.h"

#include "argtable3/argtable3.h"
#include "linenoise/linenoise.h"

#include "tcp_server.h"
#include "uart_bridge.h"
#include "wifi_manager.h"
#include "sys_stats.h"
#if CONFIG_EXAMPLE_LCD_ENABLE
#include "lcd_display.h"
#include "st7735.h"
#endif
#if CONFIG_EXAMPLE_LED_GPIO >= 0
#include "led_status.h"
#endif

static const char *TAG = "app_console";

#define CONSOLE_PROMPT "esp32s3>"

/* Result of the last 'scan' command */
static wifi_scan_ap_t s_scan_aps[WIFI_SCAN_MAX_APS];
static size_t s_scan_count;
/* Password used when 'connect' is called without a password argument */
static char s_default_password[64];

/* ------------------------------------------------------------------ */
/* UTF-8 aware column formatting                                        */
/* ------------------------------------------------------------------ */

/* Width of the SSID column, in terminal characters */
#define SSID_COL_WIDTH 32

/* Decode one UTF-8 code point. Returns the number of bytes consumed and
   stores the code point in @p cp. Invalid sequences consume a single byte. */
static int utf8_decode(const char *str, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)str;

    if (u[0] < 0x80) {
        *cp = u[0];
        return 1;
    }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (uint32_t)(u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) |
              ((uint32_t)(u[1] & 0x3F) << 6) |
              (uint32_t)(u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 &&
        (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x07) << 18) |
              ((uint32_t)(u[1] & 0x3F) << 12) |
              ((uint32_t)(u[2] & 0x3F) << 6) |
              (uint32_t)(u[3] & 0x3F);
        return 4;
    }

    *cp = u[0];
    return 1;
}

/* Number of terminal columns used by a code point (approximation of
   wcwidth(): CJK / fullwidth characters take two columns, combining
   marks and control characters take none). */
static int codepoint_width(uint32_t cp)
{
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
        return 0;
    }
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0xFE00 && cp <= 0xFE0F)) {
        return 0;
    }
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x20000 && cp <= 0x2FFFD) ||
        (cp >= 0x30000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

/* Display width of the first @p len bytes of a UTF-8 string, in terminal
   columns */
static int utf8_width_n(const char *str, size_t len)
{
    int width = 0;

    while (len > 0 && *str != '\0') {
        uint32_t cp;
        int used = utf8_decode(str, &cp);
        if ((size_t)used > len) {
            break;
        }
        str += used;
        len -= (size_t)used;
        width += codepoint_width(cp);
    }
    return width;
}

/* Display width of a NUL terminated UTF-8 string, in terminal columns */
static int utf8_display_width(const char *str)
{
    return utf8_width_n(str, strlen(str));
}

/* Print @p str so that it occupies exactly @p width terminal columns.
   Strings that are too long are cut and marked with an ellipsis. */
static void print_column(const char *str, int width)
{
    int used = utf8_display_width(str);

    if (used <= width) {
        fputs(str, stdout);
        for (int i = used; i < width; i++) {
            putchar(' ');
        }
        return;
    }

    if (width < 4) {
        /* Not enough room for the ellipsis */
        fputs(str, stdout);
        return;
    }

    const char *p = str;
    int printed = 0;
    used = 0;
    while (*p != '\0') {
        uint32_t cp;
        int len = utf8_decode(p, &cp);
        int w = codepoint_width(cp);
        if (used + w > width - 3) {
            break;
        }
        used += w;
        printed += len;
        p += len;
    }

    printf("%.*s...", printed, str);
    for (int i = used + 3; i < width; i++) {
        putchar(' ');
    }
}

/* Print @p text at the current position, wrapping the lines so that they do
   not exceed @p width terminal columns. The continuation lines are indented
   with @p col spaces, so that they stay aligned with the first line.
   The text must not contain any newline. */
static void print_wrapped(const char *text, int col, int width)
{
    const char *p = text;
    int cur = col;
    bool first = true;

    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        const char *word = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        size_t len = (size_t)(p - word);
        bool more = (*p != '\0');
        if (more) {
            p++;
        }

        int word_width = utf8_width_n(word, len);

        if (!first) {
            /* The separator is printed only when the word stays on the same
               line, so that no line ends with a space. */
            if (cur + 1 + word_width > width) {
                printf("\n%*s", col, "");
                cur = col;
            } else {
                putchar(' ');
                cur++;
            }
        }
        printf("%.*s", (int)len, word);
        cur += word_width;
        first = false;
    }
}

/* Print a pre-formatted block of text, indenting every line with @p indent
   spaces. The relative indentation of the lines is preserved. */
static void print_block(const char *text, int indent)
{
    const char *line = text;

    while (*line != '\0') {
        const char *eol = strchr(line, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - line) : strlen(line);

        printf("%*s%.*s\n", indent, "", (int)len, line);
        if (eol == NULL) {
            break;
        }
        line = eol + 1;
    }
}

static const char *authmode_str(uint8_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:      return "OPEN";
    case WIFI_AUTH_WEP:       return "WEP";
    case WIFI_AUTH_WPA_PSK:   return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:  return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENT";
    case WIFI_AUTH_WPA3_PSK:  return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
    default:                  return "UNKNOWN";
    }
}

/* Read a password from the console. Note: the characters are echoed by the
   terminal, there is no way to hide them on the ESP-IDF console. */
static bool console_read_password(char *dst, size_t len)
{
    printf("\n");
    char *line = linenoise("Password: ");
    if (line == NULL) {
        dst[0] = '\0';
        return false;
    }
    strlcpy(dst, line, len);
    linenoiseFree(line);
    return true;
}

/* ------------------------------------------------------------------ */
/* scan                                                                */
/* ------------------------------------------------------------------ */
static int cmd_scan(int argc, char **argv)
{
    size_t count = 0;
    esp_err_t err = wifi_manager_scan(s_scan_aps, WIFI_SCAN_MAX_APS, &count);
    if (err != ESP_OK) {
        printf("Scan failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    s_scan_count = count;
    if (count == 0) {
        printf("No AP found.\n");
        return 0;
    }

    printf("\n%3s  ", "Idx");
    print_column("SSID", SSID_COL_WIDTH);
    printf("  %5s  %4s  %s\n", "RSSI", "CH", "AUTH");

    for (size_t i = 0; i < count; i++) {
        printf("%3zu  ", i + 1);
        print_column(s_scan_aps[i].ssid, SSID_COL_WIDTH);
        printf("  %5d  %4u  %s\n", s_scan_aps[i].rssi, s_scan_aps[i].channel,
               authmode_str(s_scan_aps[i].authmode));
    }
    printf("\nUse 'connect <index>' or 'connect <ssid> [password]' to connect.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* connect                                                             */
/* ------------------------------------------------------------------ */
static struct {
    struct arg_str *target;
    struct arg_str *password;
    struct arg_end *end;
} connect_args;

static int cmd_connect(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&connect_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, connect_args.end, argv[0]);
        return 1;
    }

    const char *target = connect_args.target->sval[0];
    char ssid[33] = { 0 };
    uint8_t authmode = WIFI_AUTH_OPEN;
    bool known_ap = false;

    if (target[0] >= '0' && target[0] <= '9') {
        /* numeric argument: index of the last scan result (1 based) */
        long index = strtol(target, NULL, 10);
        if (index < 1 || (size_t)index > s_scan_count) {
            printf("Invalid AP index '%s'. Run 'scan' first.\n", target);
            return 1;
        }
        strlcpy(ssid, s_scan_aps[index - 1].ssid, sizeof(ssid));
        authmode = s_scan_aps[index - 1].authmode;
        known_ap = true;
    } else {
        strlcpy(ssid, target, sizeof(ssid));
        for (size_t i = 0; i < s_scan_count; i++) {
            if (strcmp(s_scan_aps[i].ssid, ssid) == 0) {
                authmode = s_scan_aps[i].authmode;
                known_ap = true;
                break;
            }
        }
    }

    char password[64] = { 0 };
    const char *password_ptr = NULL;

    if (connect_args.password->count > 0) {
        password_ptr = connect_args.password->sval[0];
    } else if (s_default_password[0] != '\0') {
        password_ptr = s_default_password;
    } else if (known_ap && authmode != WIFI_AUTH_OPEN) {
        if (!console_read_password(password, sizeof(password))) {
            printf("Aborted.\n");
            return 1;
        }
        password_ptr = password;
    }

    printf("Connecting to '%s'...\n", ssid);
    esp_err_t err = wifi_manager_connect(ssid, password_ptr, 0);
    if (err != ESP_OK) {
        printf("Connection failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    char ip[16] = { 0 };
    if (wifi_manager_get_ip(ip, sizeof(ip)) == ESP_OK) {
        printf("Connected, IP address: %s\n", ip);
    }
    printf("Use 'server_start uart0' to start the TCP server on port %d.\n",
           tcp_server_port(0));
    return 0;
}

/* ------------------------------------------------------------------ */
/* disconnect / password                                               */
/* ------------------------------------------------------------------ */
static int cmd_disconnect(int argc, char **argv)
{
    esp_err_t err = wifi_manager_disconnect();
    if (err != ESP_OK) {
        printf("Disconnect failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Disconnected.\n");
    return 0;
}

static struct {
    struct arg_str *password;
    struct arg_end *end;
} wifi_pass_args;

static int cmd_wifi_pass(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_pass_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_pass_args.end, argv[0]);
        return 1;
    }

    if (wifi_pass_args.password->count == 0) {
        printf("Default password: %s\n",
               s_default_password[0] ? s_default_password : "(not set)");
        return 0;
    }

    const char *password = wifi_pass_args.password->sval[0];
    if (strcmp(password, "clear") == 0) {
        s_default_password[0] = '\0';
        printf("Default password cleared.\n");
    } else {
        strlcpy(s_default_password, password, sizeof(s_default_password));
        printf("Default password set (%d characters).\n",
               (int)strlen(s_default_password));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

/* Format a number of bytes in a short human readable form */
static void format_bytes(uint32_t bytes, char *buf, size_t len)
{
    if (bytes < 1024) {
        snprintf(buf, len, "%u B", (unsigned)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, len, "%.1f kB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    }
}

static int cmd_status(int argc, char **argv)
{
    char ip[16] = { 0 };
    bool has_ip = (wifi_manager_get_ip(ip, sizeof(ip)) == ESP_OK);

    printf("Wi-Fi     : %s\n", wifi_manager_is_connected() ? "connected" : "not connected");
    if (has_ip) {
        printf("IP address: %s\n", ip);
    }

    /* List the ports of the servers that are running */
    char ports[48] = { 0 };
    int running = 0;
    for (int i = 0; i < UART_BRIDGE_COUNT; i++) {
        if (tcp_server_is_running(i)) {
            char item[16];
            snprintf(item, sizeof(item), "%s%d", (running > 0) ? ", " : "",
                     tcp_server_port(i));
            strncat(ports, item, sizeof(ports) - strlen(ports) - 1);
            running++;
        }
    }
    if (running > 0) {
        printf("TCP server: %d running (port%s %s)\n", running,
               (running > 1) ? "s" : "", ports);
    } else {
        printf("TCP server: stopped\n");
    }

#if CONFIG_EXAMPLE_LED_GPIO >= 0
    printf("LED       : %s\n", led_status_state_name(led_status_get()));
#endif

    printf("\nUART bridges (rx: TCP->UART, tx: UART->TCP):\n");
    for (int i = 0; i < UART_BRIDGE_COUNT; i++) {
        tcp_server_stats_t stats;
        char rx_text[16];
        char tx_text[16];

        tcp_server_get_stats(i, &stats);
        format_bytes(stats.rx_bytes, rx_text, sizeof(rx_text));
        format_bytes(stats.tx_bytes, tx_text, sizeof(tx_text));

        printf("  UART%d  port %-5d %-8s %-10s rx %-9s tx %s\n", i, stats.port,
               stats.running ? "running" : "stopped",
               stats.client ? "connected" : "no client", rx_text, tx_text);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* reboot                                                              */
/* ------------------------------------------------------------------ */

/* Time given to the terminal to receive the last message before the
   restart, in milliseconds */
#define REBOOT_DELAY_MS 500

static int cmd_reboot(int argc, char **argv)
{
    printf("\nRebooting the ESP32-S3...\n");

    /* Close the TCP servers and leave the AP, so that the clients are not
       left waiting for a connection that will never come. */
    tcp_server_stop_all();
    if (wifi_manager_is_connected()) {
        wifi_manager_disconnect();
    }

    /* The message has to reach the terminal before the chip restarts */
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));

    esp_restart();

    /* esp_restart() does not return */
    return 0;
}

/* ------------------------------------------------------------------ */
/* server_start / server_stop                                          */
/* ------------------------------------------------------------------ */

/* Value of the UART selector that means "every UART" */
#define UART_SELECT_ALL (-1)

/* Parse a UART selector: "uart0".."uart2", "0".."2" or "all".
   Returns false when the string is not a valid selector. */
static bool parse_uart_selector(const char *str, int *uart_num)
{
    if (strcmp(str, "all") == 0) {
        *uart_num = UART_SELECT_ALL;
        return true;
    }

    const char *digits = str;
    if (strncmp(str, "uart", 4) == 0) {
        digits = str + 4;
    }
    if (digits[0] < '0' || digits[0] > '9' || digits[1] != '\0') {
        return false;
    }

    int num = digits[0] - '0';
    if (num >= UART_BRIDGE_COUNT) {
        return false;
    }
    *uart_num = num;
    return true;
}

/* Print the list of the valid UART selectors */
static void print_uart_selectors(void)
{
    printf("Valid UARTs: ");
    for (int i = 0; i < UART_BRIDGE_COUNT; i++) {
        printf("%suart%d", (i > 0) ? ", " : "", i);
    }
    printf(", all\n");
}

static struct {
    struct arg_str *uart;
    struct arg_end *end;
} server_start_args;

static struct {
    struct arg_str *uart;
    struct arg_end *end;
} server_stop_args;

static int cmd_server_start(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&server_start_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, server_start_args.end, argv[0]);
        return 1;
    }

    int uart_num = UART_SELECT_ALL;
    if (server_start_args.uart->count > 0) {
        if (!parse_uart_selector(server_start_args.uart->sval[0], &uart_num)) {
            printf("Unknown UART '%s'.\n", server_start_args.uart->sval[0]);
            print_uart_selectors();
            return 1;
        }
    }

    char ip[16] = { 0 };
    bool has_ip = (wifi_manager_get_ip(ip, sizeof(ip)) == ESP_OK);
    int failed = 0;

    for (int i = 0; i < UART_BRIDGE_COUNT; i++) {
        if (uart_num != UART_SELECT_ALL && i != uart_num) {
            continue;
        }
        if (tcp_server_is_running(i)) {
            printf("The TCP server of UART%d is already running on port %d.\n",
                   i, tcp_server_port(i));
            continue;
        }

        esp_err_t err = tcp_server_start(i);
        if (err != ESP_OK) {
            printf("Failed to start the TCP server of UART%d: %s\n", i,
                   esp_err_to_name(err));
            failed++;
            continue;
        }
        if (has_ip) {
            printf("TCP server of UART%d started on port %d (%s).\n", i,
                   tcp_server_port(i), ip);
        } else {
            printf("TCP server of UART%d started on port %d (no IP address yet!).\n",
                   i, tcp_server_port(i));
        }
    }

    return (failed > 0) ? 1 : 0;
}

static int cmd_server_stop(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&server_stop_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, server_stop_args.end, argv[0]);
        return 1;
    }

    int uart_num = UART_SELECT_ALL;
    if (server_stop_args.uart->count > 0) {
        if (!parse_uart_selector(server_stop_args.uart->sval[0], &uart_num)) {
            printf("Unknown UART '%s'.\n", server_stop_args.uart->sval[0]);
            print_uart_selectors();
            return 1;
        }
    }

    int failed = 0;
    for (int i = 0; i < UART_BRIDGE_COUNT; i++) {
        if (uart_num != UART_SELECT_ALL && i != uart_num) {
            continue;
        }
        if (!tcp_server_is_running(i)) {
            printf("The TCP server of UART%d is not running.\n", i);
            continue;
        }

        esp_err_t err = tcp_server_stop(i);
        if (err != ESP_OK) {
            printf("Failed to stop the TCP server of UART%d: %s\n", i,
                   esp_err_to_name(err));
            failed++;
            continue;
        }
        printf("TCP server of UART%d stopped.\n", i);
    }

    return (failed > 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* uart_baud / uart_pins                                               */
/* ------------------------------------------------------------------ */

static struct {
    struct arg_str *uart;
    struct arg_int *baud;
    struct arg_end *end;
} uart_baud_args;

static struct {
    struct arg_str *uart;
    struct arg_int *tx;
    struct arg_int *rx;
    struct arg_end *end;
} uart_pins_args;

static int cmd_uart_baud(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&uart_baud_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, uart_baud_args.end, argv[0]);
        return 1;
    }

    int uart_num = 0;
    if (!parse_uart_selector(uart_baud_args.uart->sval[0], &uart_num) ||
        uart_num == UART_SELECT_ALL) {
        printf("Unknown UART '%s'.\n", uart_baud_args.uart->sval[0]);
        print_uart_selectors();
        return 1;
    }

    int baud = uart_baud_args.baud->ival[0];
    esp_err_t err = uart_bridge_set_baudrate(uart_num, baud);
    if (err != ESP_OK) {
        printf("Failed to set the baud rate of UART%d: %s\n", uart_num,
               esp_err_to_name(err));
        return 1;
    }

    printf("Baud rate of UART%d set to %d.\n", uart_num, baud);
    return 0;
}

static int cmd_uart_pins(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&uart_pins_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, uart_pins_args.end, argv[0]);
        return 1;
    }

    int uart_num = 0;
    if (!parse_uart_selector(uart_pins_args.uart->sval[0], &uart_num) ||
        uart_num == UART_SELECT_ALL) {
        printf("Unknown UART '%s'.\n", uart_pins_args.uart->sval[0]);
        print_uart_selectors();
        return 1;
    }

    int tx = uart_pins_args.tx->ival[0];
    int rx = uart_pins_args.rx->ival[0];
    esp_err_t err = uart_bridge_set_pins(uart_num, tx, rx);
    if (err != ESP_OK) {
        printf("Failed to set the pins of UART%d: %s\n", uart_num,
               esp_err_to_name(err));
        return 1;
    }

    printf("Pins of UART%d set to TX=%d RX=%d.\n", uart_num, tx, rx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* lcd_backlight / lcd_rotate                                          */
/* ------------------------------------------------------------------ */

#if CONFIG_EXAMPLE_LCD_ENABLE

static int cmd_lcd_backlight(int argc, char **argv)
{
    if (!lcd_display_is_ready()) {
        printf("The display is not initialized.\n");
        return 1;
    }

    if (argc < 2) {
        printf("The backlight is %s.\n",
               lcd_display_backlight_is_on() ? "on" : "off");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        lcd_display_set_backlight(true);
    } else if (strcmp(argv[1], "off") == 0) {
        lcd_display_set_backlight(false);
    } else {
        printf("Usage: lcd_backlight [on|off]\n");
        return 1;
    }

    printf("The backlight is %s.\n", lcd_display_backlight_is_on() ? "on" : "off");
    return 0;
}

static int cmd_lcd_rotate(int argc, char **argv)
{
    if (!lcd_display_is_ready()) {
        printf("The display is not initialized.\n");
        return 1;
    }

    if (argc < 2) {
        printf("The screen is rotated by %d degrees.\n", lcd_display_get_rotation());
        return 0;
    }

    const int rotation = atoi(argv[1]);
    esp_err_t err = lcd_display_set_rotation(rotation);
    if (err != ESP_OK) {
        printf("Failed to rotate the screen: %s\n", esp_err_to_name(err));
        return 1;
    }

    int width = 0;
    int height = 0;
    lcd_display_get_size(&width, &height);
    printf("The screen is rotated by %d degrees (%dx%d).\n", rotation, width, height);
    return 0;
}

static int cmd_lcd_info(int argc, char **argv)
{
    if (!lcd_display_is_ready()) {
        printf("The display is not initialized.\n");
        return 1;
    }

    int width = 0;
    int height = 0;
    lcd_display_get_size(&width, &height);

    int col_start = 0;
    int row_start = 0;
    st7735_get_offset(&col_start, &row_start);

    printf("Display: ST7735S %dx%d, rotation %d, backlight %s\n",
           width, height, lcd_display_get_rotation(),
           lcd_display_backlight_is_on() ? "on" : "off");
    printf("SPI: SCLK=%d SDA=%d CS=%d DC=%d RES=%d BL=%d, %d Hz\n",
           CONFIG_EXAMPLE_LCD_SCLK_GPIO, CONFIG_EXAMPLE_LCD_MOSI_GPIO,
           CONFIG_EXAMPLE_LCD_CS_GPIO, CONFIG_EXAMPLE_LCD_DC_GPIO,
           CONFIG_EXAMPLE_LCD_RST_GPIO, CONFIG_EXAMPLE_LCD_BL_GPIO,
           st7735_get_clock());
    printf("Colors: %s order, MADCTL 0x%02X, visible area at %d/%d\n",
           (st7735_get_madctl() & 0x08) ? "BGR" : "RGB",
           st7735_get_madctl(), col_start, row_start);
    return 0;
}

static int cmd_lcd_test(int argc, char **argv)
{
    if (!lcd_display_is_ready()) {
        printf("The display is not initialized.\n");
        return 1;
    }

    if (argc < 2) {
        printf("Usage: lcd_test [raw|map|pattern|red|green|blue|white|black]\n");
        return 1;
    }

    /* The test patterns are drawn without LVGL, they show if the panel is
       driven correctly. */
    if (strcmp(argv[1], "raw") == 0) {
        /* The whole memory of the controller is filled, the visible area of
           the module shows it whatever its offset is */
        if (st7735_fill_raw(0x07E0) != ESP_OK) {
            printf("Failed to fill the memory of the controller.\n");
            return 1;
        }
        printf("The memory of the controller is filled with green.\n");
        return 0;
    }

    if (strcmp(argv[1], "map") == 0) {
        /* The pattern shows how the screen is mapped: a red block in the top
           left corner, a blue block in the bottom right corner, a green line
           in the middle of the height and an orange line in the middle of
           the width */
        if (st7735_draw_map_pattern() != ESP_OK) {
            printf("Failed to draw the mapping pattern.\n");
            return 1;
        }
        printf("Mapping pattern drawn.\n");
        return 0;
    }

    if (strcmp(argv[1], "pattern") == 0) {
        if (st7735_draw_test_pattern() != ESP_OK) {
            printf("Failed to draw the test pattern.\n");
            return 1;
        }
        printf("Test pattern drawn.\n");
        return 0;
    }

    uint16_t color;
    if (strcmp(argv[1], "red") == 0) {
        color = 0xF800;
    } else if (strcmp(argv[1], "green") == 0) {
        color = 0x07E0;
    } else if (strcmp(argv[1], "blue") == 0) {
        color = 0x001F;
    } else if (strcmp(argv[1], "white") == 0) {
        color = 0xFFFF;
    } else if (strcmp(argv[1], "black") == 0) {
        color = 0x0000;
    } else if (strcmp(argv[1], "cyan") == 0) {
        color = 0x07FF;
    } else if (strcmp(argv[1], "magenta") == 0) {
        color = 0xF81F;
    } else if (strcmp(argv[1], "yellow") == 0) {
        color = 0xFFE0;
    } else {
        printf("Usage: lcd_test [raw|map|pattern|red|green|blue|cyan|magenta|"
               "yellow|white|black]\n");
        return 1;
    }

    if (st7735_fill(color) != ESP_OK) {
        printf("Failed to fill the screen.\n");
        return 1;
    }
    printf("The screen is filled with %s.\n", argv[1]);
    return 0;
}

static int cmd_lcd_clock(int argc, char **argv)
{
    if (!lcd_display_is_ready()) {
        printf("The display is not initialized.\n");
        return 1;
    }

    if (argc < 2) {
        printf("The SPI clock of the display is %d Hz.\n", st7735_get_clock());
        return 0;
    }

    const int clock = atoi(argv[1]);
    if (st7735_set_clock(clock) != ESP_OK) {
        printf("Failed to set the speed of the SPI bus.\n");
        return 1;
    }

    printf("The SPI clock of the display is %d Hz.\n", st7735_get_clock());
    return 0;
}

static int cmd_lcd_madctl(int argc, char **argv)
{
    if (!lcd_display_is_ready()) {
        printf("The display is not initialized.\n");
        return 1;
    }

    if (argc < 2) {
        printf("The MADCTL register of the controller is 0x%02X.\n",
               st7735_get_madctl());
        return 0;
    }

    const int value = (int)strtol(argv[1], NULL, 0);
    if (value < 0 || value > 0xFF) {
        printf("Usage: lcd_madctl [0x00..0xFF]\n");
        return 1;
    }

    if (st7735_set_madctl((uint8_t)value) != ESP_OK) {
        printf("Failed to set the MADCTL register.\n");
        return 1;
    }
    printf("The MADCTL register of the controller is 0x%02X.\n",
           st7735_get_madctl());
    return 0;
}

static int cmd_lcd_refresh(int argc, char **argv)
{
    if (!lcd_display_is_ready()) {
        printf("The display is not initialized.\n");
        return 1;
    }

    lcd_display_refresh();
    printf("The screen is drawn again.\n");
    return 0;
}

static int cmd_lcd_offset(int argc, char **argv)
{
    if (!lcd_display_is_ready()) {
        printf("The display is not initialized.\n");
        return 1;
    }

    int col = 0;
    int row = 0;
    st7735_get_offset(&col, &row);

    if (argc < 3) {
        printf("The offset of the visible area is %d/%d.\n", col, row);
        return 0;
    }

    const int new_col = atoi(argv[1]);
    const int new_row = atoi(argv[2]);
    if (st7735_set_offset(new_col, new_row) != ESP_OK) {
        printf("Invalid offset.\n");
        return 1;
    }

    printf("The offset of the visible area is %d/%d.\n", new_col, new_row);
    return 0;
}

#endif /* CONFIG_EXAMPLE_LCD_ENABLE */

/* ------------------------------------------------------------------ */
/* help                                                                */
/* ------------------------------------------------------------------ */

/* Target width of the help output, in terminal columns */
#define HELP_WIDTH 80
/* Indentation of a command line in the command list */
#define HELP_INDENT 4

/* Description of a single command, shown by the 'help' command. */
typedef struct {
    const char *syntax;   /* command name followed by its arguments */
    const char *summary;  /* one line description */
    const char *args;     /* description of the arguments, may be NULL */
    const char *note;     /* additional note, may be NULL */
    const char *example;  /* usage examples, may be NULL */
} help_cmd_t;

/* A group of related commands, printed under a common title. */
typedef struct {
    const char *title;     /* title of the group */
    const help_cmd_t *cmds;
    size_t count;
} help_group_t;

static const help_cmd_t s_help_wifi[] = {
    {
        .syntax  = "scan",
        .summary = "Scan for the surrounding access points.",
        .note    = "  The results are numbered from 1, the number of a result can\n"
                   "  be given to 'connect'.",
    },
    {
        .syntax  = "connect <ssid|index> [password]",
        .summary = "Connect to a Wi-Fi access point.",
        .args    = "  <ssid|index>   SSID of the access point, or the index of a\n"
                   "                 'scan' result (counting from 1)\n"
                   "  <password>     Password of the access point. When it is omitted,\n"
                   "                 the default password (see 'wifi_pass') is used,\n"
                   "                 or it is asked for interactively",
        .example = "  connect 1\n"
                   "  connect MyAP mysecret",
    },
    {
        .syntax  = "disconnect",
        .summary = "Disconnect from the current access point.",
    },
    {
        .syntax  = "wifi_pass [password|clear]",
        .summary = "Get, set or clear the default password.",
        .args    = "  <password>     Password of the access point. The value 'clear'\n"
                   "                 removes the stored password. Without argument\n"
                   "                 the current value is shown",
        .example = "  wifi_pass mysecret\n"
                   "  wifi_pass clear",
    },
};

static const help_cmd_t s_help_server[] = {
    {
        .syntax  = "server_start [uartN|all]",
        .summary = "Start the TCP server of a UART.",
        .args    = "  <uartN|all>     UART to start: uart0, uart1 or uart2. Without\n"
                   "                 argument all the UARTs are started.",
        .note    = "  The port of a UART is 3333 + the number of the UART:\n"
                   "  UART0 uses the port 3333, UART1 the port 3334 and UART2\n"
                   "  the port 3335. Only the started servers use their port,\n"
                   "  the other ones stay available for something else.",
        .example = "  server_start uart0\n"
                   "  server_start all",
    },
    {
        .syntax  = "server_stop [uartN|all]",
        .summary = "Stop the TCP server of a UART.",
        .args    = "  <uartN|all>     UART to stop: uart0, uart1 or uart2. Without\n"
                   "                 argument all the UARTs are stopped.",
        .example = "  server_stop uart1\n"
                   "  server_stop all",
    },
    {
        .syntax  = "uart_baud <uartN> <baud>",
        .summary = "Set the baud rate of a UART.",
        .args    = "  <uartN>         UART to configure: uart0, uart1 or uart2\n"
                   "  <baud>          Baud rate in bits per second",
        .example = "  uart_baud uart1 9600",
    },
    {
        .syntax  = "uart_pins <uartN> <tx> <rx>",
        .summary = "Set the GPIO pins of a UART.",
        .args    = "  <uartN>         UART to configure: uart0, uart1 or uart2\n"
                   "  <tx>            GPIO the TX signal is routed to\n"
                   "  <rx>            GPIO the RX signal is routed to",
        .note    = "  The GPIOs 0, 3, 45 and 46 are strapping pins of the\n"
                   "  ESP32-S3, they should not be used by the UARTs.",
        .example = "  uart_pins uart2 10 11",
    },
};

static const help_cmd_t s_help_system[] = {
    {
        .syntax  = "status",
        .summary = "Show the system state and the UART state.",
        .note    = "  The state of the TCP servers, of the clients and the number\n"
                   "  of bytes transferred by each UART are shown too.",
    },
    {
        .syntax  = "tasks",
        .summary = "List the FreeRTOS tasks.",
    },
    {
        .syntax  = "mem",
        .summary = "Show the memory usage.",
    },
    {
        .syntax  = "cpu",
        .summary = "Show the CPU load (1 second measurement).",
        .note    = "  The console is blocked during the one second measurement.",
    },
    {
        .syntax  = "reboot",
        .summary = "Restart the ESP32-S3.",
        .note    = "  The TCP servers are stopped and the AP is left before\n"
                   "  the restart, so that the clients are not left waiting\n"
                   "  for a connection that will never come.",
    },
};

#if CONFIG_EXAMPLE_LCD_ENABLE
static const help_cmd_t s_help_display[] = {
    {
        .syntax  = "lcd_info",
        .summary = "Show the settings of the display.",
    },
    {
        .syntax  = "lcd_backlight [on|off]",
        .summary = "Switch the backlight of the display on or off.",
        .args    = "  <on|off>         State of the backlight. Without argument the\n"
                   "                 current state is shown",
        .example = "  lcd_backlight off",
    },
    {
        .syntax  = "lcd_rotate [degrees]",
        .summary = "Rotate the screen of the display.",
        .args    = "  <degrees>        0, 90, 180 or 270. Without argument the\n"
                   "                 current orientation is shown",
        .example = "  lcd_rotate 90",
    },
    {
        .syntax  = "lcd_test [pattern|color]",
        .summary = "Draw a test pattern on the display.",
        .args    = "  <pattern|color>  'pattern' draws a border and four colored\n"
                   "                 blocks, a color ('red', 'green', 'blue',\n"
                   "                 'cyan', 'magenta', 'yellow', 'white' or\n"
                   "                 'black') fills the screen",
        .note    = "  The test patterns are drawn without LVGL, they show if\n"
                   "  the panel is driven correctly.",
        .example = "  lcd_test pattern\n"
                   "  lcd_test magenta",
    },
    {
        .syntax  = "lcd_clock [hz]",
        .summary = "Show or set the speed of the SPI bus of the display.",
        .args    = "  <hz>             Speed of the SPI bus in Hz. A lower speed\n"
                   "                 can be needed when the module is connected\n"
                   "                 with long wires",
        .example = "  lcd_clock 2000000",
    },
    {
        .syntax  = "lcd_refresh",
        .summary = "Draw the dashboard again over the whole screen.",
        .note    = "  The test patterns are drawn directly on the panel, the\n"
                   "  dashboard is only drawn again where it changed. This\n"
                   "  command draws it over the whole screen.",
    },
    {
        .syntax  = "lcd_madctl [value]",
        .summary = "Show or set the MADCTL register of the controller.",
        .args    = "  <value>          The value of the register, in decimal or\n"
                   "                 in hexadecimal (0xC8)",
        .note    = "  The register drives the orientation and the mirroring\n"
                   "  of the screen, it depends on the module.",
        .example = "  lcd_madctl 0x08",
    },
    {
        .syntax  = "lcd_offset [col row]",
        .summary = "Show or set the offset of the visible area.",
        .args    = "  <col>            Offset of the first column in the memory\n"
                   "  <row>            Offset of the first row in the memory",
        .note    = "  The controller has 132x162 pixels of memory, the visible\n"
                   "  area of the module is smaller and offset in it. The\n"
                   "  offset is 26/1 for the green modules and 24/0 for the\n"
                   "  red ones.",
        .example = "  lcd_offset 24 0",
    },
};
#endif

static const help_cmd_t s_help_general[] = {
    {
        .syntax  = "help [command]",
        .summary = "Show this help, or the help of a command.",
    },
};

static const help_group_t s_help_groups[] = {
    { "Wi-Fi",       s_help_wifi,    sizeof(s_help_wifi) / sizeof(s_help_wifi[0]) },
    { "TCP server", s_help_server,  sizeof(s_help_server) / sizeof(s_help_server[0]) },
    { "System",     s_help_system,  sizeof(s_help_system) / sizeof(s_help_system[0]) },
#if CONFIG_EXAMPLE_LCD_ENABLE
    { "Display",    s_help_display, sizeof(s_help_display) / sizeof(s_help_display[0]) },
#endif
    { "Help",       s_help_general, sizeof(s_help_general) / sizeof(s_help_general[0]) },
};

#define HELP_GROUP_COUNT (sizeof(s_help_groups) / sizeof(s_help_groups[0]))

/* Find the command named @p name, return NULL when there is no such command */
static const help_cmd_t *help_find_command(const char *name)
{
    size_t len = strlen(name);

    for (size_t g = 0; g < HELP_GROUP_COUNT; g++) {
        for (size_t i = 0; i < s_help_groups[g].count; i++) {
            const char *syntax = s_help_groups[g].cmds[i].syntax;
            if (strncmp(syntax, name, len) == 0 &&
                (syntax[len] == '\0' || syntax[len] == ' ')) {
                return &s_help_groups[g].cmds[i];
            }
        }
    }
    return NULL;
}

/* Print the list of the available commands, grouped by topic */
static void help_print_list(void)
{
    int syntax_width = 0;

    /* All the commands are aligned on the same column, so that the
       descriptions can be scanned down the whole list */
    for (size_t g = 0; g < HELP_GROUP_COUNT; g++) {
        for (size_t i = 0; i < s_help_groups[g].count; i++) {
            int width = utf8_display_width(s_help_groups[g].cmds[i].syntax);
            if (width > syntax_width) {
                syntax_width = width;
            }
        }
    }
    syntax_width += 2;     /* gap between the syntax and the description */

    printf("\nAvailable commands ('help <command>' shows the details of one"
           " command):\n");

    for (size_t g = 0; g < HELP_GROUP_COUNT; g++) {
        const help_group_t *group = &s_help_groups[g];

        printf("\n  %s\n", group->title);
        for (size_t i = 0; i < group->count; i++) {
            const help_cmd_t *cmd = &group->cmds[i];

            printf("%*s", HELP_INDENT, "");
            print_column(cmd->syntax, syntax_width);
            print_wrapped(cmd->summary, HELP_INDENT + syntax_width, HELP_WIDTH);
            printf("\n");
        }
    }
    printf("\n");
}

/* Print the detailed help of a single command */
static void help_print_details(const help_cmd_t *cmd)
{
    printf("\n  %s\n", cmd->syntax);

    printf("%*s", HELP_INDENT, "");
    print_wrapped(cmd->summary, HELP_INDENT, HELP_WIDTH);
    printf("\n");

    if (cmd->args != NULL) {
        printf("\n%*sArguments:\n", HELP_INDENT, "");
        print_block(cmd->args, HELP_INDENT);
    }

    if (cmd->note != NULL) {
        printf("\n%*sNote:\n", HELP_INDENT, "");
        print_block(cmd->note, HELP_INDENT);
    }

    if (cmd->example != NULL) {
        printf("\n%*sExample:\n", HELP_INDENT, "");
        print_block(cmd->example, HELP_INDENT);
    }
    printf("\n");
}

static int cmd_help(int argc, char **argv)
{
    if (argc < 2) {
        help_print_list();
        return 0;
    }

    const char *name = argv[1];
    if (name[0] == '-') {
        printf("help: unknown option '%s'.\n", name);
        printf("Usage: help [command]\n");
        return 1;
    }

    const help_cmd_t *cmd = help_find_command(name);
    if (cmd == NULL) {
        printf("help: unknown command '%s'.\n", name);
        printf("Type 'help' to list the available commands.\n");
        return 1;
    }

    help_print_details(cmd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */
static void register_console_commands(void)
{
    const esp_console_cmd_t scan_cmd = {
        .command = "scan",
        .help = "Scan for Wi-Fi APs",
        .hint = NULL,
        .func = &cmd_scan,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scan_cmd));

    connect_args.target = arg_str1(NULL, NULL, "<ssid|index>", "SSID or index of a scanned AP");
    connect_args.password = arg_str0(NULL, NULL, "<password>", "password of the AP");
    connect_args.end = arg_end(2);
    const esp_console_cmd_t connect_cmd = {
        .command = "connect",
        .help = "Connect to a Wi-Fi AP",
        .hint = NULL,
        .func = &cmd_connect,
        .argtable = &connect_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&connect_cmd));

    const esp_console_cmd_t disconnect_cmd = {
        .command = "disconnect",
        .help = "Disconnect from the Wi-Fi AP",
        .hint = NULL,
        .func = &cmd_disconnect,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&disconnect_cmd));

    wifi_pass_args.password = arg_str0(NULL, NULL, "<password>", "password, or 'clear' to remove it");
    wifi_pass_args.end = arg_end(1);
    const esp_console_cmd_t wifi_pass_cmd = {
        .command = "wifi_pass",
        .help = "Set or clear the default Wi-Fi password",
        .hint = NULL,
        .func = &cmd_wifi_pass,
        .argtable = &wifi_pass_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_pass_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "Show Wi-Fi, IP and TCP server state",
        .hint = NULL,
        .func = &cmd_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    server_start_args.uart = arg_str0(NULL, NULL, "<uartN|all>",
                                      "UART to start: uart0, uart1, uart2 or all");
    server_start_args.end = arg_end(1);
    const esp_console_cmd_t server_start_cmd = {
        .command = "server_start",
        .help = "Start the TCP server of one or of all the UARTs",
        .hint = NULL,
        .func = &cmd_server_start,
        .argtable = &server_start_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&server_start_cmd));

    server_stop_args.uart = arg_str0(NULL, NULL, "<uartN|all>",
                                     "UART to stop: uart0, uart1, uart2 or all");
    server_stop_args.end = arg_end(1);
    const esp_console_cmd_t server_stop_cmd = {
        .command = "server_stop",
        .help = "Stop the TCP server of one or of all the UARTs",
        .hint = NULL,
        .func = &cmd_server_stop,
        .argtable = &server_stop_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&server_stop_cmd));

    uart_baud_args.uart = arg_str1(NULL, NULL, "<uartN>",
                                   "UART to configure: uart0, uart1 or uart2");
    uart_baud_args.baud = arg_int1(NULL, NULL, "<baud>",
                                   "baud rate in bits per second");
    uart_baud_args.end = arg_end(2);
    const esp_console_cmd_t uart_baud_cmd = {
        .command = "uart_baud",
        .help = "Set the baud rate of a UART",
        .hint = NULL,
        .func = &cmd_uart_baud,
        .argtable = &uart_baud_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&uart_baud_cmd));

    uart_pins_args.uart = arg_str1(NULL, NULL, "<uartN>",
                                   "UART to configure: uart0, uart1 or uart2");
    uart_pins_args.tx = arg_int1(NULL, NULL, "<tx_gpio>",
                                 "GPIO the TX signal is routed to");
    uart_pins_args.rx = arg_int1(NULL, NULL, "<rx_gpio>",
                                 "GPIO the RX signal is routed to");
    uart_pins_args.end = arg_end(3);
    const esp_console_cmd_t uart_pins_cmd = {
        .command = "uart_pins",
        .help = "Set the GPIO pins of a UART",
        .hint = NULL,
        .func = &cmd_uart_pins,
        .argtable = &uart_pins_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&uart_pins_cmd));

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Restart the ESP32-S3",
        .hint = NULL,
        .func = &cmd_reboot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot_cmd));

    ESP_ERROR_CHECK(sys_stats_register());

#if CONFIG_EXAMPLE_LCD_ENABLE
    const esp_console_cmd_t lcd_backlight_cmd = {
        .command = "lcd_backlight",
        .help = "Switch the backlight of the display on or off",
        .hint = "[on|off]",
        .func = &cmd_lcd_backlight,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_backlight_cmd));

    const esp_console_cmd_t lcd_rotate_cmd = {
        .command = "lcd_rotate",
        .help = "Rotate the screen of the display",
        .hint = "[degrees]",
        .func = &cmd_lcd_rotate,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_rotate_cmd));

    const esp_console_cmd_t lcd_info_cmd = {
        .command = "lcd_info",
        .help = "Show the settings of the display",
        .hint = NULL,
        .func = &cmd_lcd_info,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_info_cmd));

    const esp_console_cmd_t lcd_test_cmd = {
        .command = "lcd_test",
        .help = "Draw a test pattern on the display",
        .hint = "[pattern|red|green|blue|white|black]",
        .func = &cmd_lcd_test,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_test_cmd));

    const esp_console_cmd_t lcd_clock_cmd = {
        .command = "lcd_clock",
        .help = "Show or set the speed of the SPI bus of the display",
        .hint = "[hz]",
        .func = &cmd_lcd_clock,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_clock_cmd));

    const esp_console_cmd_t lcd_refresh_cmd = {
        .command = "lcd_refresh",
        .help = "Draw the dashboard again over the whole screen",
        .hint = NULL,
        .func = &cmd_lcd_refresh,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_refresh_cmd));

    const esp_console_cmd_t lcd_madctl_cmd = {
        .command = "lcd_madctl",
        .help = "Show or set the MADCTL register of the display controller",
        .hint = "[value]",
        .func = &cmd_lcd_madctl,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_madctl_cmd));

    const esp_console_cmd_t lcd_offset_cmd = {
        .command = "lcd_offset",
        .help = "Show or set the offset of the visible area",
        .hint = "[col row]",
        .func = &cmd_lcd_offset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&lcd_offset_cmd));
#endif

    /* This replaces the 'help' command of the ESP-IDF console: the built-in
       one prints every command on several lines, which is hard to read on a
       terminal of 80 columns. */
    const esp_console_cmd_t help_cmd = {
        .command = "help",
        .help = "Show the list of commands, or the details of one command",
        .hint = "[command]",
        .func = &cmd_help,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&help_cmd));
}

esp_err_t app_console_start(void)
{
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = CONSOLE_PROMPT;
    repl_config.max_cmdline_length = 256;
    repl_config.task_stack_size = 6144;
    repl_config.task_priority = 2;

    /* This creates the console, installs the driver of the console
       peripheral (UART / USB-CDC / USB-Serial-JTAG, depending on the
       menuconfig setting) and spawns the REPL task. */
    esp_console_repl_t *repl = NULL;
    ESP_RETURN_ON_ERROR(esp_console_new_repl_stdio(&repl_config, &repl), TAG,
                        "esp_console_new_repl_stdio() failed");

#if CONFIG_EXAMPLE_CONSOLE_FORCE_RAW_MODE
    /* The console probes the terminal with a DSR query (ESC[5n) and falls
       back to "dumb mode" (no line editing, no history) if the answer does
       not come back within 500 ms. On USB Serial/JTAG the round trip is
       easily lost (the boot logs are still being flushed, the monitor may
       not be attached yet), which disables the features even though the
       terminal is perfectly capable. Force the raw mode here, before the
       REPL task starts, so that line editing and history always work. */
    linenoiseSetDumbMode(0);
#endif

    register_console_commands();

    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "esp_console_start_repl() failed");

    return ESP_OK;
}
