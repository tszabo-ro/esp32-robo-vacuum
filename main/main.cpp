#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "console.h"
#include "vacuum.h"
#include "mqtt.h"
#include "webserver.h"
#include "ota.h"

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

    while (!wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_err_t web_err = services->web.start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed to start");
    }

    // init() warns on its own when no broker is stored yet; the broker can
    // still be set later from the web UI, which restarts the client.
    if (services->mqtt.init() == ESP_OK) {
        ESP_ERROR_CHECK(services->mqtt.start());
    }

    // Health check for a freshly installed image: WiFi is up and the web server
    // is serving, so this build can still be updated remotely. Confirming any
    // less would risk keeping an image that cannot be replaced over the air.
    // MQTT is deliberately excluded - an unreachable broker is a config problem,
    // not a reason to discard working firmware.
    if (web_err == ESP_OK) {
        OtaUpdater::mark_running_image_valid();
    }

    vTaskDelete(nullptr);
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "=== Neato D5 MQTT Controller ===");
    ESP_LOGI(TAG, "Version: 0.1.0");
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
    static WebServer web(vacuum, mqtt);
    static OtaUpdater ota;
    static NetworkServices services{mqtt, web};

    xTaskCreate(blink_task, "blink", 2048, nullptr, 5, nullptr);
    console_init(ota);
    vacuum.start_simulation();

    // Started before wifi_init_sta() so the services come up as soon as the
    // connection lands; wifi_init_sta() itself blocks until connect or timeout.
    xTaskCreate(network_services_task, "net_svc", 4096, &services, 5, nullptr);
    wifi_init_sta();
}
