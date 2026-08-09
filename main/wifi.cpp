#include "wifi.h"
#include "captive_dns.h"

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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "dhcpserver/dhcpserver.h"
#include "nvs_flash.h"

static constexpr const char* TAG = "wifi";
static constexpr const char* NVS_NAMESPACE = "wifi_creds";

// Attempts before wifi_init_sta() reports failure to its caller. Retrying does
// not stop there: this device is headless, so giving up permanently would mean
// it could only be recovered by physically reflashing it.
static constexpr int RETRIES_BEFORE_REPORTING_FAILURE = 5;

// How long connect_with() waits for a result before reporting a timeout. Only
// the boot path waits on this now; a connection asked for over the network is
// applied on a worker task and its outcome read back from wifi_state().
static constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;

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

// Overrides ESP-IDF's 192.168.4.1 default, and deliberately picks a range
// nothing else is likely to be sitting in.
//
// Two separate reasons, both learned the hard way. A common home range means
// the station may one day join a router using it, putting both interfaces on
// the same subnet and making routing ambiguous. Worse, a browser remembers what
// it has met at an address: one that had opened a router's admin page at
// 192.168.1.1 had cached that router's redirect to its own HTTPS interface, and
// from then on sent every request for the address to port 443 without
// consulting the network. Nothing listens there, so the browser reported a
// refused connection - while curl and wget, keeping no such cache, fetched the
// page from port 80 quite happily, and nothing appeared in this device's log
// because the request never arrived.
//
// 192.168.255.0/24 is valid, routable and effectively unused by consumer gear,
// so it carries none of that history.
//
// Nothing outside this file should hard-code the value. The captive DNS answers
// every name with whatever the interface actually ended up with, so the way in
// is the name below, and the numeric address is reported by the device itself.
static constexpr const char* AP_IP = "192.168.255.1";
static constexpr const char* AP_NETMASK = "255.255.255.0";

// Bootstrap value only. It is published in this repository and the SSID is
// derived from the MAC, so a device still using it is joinable by anyone in
// range. First-run setup will not complete until it has been replaced, and
// wifi_ap_password_is_default() reports it for as long as it is in use.
static constexpr const char* DEFAULT_AP_PASSWORD = "neato-setup";

static EventGroupHandle_t s_wifi_events;
static constexpr int CONNECTED_BIT = BIT0;
static constexpr int FAIL_BIT = BIT1;
// Tracked separately from CONNECTED_BIT so association and DHCP can be told
// apart: associating fine but never getting a lease is a different fault.
static constexpr int ASSOCIATED_BIT = BIT2;
static bool s_have_credentials = false;
static int s_retry_count = 0;
static bool s_wifi_initialized = false;
static esp_timer_handle_t s_reconnect_timer = nullptr;
static uint32_t s_reconnect_delay_ms = RECONNECT_BASE_MS;
static bool s_wifi_started = false;
static bool s_ap_active = false;
// Set when the access point was requested deliberately rather than as a
// fallback, so a station reconnect does not silently take it away again.
static bool s_ap_forced = false;
static esp_netif_t* s_ap_netif = nullptr;
static std::string s_ap_ssid;
static bool s_ap_password_default = true;

// Guards every one of the above and every radio state transition. They are
// written from three tasks - the event loop, the reconnect timer, and whichever
// task is applying new credentials - and were previously unsynchronised. The
// concrete failure: the backoff timer raises the access point (stopping the
// radio) while a re-provisioning request is halfway through reconfiguring the
// station, and one of them then drives the driver through a transition it
// rejects. Recursive because the public entry points call the internal ones.
static SemaphoreHandle_t s_state_mutex = xSemaphoreCreateRecursiveMutex();

namespace {

class WifiLock {
public:
    WifiLock() { if (s_state_mutex) xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY); }
    ~WifiLock() { if (s_state_mutex) xSemaphoreGiveRecursive(s_state_mutex); }
    WifiLock(const WifiLock&) = delete;
    WifiLock& operator=(const WifiLock&) = delete;
};

// Credentials waiting to be applied, and the task that applies them. One task
// and one slot: repeated requests overwrite the pending value rather than
// queueing, because only the most recent one is of any interest.
TaskHandle_t s_apply_task = nullptr;
char s_pending_ssid[WIFI_MAX_SSID + 1] = {};
char s_pending_pass[WIFI_MAX_PASSWORD + 1] = {};

void copy_bounded(char* dst, size_t dst_size, const char* src)
{
    const size_t len = std::min(strlen(src), dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

}  // namespace

static esp_err_t ensure_wifi_initialized();

static std::string build_ap_ssid()
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    char buf[32];
    snprintf(buf, sizeof(buf), "neato-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return buf;
}

// Fills `out` with the stored passphrase, or the bootstrap default when there
// is none. Never returns something too short for WPA2: the access point is the
// recovery interface, and an open one would let anyone in range reach it.
static void load_ap_password(char* out, size_t len)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t stored = len;
        esp_err_t err = nvs_get_str(handle, "ap_pass", out, &stored);
        nvs_close(handle);
        if (err == ESP_OK && strlen(out) >= WIFI_MIN_AP_PASSWORD) {
            WifiLock lock;
            s_ap_password_default = (strcmp(out, DEFAULT_AP_PASSWORD) == 0);
            return;
        }
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Stored access point password is too short for WPA2, using the default");
        }
    }

    copy_bounded(out, len, DEFAULT_AP_PASSWORD);
    WifiLock lock;
    s_ap_password_default = true;
}

// Brings the rescue network up. Runs alongside the station (APSTA) so retries
// continue while it is up.
//
// Deliberately not ESP_ERROR_CHECK, anywhere: this is the recovery path, and it
// runs on the reconnect timer while the station is already failing. Aborting
// here turns "the access point did not come up" into a reboot loop on a device
// whose only other way in is a pin inside a sealed shell.
static esp_err_t start_ap()
{
    WifiLock lock;

    if (s_ap_active) return ESP_OK;

    esp_err_t err = ensure_wifi_initialized();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start the access point: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    s_ap_ssid = build_ap_ssid();

    char password[WIFI_MAX_PASSWORD + 1] = {};
    load_ap_password(password, sizeof(password));

    wifi_config_t cfg = {};
    copy_bounded(reinterpret_cast<char*>(cfg.ap.ssid), sizeof(cfg.ap.ssid), s_ap_ssid.c_str());
    cfg.ap.ssid_len = s_ap_ssid.size();
    copy_bounded(reinterpret_cast<char*>(cfg.ap.password), sizeof(cfg.ap.password), password);
    cfg.ap.channel = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_CONNECTIONS;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    // Order matters, and getting it wrong is silent. Switching the mode while
    // the radio is running brings the AP interface up immediately, using the
    // config it already holds - which is empty. Setting the SSID afterwards does
    // not restart the beacon, so it advertises a blank network: every call
    // returns OK and nothing is visible to scan. Stop the radio first so the
    // mode and the config are both in place before the interface comes up.
    if (s_wifi_started) {
        err = esp_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not stop the radio: %s", esp_err_to_name(err));
        }
        s_wifi_started = false;
    }

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not bring the access point up: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_started = true;

    // Addressed after starting, because stopping the radio takes the AP netif
    // and its DHCP server down with it. Best-effort for the same reason as the
    // rest of this function: an access point on the default address is still a
    // way in; a panicking one is not.
    esp_netif_ip_info_t ip = {};
    ip.ip.addr = esp_ip4addr_aton(AP_IP);
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton(AP_NETMASK);

    esp_err_t ip_err = esp_netif_dhcps_stop(s_ap_netif);
    if (ip_err == ESP_OK || ip_err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ip_err = esp_netif_set_ip_info(s_ap_netif, &ip);

        // Hand out this device as the DNS server, which the lease does not do
        // by default. Without it a joining client has no resolver at all, so it
        // decides the network is broken and leaves - taking the person trying
        // to re-provision the robot with it. Set while the server is stopped,
        // because that is the only time the lease template can be changed.
        if (ip_err == ESP_OK) {
            esp_netif_dns_info_t dns = {};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = ip.ip.addr;
            esp_err_t dns_err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);

            uint8_t offer_dns = OFFER_DNS;
            if (dns_err == ESP_OK) {
                dns_err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                                 &offer_dns, sizeof(offer_dns));
            }
            if (dns_err != ESP_OK) {
                ESP_LOGW(TAG, "Could not advertise a DNS server: %s", esp_err_to_name(dns_err));
            }
        }

        if (ip_err == ESP_OK) {
            ip_err = esp_netif_dhcps_start(s_ap_netif);
            if (ip_err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) ip_err = ESP_OK;
        }
    }
    if (ip_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not set the access point address, using the default: %s",
                 esp_err_to_name(ip_err));
    }

    s_ap_active = true;

    // Best-effort: without it the setup page still works, it just has to be
    // typed in by hand rather than opening on its own.
    esp_netif_ip_info_t serving = {};
    esp_netif_get_ip_info(s_ap_netif, &serving);
    captive_dns_start(serving.ip.addr);

    // Report what the interface actually ended up with, not what was intended,
    // so the log can be trusted when the addressing above did not take.
    esp_netif_ip_info_t actual = {};
    esp_netif_get_ip_info(s_ap_netif, &actual);
    ESP_LOGW(TAG, "Setup access point '%s' up: http://%s (" IPSTR ")",
             s_ap_ssid.c_str(), WIFI_SETUP_HOSTNAME, IP2STR(&actual.ip));
    if (s_ap_password_default) {
        ESP_LOGW(TAG, "Access point is using the built-in default passphrase. "
                      "Set your own from the web interface.");
    }
    return ESP_OK;
}

// Dropped once the station is back, so the rescue network is only exposed while
// it is actually needed.
static void stop_ap()
{
    WifiLock lock;

    if (!s_ap_active) return;
    if (s_ap_forced) {
        ESP_LOGI(TAG, "Leaving the access point up, it was started on request");
        return;
    }

    ESP_LOGI(TAG, "Station connected, shutting down the setup access point");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not leave AP mode: %s", esp_err_to_name(err));
        return;
    }
    // Stopped with the network it belongs to: answering every name with this
    // device's address is right for a recovery network and wrong anywhere else.
    captive_dns_stop();
    s_ap_active = false;
}

static void reconnect_timer_cb(void*)
{
    // Started from here rather than the WiFi event handler so the mode change
    // happens outside the event task. Retries continue regardless.
    {
        WifiLock lock;
        if (s_retry_count >= RETRIES_BEFORE_REPORTING_FAILURE) start_ap();
    }

    esp_wifi_connect();
}

static void schedule_reconnect()
{
    WifiLock lock;

    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback = reconnect_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_reconnect",
            .skip_unhandled_events = true,
        };
        esp_err_t err = esp_timer_create(&args, &s_reconnect_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not create the reconnect timer: %s", esp_err_to_name(err));
            return;
        }
    }

    esp_timer_stop(s_reconnect_timer); // harmless if it is not running
    esp_err_t err = esp_timer_start_once(s_reconnect_timer,
                                         static_cast<uint64_t>(s_reconnect_delay_ms) * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not schedule a reconnect: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Next connection attempt in %" PRIu32 " ms", s_reconnect_delay_ms);
    s_reconnect_delay_ms = std::min(s_reconnect_delay_ms * 2, RECONNECT_MAX_MS);
}

static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        // Associated, but not usable until DHCP produces an address.
        ESP_LOGI(TAG, "Associated, waiting for an address");
        xEventGroupSetBits(s_wifi_events, ASSOCIATED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, ASSOCIATED_BIT);
        // This bit was never cleared here, so wifi_is_connected() kept
        // reporting a live link after the connection had dropped.
        xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);

        WifiLock lock;
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

        WifiLock lock;
        s_retry_count = 0;
        s_reconnect_delay_ms = RECONNECT_BASE_MS;
        xEventGroupClearBits(s_wifi_events, FAIL_BIT);
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
        stop_ap();
    }
}

// Removes a stored credential the read path can no longer use. Without this,
// an over-long value written by an older build makes every subsequent boot
// report "no credentials" while NVS still holds them - permanently, and only
// after a power cycle, so it presents as "it worked until I restarted it".
static void erase_unreadable_credentials()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_key(handle, "ssid");
    nvs_erase_key(handle, "pass");
    nvs_commit(handle);
    nvs_close(handle);
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

    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "Stored credentials are longer than the radio can use; discarding them");
        erase_unreadable_credentials();
        return ESP_ERR_NOT_FOUND;
    }
    return err;
}

static esp_err_t store_credentials(const char* ssid, const char* password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "pass", password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t ensure_wifi_initialized()
{
    WifiLock lock;

    if (s_wifi_initialized) return ESP_OK;

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Power save is deliberately left at the ESP-IDF default of WIFI_PS_MIN_MODEM,
    // which still sleeps between DTIM beacons. MAX_MODEM with a longer listen
    // interval saves more, but sleeping through beacons costs latency and
    // reliability on a signal that is already attenuated by the robot's shell,
    // and retransmissions give the saving back.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr));

    s_wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t connect_with(const char* ssid, const char* password)
{
    {
        WifiLock lock;

        esp_err_t err = ensure_wifi_initialized();
        if (err != ESP_OK) return err;
        s_have_credentials = true;

        wifi_config_t wifi_config = {};
        // memcpy rather than strncpy: the driver reads these as length-bounded
        // fields, not C strings, so reserving a byte for a terminator threw
        // away the last character of a full 32-character SSID and left the
        // device retrying a network whose name it had quietly altered.
        const size_t ssid_len = std::min(strlen(ssid), sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.ssid, ssid, ssid_len);
        copy_bounded(reinterpret_cast<char*>(wifi_config.sta.password),
                     sizeof(wifi_config.sta.password), password);

        // Keep the access point up if it is running: someone may be
        // re-provisioning through it right now, and dropping it would cut them
        // off mid-request. It is torn down once the station actually connects.
        err = esp_wifi_set_mode(s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA);
        if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not configure the station: %s", esp_err_to_name(err));
            return err;
        }

        // Cancel any pending backoff attempt so it cannot race these credentials.
        s_retry_count = 0;
        s_reconnect_delay_ms = RECONNECT_BASE_MS;
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);

        xEventGroupClearBits(s_wifi_events, CONNECTED_BIT | FAIL_BIT);

        if (!s_wifi_started) {
            err = esp_wifi_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not start the radio: %s", esp_err_to_name(err));
                return err;
            }
            s_wifi_started = true;
        } else {
            // Already running, so just move the station onto the new credentials.
            esp_wifi_disconnect();
            esp_wifi_connect();
        }

        ESP_LOGI(TAG, "Connecting to '%s'...", ssid);
    }

    // Waited outside the lock: this blocks for up to half a minute, and the
    // event task needs the lock to record the very result being waited for.
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, CONNECTED_BIT | FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));

    if (bits & CONNECTED_BIT) return ESP_OK;
    if (bits & FAIL_BIT) return ESP_FAIL;
    ESP_LOGW(TAG, "Connection timed out");
    return ESP_ERR_TIMEOUT;
}

// Applies whatever credentials are pending. Exists so the web server's single
// task is never the one blocked for thirty seconds waiting for a radio.
static void apply_credentials_task(void*)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char ssid[WIFI_MAX_SSID + 1] = {};
        char pass[WIFI_MAX_PASSWORD + 1] = {};
        {
            WifiLock lock;
            copy_bounded(ssid, sizeof(ssid), s_pending_ssid);
            copy_bounded(pass, sizeof(pass), s_pending_pass);
        }
        if (ssid[0] == '\0') continue;

        esp_err_t err = connect_with(ssid, pass);
        if (err != ESP_OK) {
            // Not rolled back to the previous credentials: the usual reason
            // this fails is that the network is not there yet, and reverting
            // would undo a correct change made while the router was rebooting.
            // Background retry continues either way.
            ESP_LOGW(TAG, "Could not join '%s' (%s); still retrying in the background",
                     ssid, esp_err_to_name(err));
        }
    }
}

esp_err_t wifi_init_sta()
{
    char ssid[WIFI_MAX_SSID + 1] = {};
    char pass[WIFI_MAX_PASSWORD + 1] = {};

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
    WifiLock lock;
    return wifi_is_connected() || s_ap_active;
}

esp_err_t wifi_start_ap()
{
    // Sticky, so it survives the station reconnecting. Exists mainly because the
    // fallback is otherwise only reachable by breaking the stored credentials,
    // which is a poor way to test the one path that has to work.
    WifiLock lock;
    s_ap_forced = true;
    return start_ap();
}

WifiState wifi_state()
{
    WifiLock lock;

    // Reported before Connected, so a rescue network that is deliberately being
    // held up is visible. It used to be hidden the moment the station came
    // back, which meant the one diagnostic a sealed device has said nothing
    // about a live, joinable access point.
    if (s_ap_active) return WifiState::AccessPoint;
    if (wifi_is_connected()) return WifiState::Connected;

    if (s_wifi_initialized && (xEventGroupGetBits(s_wifi_events) & ASSOCIATED_BIT)) {
        return WifiState::Associated;
    }
    if (!s_have_credentials) return WifiState::NoCredentials;
    return WifiState::Associating;
}

const char* wifi_state_to_string(WifiState state)
{
    switch (state) {
    case WifiState::Connected:     return "connected";
    case WifiState::Associating:   return "associating";
    case WifiState::Associated:    return "associated";
    case WifiState::AccessPoint:   return "access_point";
    case WifiState::NoCredentials: return "no_credentials";
    }
    return "unknown";
}

int wifi_rssi()
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

bool wifi_ap_active()
{
    WifiLock lock;
    return s_ap_active;
}

std::string wifi_ap_ssid()
{
    // Derived from the MAC, so it can be reported before the access point has
    // ever run. That matters: the name has to be written down while the device
    // is still reachable, not discovered once it is not.
    WifiLock lock;
    if (s_ap_ssid.empty()) s_ap_ssid = build_ap_ssid();
    return s_ap_ssid;
}

// Renders an interface's address, or nothing when it does not have one. A
// zero address means the interface is up without a lease, which must read as
// "no address" rather than as 0.0.0.0.
static std::string address_of(esp_netif_t* netif)
{
    if (!netif) return {};

    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) return {};

    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return buf;
}

std::string wifi_ap_address()
{
    WifiLock lock;
    if (!s_ap_active) return {};
    return address_of(s_ap_netif);
}

std::string wifi_sta_address()
{
    // Looked up rather than stored: the station netif is created by
    // esp_netif_create_default_wifi_sta(), whose handle this module never kept.
    return address_of(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
}

bool wifi_is_default_ap_password(const char* candidate)
{
    return candidate && strcmp(candidate, DEFAULT_AP_PASSWORD) == 0;
}

bool wifi_ap_password_is_default()
{
    // Reads through the same path the access point uses, so the answer reflects
    // what would actually be advertised rather than a cached guess.
    char password[WIFI_MAX_PASSWORD + 1] = {};
    load_ap_password(password, sizeof(password));

    WifiLock lock;
    return s_ap_password_default;
}

esp_err_t wifi_set_ap_password(const char* password)
{
    const size_t len = strlen(password);
    // No empty case any more. It used to pass validation and turn the rescue
    // network open on its next start - a remote, unauthenticated request that
    // removed the authentication from the recovery interface, recoverable only
    // through a pin inside a sealed robot.
    if (len < WIFI_MIN_AP_PASSWORD || len > WIFI_MAX_PASSWORD) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ap_pass", password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        WifiLock lock;
        s_ap_password_default = wifi_is_default_ap_password(password);
        ESP_LOGI(TAG, "Access point password updated, effective next time it starts");
    }
    return err;
}

esp_err_t wifi_set_credentials(const char* ssid, const char* password)
{
    // Checked here as well as at the request handler, because the console
    // reaches this too. Values off the network decide how long these strings
    // are, and NVS refuses an over-long one - which used to be an abort.
    const size_t ssid_len = strlen(ssid);
    const size_t pass_len = strlen(password);
    if (ssid_len == 0 || ssid_len > WIFI_MAX_SSID) return ESP_ERR_INVALID_ARG;
    if (pass_len > WIFI_MAX_PASSWORD) return ESP_ERR_INVALID_ARG;

    esp_err_t err = store_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save credentials: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Credentials saved for '%s'", ssid);

    {
        WifiLock lock;
        copy_bounded(s_pending_ssid, sizeof(s_pending_ssid), ssid);
        copy_bounded(s_pending_pass, sizeof(s_pending_pass), password);

        if (!s_apply_task
            && xTaskCreate(apply_credentials_task, "wifi_apply", 4096, nullptr, 5, &s_apply_task)
                   != pdPASS) {
            s_apply_task = nullptr;
            ESP_LOGE(TAG, "Could not start the connection worker; credentials are stored "
                          "and will be used on the next restart");
            return ESP_OK;
        }
    }

    // Deliberately not esp_wifi_stop(): that would also drop the access point,
    // which is the very interface these credentials may have arrived over.
    xTaskNotifyGive(s_apply_task);
    return ESP_OK;
}
