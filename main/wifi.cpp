#include "wifi.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static constexpr const char* TAG = "wifi";
static constexpr const char* NVS_NAMESPACE = "wifi_creds";

// Attempts before wifi_init_sta() reports failure to its caller. Retrying does
// not stop there: this device is headless, so giving up permanently would mean
// it could only be recovered by physically reflashing it.
static constexpr int RETRIES_BEFORE_REPORTING_FAILURE = 5;

// Reconnect backoff. The previous code retried immediately and only five times,
// which a router reboot outlasts every time, leaving the device offline until
// someone power-cycled it.
static constexpr uint32_t RECONNECT_BASE_MS = 1000;
static constexpr uint32_t RECONNECT_MAX_MS = 60000;

static EventGroupHandle_t s_wifi_events;
static constexpr int CONNECTED_BIT = BIT0;
static constexpr int FAIL_BIT = BIT1;
static int s_retry_count = 0;
static bool s_wifi_initialized = false;
static esp_timer_handle_t s_reconnect_timer = nullptr;
static uint32_t s_reconnect_delay_ms = RECONNECT_BASE_MS;

static void reconnect_timer_cb(void*)
{
    esp_wifi_connect();
}

static void schedule_reconnect()
{
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback = reconnect_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_reconnect",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_reconnect_timer));
    }

    esp_timer_stop(s_reconnect_timer); // harmless if it is not running
    ESP_ERROR_CHECK(esp_timer_start_once(s_reconnect_timer,
                                         static_cast<uint64_t>(s_reconnect_delay_ms) * 1000));

    ESP_LOGI(TAG, "Next connection attempt in %" PRIu32 " ms", s_reconnect_delay_ms);
    s_reconnect_delay_ms = std::min(s_reconnect_delay_ms * 2, RECONNECT_MAX_MS);
}

static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // This bit was never cleared here, so wifi_is_connected() kept
        // reporting a live link after the connection had dropped.
        xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);

        s_retry_count++;
        if (s_retry_count == RETRIES_BEFORE_REPORTING_FAILURE) {
            ESP_LOGW(TAG, "Still not connected after %d attempts, continuing to retry",
                     RETRIES_BEFORE_REPORTING_FAILURE);
            xEventGroupSetBits(s_wifi_events, FAIL_BIT);
        }
        schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "Connected - IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_reconnect_delay_ms = RECONNECT_BASE_MS;
        xEventGroupClearBits(s_wifi_events, FAIL_BIT);
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

    // Cancel any pending backoff attempt so it cannot race these credentials.
    s_retry_count = 0;
    s_reconnect_delay_ms = RECONNECT_BASE_MS;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);

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

bool wifi_is_connected()
{
    if (!s_wifi_initialized) return false;
    return xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT;
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
