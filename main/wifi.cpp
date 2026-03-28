#include "wifi.h"

#include <cstring>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static constexpr const char* TAG = "wifi";
static constexpr const char* NVS_NAMESPACE = "wifi_creds";
static constexpr int MAX_RETRIES = 5;

static EventGroupHandle_t s_wifi_events;
static constexpr int CONNECTED_BIT = BIT0;
static constexpr int FAIL_BIT = BIT1;
static int s_retry_count = 0;
static bool s_wifi_initialized = false;

static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < MAX_RETRIES) {
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying connection (%d/%d)...", s_retry_count, MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Failed to connect after %d attempts", MAX_RETRIES);
            xEventGroupSetBits(s_wifi_events, FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "Connected - IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
    }
}

static esp_err_t load_credentials(char* ssid, size_t ssid_len, char* pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;

    err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "pass", pass, &pass_len);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t ensure_wifi_initialized()
{
    if (s_wifi_initialized) return ESP_OK;

    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr));

    s_wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t connect_with(const char* ssid, const char* password)
{
    ESP_ERROR_CHECK(ensure_wifi_initialized());

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_events, CONNECTED_BIT | FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, CONNECTED_BIT | FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & CONNECTED_BIT) return ESP_OK;
    if (bits & FAIL_BIT) return ESP_FAIL;
    ESP_LOGW(TAG, "Connection timed out");
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_init_sta()
{
    char ssid[33] = {};
    char pass[65] = {};

    esp_err_t err = load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No WiFi credentials in NVS. Use 'wifi_set <ssid> <password>' to configure.");
        return ESP_ERR_NOT_FOUND;
    }

    return connect_with(ssid, pass);
}

esp_err_t wifi_set_credentials(const char* ssid, const char* password)
{
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, "pass", password));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    ESP_LOGI(TAG, "Credentials saved for '%s'", ssid);

    // Stop existing connection if any, then reconnect
    if (s_wifi_initialized) {
        esp_wifi_stop();
    }
    return connect_with(ssid, password);
}
