/*
 * Interactive console
 *
 * Commands:
 *   scan                      scan for APs
 *   connect <ssid|index> [pw] connect to an AP
 *   disconnect                disconnect from the AP
 *   wifi_pass [password|clear]  set/clear the default password
 *   status                    show Wi-Fi/IP/TCP server state
 *   server_start              start the TCP server
 *   server_stop               stop the TCP server
 *   help                      built-in help
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_check.h"

#include "argtable3/argtable3.h"
#include "linenoise/linenoise.h"

#include "tcp_server.h"
#include "wifi_manager.h"

static const char *TAG = "app_console";

#define CONSOLE_PROMPT "esp32s3> "

/* Result of the last 'scan' command */
static wifi_scan_ap_t s_scan_aps[WIFI_SCAN_MAX_APS];
static size_t s_scan_count;
/* Password used when 'connect' is called without a password argument */
static char s_default_password[64];

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

    printf("\n%3s  %-32s  %5s  %4s  %s\n", "Idx", "SSID", "RSSI", "CH", "AUTH");
    for (size_t i = 0; i < count; i++) {
        printf("%3zu  %-32s  %5d  %4u  %s\n", i + 1, s_scan_aps[i].ssid,
               s_scan_aps[i].rssi, s_scan_aps[i].channel,
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
    printf("Use 'server_start' to start the TCP server on port %d.\n", tcp_server_port());
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
static int cmd_status(int argc, char **argv)
{
    char ip[16] = { 0 };
    bool has_ip = (wifi_manager_get_ip(ip, sizeof(ip)) == ESP_OK);

    printf("Wi-Fi     : %s\n", wifi_manager_is_connected() ? "connected" : "not connected");
    if (has_ip) {
        printf("IP address: %s\n", ip);
    }
    printf("TCP server: %s (port %d)\n",
           tcp_server_is_running() ? "running" : "stopped",
           tcp_server_port());
    return 0;
}

/* ------------------------------------------------------------------ */
/* server_start / server_stop                                          */
/* ------------------------------------------------------------------ */
static int cmd_server_start(int argc, char **argv)
{
    if (tcp_server_is_running()) {
        printf("TCP server is already running on port %d.\n", tcp_server_port());
        return 0;
    }

    char ip[16] = { 0 };
    if (wifi_manager_get_ip(ip, sizeof(ip)) == ESP_OK) {
        printf("Starting TCP server on port %d (%s)\n", tcp_server_port(), ip);
    } else {
        printf("Starting TCP server on port %d (no IP address yet!)\n", tcp_server_port());
    }

    esp_err_t err = tcp_server_start();
    if (err != ESP_OK) {
        printf("Failed to start the TCP server: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("TCP server started.\n");
    return 0;
}

static int cmd_server_stop(int argc, char **argv)
{
    if (!tcp_server_is_running()) {
        printf("TCP server is not running.\n");
        return 0;
    }

    esp_err_t err = tcp_server_stop();
    if (err != ESP_OK) {
        printf("Failed to stop the TCP server: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("TCP server stopped.\n");
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

    const esp_console_cmd_t server_start_cmd = {
        .command = "server_start",
        .help = "Start the TCP server",
        .hint = NULL,
        .func = &cmd_server_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&server_start_cmd));

    const esp_console_cmd_t server_stop_cmd = {
        .command = "server_stop",
        .help = "Stop the TCP server",
        .hint = NULL,
        .func = &cmd_server_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&server_stop_cmd));
}

esp_err_t app_console_start(void)
{
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = CONSOLE_PROMPT;
    repl_config.max_cmdline_length = 256;
    repl_config.task_stack_size = 4096;
    repl_config.task_priority = 2;

    /* This creates the console, installs the driver of the console
       peripheral (UART / USB-CDC / USB-Serial-JTAG, depending on the
       menuconfig setting) and spawns the REPL task. */
    esp_console_repl_t *repl = NULL;
    ESP_RETURN_ON_ERROR(esp_console_new_repl_stdio(&repl_config, &repl), TAG,
                        "esp_console_new_repl_stdio() failed");

    register_console_commands();

    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "esp_console_start_repl() failed");

    printf("\n"
           "TCP server console\n"
           "  scan                       list the available APs\n"
           "  connect <index|ssid> [pw]  connect to an AP\n"
           "  disconnect                 leave the AP\n"
           "  wifi_pass [pw|clear]       set/clear the default password\n"
           "  status                     show the current state\n"
           "  server_start               start the TCP server\n"
           "  server_stop                stop the TCP server\n"
           "  help                       list all commands\n\n");

    return ESP_OK;
}
