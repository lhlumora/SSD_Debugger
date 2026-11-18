/*
 * Wi-Fi station manager
 *
 * Only initializes the Wi-Fi driver and the STA netif. Connecting is done
 * on demand from the console, so the user can pick the AP at runtime.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SCAN_MAX_APS 16

typedef struct {
    char    ssid[33];
    int     rssi;
    uint8_t channel;
    uint8_t authmode;
} wifi_scan_ap_t;

/**
 * @brief  Initialize the Wi-Fi driver and create the STA netif (no connection yet)
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief  Scan for APs and copy the result into @p aps (sorted by signal strength)
 *
 * @param aps     destination array
 * @param max_aps size of @p aps
 * @param num_aps number of APs found (may be NULL)
 */
esp_err_t wifi_manager_scan(wifi_scan_ap_t *aps, size_t max_aps, size_t *num_aps);

/**
 * @brief  Connect to the given AP and wait until an IP address is assigned
 *
 * @param ssid       SSID of the AP
 * @param password   password, NULL or "" for open networks
 * @param timeout_ms how long to wait for the IP address
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password, int timeout_ms);

/**
 * @brief  Disconnect from the current AP
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief  Whether an IP address has been assigned to the STA netif
 */
bool wifi_manager_is_connected(void);

/**
 * @brief  Get the IPv4 address of the STA netif as a string
 */
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);

#ifdef __cplusplus
}
#endif
