#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "console.h"

static constexpr const char* TAG = "neato-mqtt";
static constexpr gpio_num_t LED_PIN = GPIO_NUM_8;

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

    xTaskCreate(blink_task, "blink", 2048, nullptr, 5, nullptr);
    console_init();
    wifi_init_sta();
}
