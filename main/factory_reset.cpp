#include "factory_reset.h"

#include <atomic>
#include <cinttypes>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static constexpr const char* TAG = "factory";

// Read by the LED task as well as written here, hence atomic.
static std::atomic<bool> s_pending{false};

void FactoryReset::start()
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << PIN;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreate(watch_task, "factory", 3072, nullptr, 5, nullptr);

    ESP_LOGI(TAG, "Factory reset armed: hold GPIO%d to ground for %" PRIu32 " ms",
             static_cast<int>(PIN), HOLD_MS);
}

bool FactoryReset::pending()
{
    return s_pending;
}

void FactoryReset::watch_task(void*)
{
    uint32_t held_ms = 0;

    while (true) {
        // Active low: the internal pull-up holds it high when nothing is wired.
        if (gpio_get_level(PIN) == 0) {
            held_ms += POLL_MS;
            s_pending = true;

            if (held_ms % 1000 == 0) {
                ESP_LOGW(TAG, "Factory reset in %" PRIu32 " ms, release to cancel",
                         HOLD_MS - held_ms);
            }
            if (held_ms >= HOLD_MS) {
                wipe_and_restart();
            }
        } else {
            if (s_pending) ESP_LOGI(TAG, "Factory reset cancelled");
            held_ms = 0;
            s_pending = false;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void FactoryReset::wipe_and_restart()
{
    ESP_LOGW(TAG, "Erasing stored configuration");

    // The whole partition rather than individual namespaces, so nothing
    // survives: WiFi credentials, broker URI, access point password, and the
    // WiFi driver's own stored configuration.
    nvs_flash_deinit(); // may report handles still open; the restart settles it
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        // Restart anyway. Continuing to run against configuration that was
        // meant to be gone is worse than rebooting into it deliberately.
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "Restarting");
    vTaskDelay(pdMS_TO_TICKS(500)); // let the log drain to serial and WebSocket
    esp_restart();
}
