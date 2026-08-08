#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "console.h"
#include "vacuum.h"
#include "mqtt.h"
#include "webserver.h"
#include "ota.h"
#include "serial.h"
#include "factory_reset.h"

static constexpr const char* TAG = "neato-mqtt";
static constexpr gpio_num_t LED_PIN = GPIO_NUM_8;

// A heartbeat rather than a continuous blink: the controller runs off the
// robot's battery, and the LED was previously lit around half the time.
//
// The pattern is also the only diagnostic channel that survives a network
// failure, so it carries as much state as can still be counted by eye: a first
// group of flashes for the network state, then a longer pause, then a second
// group for signal strength when there is an association to measure.
static constexpr uint32_t HEARTBEAT_PERIOD_MS = 10000;
static constexpr uint32_t HEARTBEAT_FLASH_MS = 40;
static constexpr uint32_t HEARTBEAT_GAP_MS = 220;
static constexpr uint32_t HEARTBEAT_GROUP_GAP_MS = 1200;
static constexpr uint32_t LED_CHECK_MS = 100;

// Signal buckets. Chosen around the -64dBm seen on the bench: anything at or
// below FAIR is where association starts becoming unreliable.
static constexpr int RSSI_GOOD_DBM = -65;
static constexpr int RSSI_FAIR_DBM = -75;

// Services that cannot start until the device is on the network.
struct NetworkServices {
    MqttClient& mqtt;
    WebServer& web;
};

static void led_flash(uint32_t on_ms)
{
    gpio_set_level(LED_PIN, 0); // active-low
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    gpio_set_level(LED_PIN, 1);
}

// A countable burst: flashes separated by a gap shorter than the gap between
// groups, so two groups do not read as one longer count.
static void led_flash_group(int count)
{
    for (int i = 0; i < count; i++) {
        led_flash(HEARTBEAT_FLASH_MS);
        if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_GAP_MS));
    }
}

static int flashes_for_state(WifiState state)
{
    switch (state) {
    case WifiState::Connected:     return 1;
    case WifiState::Associating:   return 2;
    case WifiState::Associated:    return 3;
    case WifiState::AccessPoint:   return 4;
    case WifiState::NoCredentials: return 5;
    }
    return 2;
}

// 0 means there is nothing to report, so the second group is skipped entirely.
static int flashes_for_signal(int rssi_dbm)
{
    if (rssi_dbm == 0) return 0;
    if (rssi_dbm >= RSSI_GOOD_DBM) return 3;
    if (rssi_dbm >= RSSI_FAIR_DBM) return 2;
    return 1;
}

static void led_task(void* arg)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 1); // off (active-low)

    while (true) {
        if (FactoryReset::pending()) {
            // Fast blink while the reset pin is held. With the robot closed this
            // is the only confirmation that the hold has been registered.
            led_flash(LED_CHECK_MS);
            vTaskDelay(pdMS_TO_TICKS(LED_CHECK_MS));
            continue;
        }

        const WifiState state = wifi_state();
        led_flash_group(flashes_for_state(state));

        // Signal strength is the one thing that cannot be read at all without a
        // cable, and it is exactly what a marginal antenna looks like, so it
        // gets its own group whenever there is an association to measure.
        const int signal = flashes_for_signal(wifi_rssi());
        if (signal > 0) {
            vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_GROUP_GAP_MS));
            led_flash_group(signal);
        }

        // Waited in slices so a held reset pin is acknowledged promptly rather
        // than up to ten seconds later.
        for (uint32_t waited = 0; waited < HEARTBEAT_PERIOD_MS; waited += LED_CHECK_MS) {
            if (FactoryReset::pending()) break;
            vTaskDelay(pdMS_TO_TICKS(LED_CHECK_MS));
        }
    }
}

// Starting the console involves probing the terminal for escape-sequence
// support, which is a blocking read on stdin. With a USB host attached that
// completes; with nothing plugged in it stalls, and running it inline stalled
// app_main with it - so wifi_init_sta() never ran, and the device sat with no
// station, no fallback access point, and no way in. On its own task it cannot
// gate the network: a headless device has to get online whether or not anyone
// is plugged into it.
static void console_task(void* arg)
{
    console_init(*static_cast<OtaUpdater*>(arg));
    vTaskDelete(nullptr);
}

// Brings up the web server and MQTT client once WiFi is up. Credentials can
// arrive well after boot (via the console), so this waits instead of giving up.
static void network_services_task(void* arg)
{
    auto* services = static_cast<NetworkServices*>(arg);

    // Either interface will do: when the station cannot connect, the fallback
    // access point still needs to serve the web interface so the device can be
    // re-provisioned.
    while (!wifi_has_network()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_err_t web_err = services->web.start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed to start");
    }

    // The rest needs a real network, not just the rescue access point. Waiting
    // indefinitely is deliberate: the station may take a while, and a good image
    // must not be rolled back merely because the router was slow to come back.
    while (!wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // init() warns on its own when no broker is stored yet; the broker can
    // still be set later from the web UI, which restarts the client.
    if (services->mqtt.init() == ESP_OK) {
        ESP_ERROR_CHECK(services->mqtt.start());
    }

    // Health check for a freshly installed image: the station is on the network
    // and the web server is serving, so this build can still be updated
    // remotely. Confirming any less would risk keeping an image that cannot be
    // replaced over the air - which is why reaching only the fallback access
    // point does not count. MQTT is excluded too: an unreachable broker is a
    // configuration problem, not a reason to discard working firmware.
    if (web_err == ESP_OK) {
        OtaUpdater::mark_running_image_valid();
    }

    vTaskDelete(nullptr);
}

extern "C" void app_main()
{
    const esp_app_desc_t* app = esp_app_get_description();

    ESP_LOGI(TAG, "=== Neato D5 MQTT Controller ===");
    ESP_LOGI(TAG, "Version: %s", app ? app->version : "unknown");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "System initialized successfully");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    OtaUpdater::log_running_partition();

    // Armed first, so a wedged configuration can still be cleared even if
    // something later in startup misbehaves.
    static FactoryReset factory;
    factory.start();

    // app_main returns once setup is done, so these must outlive its stack.
    static Vacuum vacuum;
    static MqttClient mqtt(vacuum);
    static SerialPort serial;
    static WebServer web(vacuum, mqtt, serial);
    static OtaUpdater ota;
    static NetworkServices services{mqtt, web};

    // Independent of WiFi, so the UART is live even with no network.
    if (serial.start() != ESP_OK) {
        ESP_LOGE(TAG, "Serial port failed to start");
    }

    xTaskCreate(led_task, "led", 2048, nullptr, 5, nullptr);
    xTaskCreate(console_task, "console", 4096, &ota, 5, nullptr);
    vacuum.start_simulation();

    // Started before wifi_init_sta() so the services come up as soon as the
    // connection lands; wifi_init_sta() itself blocks until connect or timeout.
    xTaskCreate(network_services_task, "net_svc", 4096, &services, 5, nullptr);
    wifi_init_sta();
}
