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

static constexpr const char* TAG = "neato-mqtt";
static constexpr gpio_num_t LED_PIN = GPIO_NUM_8;

// Services that cannot start until the device is on the network.
struct NetworkServices {
    MqttClient& mqtt;
    WebServer& web;
};

static void blink_task(void* arg)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 1); // off (active-low)

    bool led_on = false;
    while (true) {
        if (wifi_is_connected()) {
            led_on = !led_on;
            gpio_set_level(LED_PIN, led_on ? 0 : 1);
        } else {
            gpio_set_level(LED_PIN, 1); // off when disconnected
            led_on = false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
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

    xTaskCreate(blink_task, "blink", 2048, nullptr, 5, nullptr);
    console_init(ota);
    vacuum.start_simulation();

    // Started before wifi_init_sta() so the services come up as soon as the
    // connection lands; wifi_init_sta() itself blocks until connect or timeout.
    xTaskCreate(network_services_task, "net_svc", 4096, &services, 5, nullptr);
    wifi_init_sta();
}
