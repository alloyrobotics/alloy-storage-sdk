// alloy_wifi.c: quick-start Wi-Fi station with automatic reconnect. See alloy_storage_wifi.h.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "alloy_storage_wifi.h"

static const char *TAG = "alloy_wifi";
static EventGroupHandle_t s_events;
#define GOT_IP BIT0

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(s_events, GOT_IP);
        esp_wifi_connect();  // fails harmlessly while Wi-Fi is stopped
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, GOT_IP);
    }
}

static bool tolerated(esp_err_t err) { return err == ESP_OK || err == ESP_ERR_INVALID_STATE; }

esp_err_t alloy_storage_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 64))
        return ESP_ERR_INVALID_ARG;
    if (s_events) return ESP_ERR_INVALID_STATE;
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_flash_init();  // Wi-Fi keeps calibration data here; never erased by us
    if (err != ESP_OK) ESP_LOGW(TAG, "nvs_flash_init: %s (continuing)", esp_err_to_name(err));
    if (!tolerated(err = esp_netif_init())) return err;
    if (!tolerated(err = esp_event_loop_create_default())) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    if (password) memcpy(wc.sta.password, password, strlen(password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &wc)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;

    EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & GOT_IP) ? ESP_OK : ESP_ERR_TIMEOUT;
}

int alloy_storage_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
