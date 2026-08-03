#include "wifi.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
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

// Beacon intervals (~102ms each) slept through under MAX_MODEM power save.
// Higher saves more radio power and adds inbound latency; too high and access
// points give up holding buffered frames, so this stays moderate.
static constexpr uint16_t STA_LISTEN_INTERVAL = 6;

// Reconnect backoff. The previous code retried immediately and only five times,
// which a router reboot outlasts every time, leaving the device offline until
// someone power-cycled it.
static constexpr uint32_t RECONNECT_BASE_MS = 1000;
static constexpr uint32_t RECONNECT_MAX_MS = 60000;

// The fallback access point. Without it, credentials that stop working - a new
// router, a changed password, or the NVS erase that nvs_flash_init() performs
// when it finds no free pages - would leave no way into a sealed device.
static constexpr uint8_t AP_CHANNEL = 1;
static constexpr uint8_t AP_MAX_CONNECTIONS = 2;
static constexpr size_t AP_MIN_WPA2_PASSWORD = 8;

// Overrides ESP-IDF's 192.168.4.1 default. Note that 192.168.1.0/24 is a very
// common home range: if the station ever joins a router using it, both
// interfaces end up on the same subnet and routing becomes ambiguous, so move
// this if that happens.
static constexpr const char* AP_IP = "192.168.1.1";
static constexpr const char* AP_NETMASK = "255.255.255.0";

// Placeholder, so the rescue network is not open by default. This value is
// public in the repository: set your own from the web interface, which stores
// it in NVS and takes precedence over this.
static constexpr const char* DEFAULT_AP_PASSWORD = "neato-setup";

static EventGroupHandle_t s_wifi_events;
static constexpr int CONNECTED_BIT = BIT0;
static constexpr int FAIL_BIT = BIT1;
static int s_retry_count = 0;
static bool s_wifi_initialized = false;
static esp_timer_handle_t s_reconnect_timer = nullptr;
static uint32_t s_reconnect_delay_ms = RECONNECT_BASE_MS;
static bool s_wifi_started = false;
static bool s_ap_active = false;
static esp_netif_t* s_ap_netif = nullptr;
static std::string s_ap_ssid;

static esp_err_t ensure_wifi_initialized();

static std::string build_ap_ssid()
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    char buf[32];
    snprintf(buf, sizeof(buf), "neato-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return buf;
}

static void load_ap_password(char* out, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t stored = len;
        esp_err_t err = nvs_get_str(handle, "ap_pass", out, &stored);
        nvs_close(handle);
        if (err == ESP_OK) return;
    }
    strncpy(out, DEFAULT_AP_PASSWORD, len - 1);
}

// Brings up the rescue network. Runs alongside the station (APSTA) so retries
// continue while it is up.
static esp_err_t start_ap()
{
    if (s_ap_active) return ESP_OK;

    ESP_ERROR_CHECK(ensure_wifi_initialized());
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    // The DHCP server has to be stopped to re-address the interface, and then
    // hands out leases on the new subnet.
    //
    // Deliberately best-effort rather than ESP_ERROR_CHECK: this is the recovery
    // path, and aborting here would turn a cosmetic addressing problem into an
    // unreachable device. An access point on the default address is still a way
    // in; a panicking one is not.
    esp_netif_ip_info_t ip = {};
    ip.ip.addr = esp_ip4addr_aton(AP_IP);
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(AP_NETMASK);

    esp_err_t ip_err = esp_netif_dhcps_stop(s_ap_netif);
    if (ip_err == ESP_OK || ip_err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ip_err = esp_netif_set_ip_info(s_ap_netif, &ip);
        if (ip_err == ESP_OK) {
            ip_err = esp_netif_dhcps_start(s_ap_netif);
            if (ip_err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) ip_err = ESP_OK;
        }
    }
    if (ip_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not set the access point address, using the default: %s",
                 esp_err_to_name(ip_err));
    }

    s_ap_ssid = build_ap_ssid();

    char password[65] = {};
    load_ap_password(password, sizeof(password));
    const bool secured = strlen(password) >= AP_MIN_WPA2_PASSWORD;
    if (!secured) {
        ESP_LOGW(TAG, "AP password shorter than %d characters, starting an open network",
                 static_cast<int>(AP_MIN_WPA2_PASSWORD));
    }

    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char*>(cfg.ap.ssid), s_ap_ssid.c_str(), sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = s_ap_ssid.size();
    strncpy(reinterpret_cast<char*>(cfg.ap.password), password, sizeof(cfg.ap.password) - 1);
    cfg.ap.channel = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_CONNECTIONS;
    cfg.ap.authmode = secured ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }

    s_ap_active = true;

    // Report what the interface actually ended up with, not what was intended,
    // so the log can be trusted when the addressing above did not take.
    esp_netif_ip_info_t actual = {};
    esp_netif_get_ip_info(s_ap_netif, &actual);
    ESP_LOGW(TAG, "Setup access point '%s' (%s) up on " IPSTR,
             s_ap_ssid.c_str(), secured ? "WPA2" : "open", IP2STR(&actual.ip));
    return ESP_OK;
}

// Dropped once the station is back, so the rescue network is only exposed while
// it is actually needed.
static void stop_ap()
{
    if (!s_ap_active) return;

    ESP_LOGI(TAG, "Station connected, shutting down the setup access point");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not leave AP mode: %s", esp_err_to_name(err));
        return;
    }
    s_ap_active = false;
}

static void reconnect_timer_cb(void*)
{
    // Started from here rather than the WiFi event handler so the mode change
    // happens outside the event task. Retries continue regardless.
    if (s_retry_count >= RETRIES_BEFORE_REPORTING_FAILURE) start_ap();

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
        stop_ap();
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

    // MAX_MODEM sleeps through whole listen intervals rather than waking for
    // every DTIM, which suits a mostly idle device. Note that the radio cannot
    // sleep at all while the fallback access point is up, so power save only
    // applies in normal station operation.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
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
    wifi_config.sta.listen_interval = STA_LISTEN_INTERVAL;

    // Keep the access point up if it is running: someone may be re-provisioning
    // through it right now, and dropping it would cut them off mid-request. It
    // is torn down once the station actually connects.
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Cancel any pending backoff attempt so it cannot race these credentials.
    s_retry_count = 0;
    s_reconnect_delay_ms = RECONNECT_BASE_MS;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);

    xEventGroupClearBits(s_wifi_events, CONNECTED_BIT | FAIL_BIT);

    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        // Already running, so just move the station onto the new credentials.
        esp_wifi_disconnect();
        esp_wifi_connect();
    }

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
        ESP_LOGW(TAG, "No WiFi credentials in NVS. Use 'wifi_set <ssid> <password>', "
                      "or join the setup access point and use the web interface.");
        // Nothing to connect to, so the rescue network is the only way in.
        start_ap();
        return ESP_ERR_NOT_FOUND;
    }

    return connect_with(ssid, pass);
}

bool wifi_is_connected()
{
    if (!s_wifi_initialized) return false;
    return xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT;
}

bool wifi_has_network()
{
    return wifi_is_connected() || s_ap_active;
}

bool wifi_ap_active()
{
    return s_ap_active;
}

std::string wifi_ap_ssid()
{
    // Derived from the MAC, so it can be reported before the access point has
    // ever run. That matters: the name has to be written down while the device
    // is still reachable, not discovered once it is not.
    if (s_ap_ssid.empty()) s_ap_ssid = build_ap_ssid();
    return s_ap_ssid;
}

esp_err_t wifi_set_ap_password(const char* password)
{
    const size_t len = strlen(password);
    if (len != 0 && len < AP_MIN_WPA2_PASSWORD) return ESP_ERR_INVALID_ARG;
    if (len >= 64) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ap_pass", password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Access point password updated, effective next time it starts");
    }
    return err;
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

    // Deliberately not esp_wifi_stop(): that would also drop the access point,
    // which is the very interface these credentials may have arrived over.
    return connect_with(ssid, password);
}
