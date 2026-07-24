/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_net.h"

#include "sdkconfig.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_net";

#if CONFIG_BABELBUS_WIFI_ENABLE

#include "esp_wifi.h"

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "Wi-Fi Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "Open http://" IPSTR "/ or http://babelbus.local/", IP2STR(&e->ip_info.ip));
}

esp_err_t wifi_net_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL), TAG, "ip ev");

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, CONFIG_BABELBUS_WIFI_SSID, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, CONFIG_BABELBUS_WIFI_PASSWORD, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wcfg), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect");

    ESP_LOGI(TAG, "Wi-Fi STA connecting to \"%s\" (2.4 GHz / Wi-Fi 6 via C6)", CONFIG_BABELBUS_WIFI_SSID);
    return ESP_OK;
}

#else

esp_err_t wifi_net_init(void)
{
    ESP_LOGI(TAG, "Wi-Fi disabled (enable BABELBUS_WIFI_ENABLE + esp_hosted/esp_wifi_remote)");
    return ESP_OK;
}

#endif
