/*
 * Wi-Fi station manager
 *
 * The Wi-Fi driver is started, but no AP is connected automatically.
 * The user picks an AP at runtime from the console.
 */
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_check.h"

#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_SCAN_DONE_BIT      BIT1

#define WIFI_SCAN_TIMEOUT_MS    10000
#define WIFI_CONNECT_TIMEOUT_MS 15000

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta_netif;

/* Last scan result, kept in RAM so that the console can refer to it by index */
static wifi_ap_record_t s_ap_records[WIFI_SCAN_MAX_APS];
static uint16_t s_ap_count;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected from AP, reason %d", event->reason);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Connected to AP, waiting for IP address...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_wifi_events, WIFI_SCAN_DONE_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_wifi_events != NULL) {
        return ESP_OK;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Create the STA netif (the default event loop and esp_netif_init()
       must have been called before) */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create the STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init() failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    wifi_event_handler, NULL),
                        TAG, "Failed to register the WIFI event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    wifi_event_handler, NULL),
                        TAG, "Failed to register the IP event handler");
    /* Keep the configuration in RAM only, nothing is written to NVS */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage() failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode() failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start() failed");

    ESP_LOGI(TAG, "Wi-Fi started (STA mode), use the console to scan and connect");
    return ESP_OK;
}

/* Insertion sort, strongest signal first */
static void sort_ap_records(void)
{
    for (int i = 1; i < (int)s_ap_count; i++) {
        wifi_ap_record_t key = s_ap_records[i];
        int j = i - 1;
        while (j >= 0 && s_ap_records[j].rssi < key.rssi) {
            s_ap_records[j + 1] = s_ap_records[j];
            j--;
        }
        s_ap_records[j + 1] = key;
    }
}

esp_err_t wifi_manager_scan(wifi_scan_ap_t *aps, size_t max_aps, size_t *num_aps)
{
    if (s_wifi_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(s_wifi_events, WIFI_SCAN_DONE_BIT);

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_config, false), TAG, "esp_wifi_scan_start() failed");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_SCAN_DONE_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_SCAN_TIMEOUT_MS));
    if (!(bits & WIFI_SCAN_DONE_BIT)) {
        ESP_LOGE(TAG, "Scan timed out");
        return ESP_ERR_TIMEOUT;
    }

    uint16_t found = WIFI_SCAN_MAX_APS;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&found, s_ap_records), TAG,
                        "esp_wifi_scan_get_ap_records() failed");
    s_ap_count = found;
    sort_ap_records();

    size_t count = (s_ap_count < max_aps) ? s_ap_count : max_aps;
    for (size_t i = 0; i < count; i++) {
        strlcpy(aps[i].ssid, (const char *)s_ap_records[i].ssid, sizeof(aps[i].ssid));
        aps[i].rssi = s_ap_records[i].rssi;
        aps[i].channel = s_ap_records[i].primary;
        aps[i].authmode = s_ap_records[i].authmode;
    }

    if (num_aps != NULL) {
        *num_aps = count;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password, int timeout_ms)
{
    if (s_wifi_events == NULL || ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password != NULL && password[0] != '\0') {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                        "esp_wifi_set_config() failed");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "esp_wifi_connect() failed");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : WIFI_CONNECT_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Timeout while waiting for the IP address");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_disconnect(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_disconnect(), TAG, "esp_wifi_disconnect() failed");
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    if (s_wifi_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_get_ip(char *ip_str, size_t len)
{
    if (s_sta_netif == NULL || ip_str == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_sta_netif, &ip_info), TAG,
                        "esp_netif_get_ip_info() failed");

    if (ip_info.ip.addr == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}
