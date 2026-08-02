#include "ota.h"

#include <cinttypes>
#include <utility>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr const char* TAG = "ota";
static constexpr int HTTP_TIMEOUT_MS = 10000;
static constexpr int HTTP_BUFFER_BYTES = 2048;

esp_err_t OtaUpdater::start(std::string url)
{
    bool expected = false;
    if (!in_progress_.compare_exchange_strong(expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }

    url_ = std::move(url);

    if (xTaskCreate(update_task, "ota", TASK_STACK_BYTES, this, 5, nullptr) != pdPASS) {
        in_progress_ = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool OtaUpdater::in_progress() const
{
    return in_progress_;
}

void OtaUpdater::update_task(void* arg)
{
    auto* self = static_cast<OtaUpdater*>(arg);

    esp_err_t err = self->perform();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Update installed, rebooting into the new image");
        vTaskDelay(pdMS_TO_TICKS(500)); // let the log reach serial and WebSocket clients
        esp_restart();
    }

    ESP_LOGE(TAG, "Update failed: %s", esp_err_to_name(err));
    self->in_progress_ = false;
    vTaskDelete(nullptr);
}

esp_err_t OtaUpdater::perform()
{
    ESP_LOGI(TAG, "Starting update from %s", url_.c_str());

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url_.c_str();
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms = HTTP_TIMEOUT_MS;
    http_cfg.keep_alive_enable = true;
    // A GitHub release download redirects to a signed URL roughly 800
    // characters long, which does not fit the default 512 byte request buffer.
    http_cfg.buffer_size = HTTP_BUFFER_BYTES;
    http_cfg.buffer_size_tx = HTTP_BUFFER_BYTES;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start download: %s", esp_err_to_name(err));
        return err;
    }

    const int total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "Image size: %d bytes", total);

    int last_logged = -PROGRESS_LOG_STEP;
    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        const int read = esp_https_ota_get_image_len_read(handle);
        const int percent = (total > 0) ? (read * 100 / total) : 0;
        if (percent >= last_logged + PROGRESS_LOG_STEP) {
            ESP_LOGI(TAG, "Progress: %d%% (%d/%d bytes)", percent, read, total);
            last_logged = percent;
        }
    }

    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        return err;
    }

    // A truncated transfer can still end the loop without an error, and writing
    // a partial image would hand the bootloader something unbootable.
    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "Incomplete image received");
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    return esp_https_ota_finish(handle);
}

void OtaUpdater::mark_running_image_valid()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "Image on '%s' confirmed healthy, rollback cancelled", running->label);
    } else {
        ESP_LOGE(TAG, "Failed to confirm image, it will be rolled back on restart");
    }
}

void OtaUpdater::log_running_partition()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t state;
    const bool known = esp_ota_get_state_partition(running, &state) == ESP_OK;
    const bool pending = known && state == ESP_OTA_IMG_PENDING_VERIFY;

    // ESP-IDF derives the version from `git describe` unless PROJECT_VER is set,
    // so a release tag shows up here without any extra plumbing.
    const esp_app_desc_t* desc = esp_app_get_description();

    ESP_LOGI(TAG, "Running '%s' from '%s' at 0x%" PRIx32 "%s",
             desc ? desc->version : "unknown",
             running->label, running->address,
             pending ? " (awaiting verification)" : "");
}
