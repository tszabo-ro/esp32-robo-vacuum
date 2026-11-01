#include "ota.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>
#include "cJSON.h"
#include "firmware_version.h"
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
static constexpr const char* LATEST_RELEASE_API =
    "https://api.github.com/repos/tszabo-ro/esp32-robo-vacuum/releases/latest";
static constexpr size_t MAX_RELEASE_JSON_BYTES = 6144;

namespace {

class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex)
    {
        if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~Lock() { if (mutex_) xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
private:
    SemaphoreHandle_t mutex_;
};

bool has_firmware_asset(cJSON* root)
{
    cJSON* assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (!cJSON_IsArray(assets)) return false;
    cJSON* asset = nullptr;
    cJSON_ArrayForEach(asset, assets) {
        cJSON* name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        if (cJSON_IsString(name) && name->valuestring
            && std::strcmp(name->valuestring, "neato-mqtt-controller.bin") == 0) return true;
    }
    return false;
}

}  // namespace

OtaUpdater::OtaUpdater()
    : check_mutex_(xSemaphoreCreateMutex())
{
    if (!check_mutex_) ESP_LOGE(TAG, "Could not create update-check mutex");
}

esp_err_t OtaUpdater::check_for_update()
{
    bool expected = false;
    if (!check_in_progress_.compare_exchange_strong(expected, true)) return ESP_ERR_INVALID_STATE;
    if (!check_mutex_) {
        check_in_progress_ = false;
        return ESP_ERR_NO_MEM;
    }
    {
        Lock lock(check_mutex_);
        check_status_ = {};
        check_status_.checking = true;
    }
    if (xTaskCreate(check_task, "ota_check", CHECK_TASK_STACK_BYTES, this, 5, nullptr) != pdPASS) {
        set_update_check_result(false, false, nullptr, "Could not start update check");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

UpdateCheckStatus OtaUpdater::update_check_status() const
{
    Lock lock(check_mutex_);
    return check_status_;
}

void OtaUpdater::set_update_check_result(bool success, bool available,
                                         const char* version, const char* error)
{
    {
        Lock lock(check_mutex_);
        check_status_.checking = false;
        check_status_.checked = success;
        check_status_.available = success && available;
        if (version) std::snprintf(check_status_.version, sizeof(check_status_.version), "%s", version);
        if (error) std::snprintf(check_status_.error, sizeof(check_status_.error), "%s", error);
    }
    check_in_progress_ = false;
}

void OtaUpdater::check_task(void* arg)
{
    auto* self = static_cast<OtaUpdater*>(arg);
    esp_err_t err = self->perform_update_check();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Update check failed: %s", esp_err_to_name(err));
        self->set_update_check_result(false, false, nullptr, esp_err_to_name(err));
    }
    vTaskDelete(nullptr);
}

esp_err_t OtaUpdater::perform_update_check()
{
    esp_http_client_config_t config = {};
    config.url = LATEST_RELEASE_API;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.buffer_size = HTTP_BUFFER_BYTES;
    config.buffer_size_tx = HTTP_BUFFER_BYTES;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_set_header(client, "User-Agent", "Neato-D5-Controller");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    if (err == ESP_OK) err = esp_http_client_open(client, 0);

    char body[MAX_RELEASE_JSON_BYTES];
    size_t used = 0;
    if (err == ESP_OK) {
        int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length > static_cast<int64_t>(sizeof(body) - 1)) {
            err = ESP_ERR_INVALID_SIZE;
        } else if (esp_http_client_get_status_code(client) != 200) {
            err = ESP_FAIL;
        } else {
            while (used + 1 < sizeof(body)) {
                int received = esp_http_client_read(client, body + used,
                                                    static_cast<int>(sizeof(body) - used - 1));
                if (received < 0) {
                    err = ESP_FAIL;
                    break;
                }
                if (received == 0) break;
                used += static_cast<size_t>(received);
            }
            if (used + 1 == sizeof(body)) err = ESP_ERR_INVALID_SIZE;
            body[used] = '\0';
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;

    cJSON* root = cJSON_ParseWithLength(body, used);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    cJSON* tag_item = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsString(tag_item) || !tag_item->valuestring
        || std::strlen(tag_item->valuestring) >= sizeof(check_status_.version)
        || !has_firmware_asset(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const esp_app_desc_t* app = esp_app_get_description();
    firmware_version::Version latest = {};
    firmware_version::Version running = {};
    if (!app || !firmware_version::parse_release_tag(tag_item->valuestring, latest)
        || !firmware_version::parse_running_version(app->version, running)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_VERSION;
    }

    const bool available = firmware_version::is_newer(latest, running);
    ESP_LOGI(TAG, "Latest release %s; update %s", tag_item->valuestring,
             available ? "available" : "not needed");
    set_update_check_result(true, available, tag_item->valuestring, nullptr);
    cJSON_Delete(root);
    return ESP_OK;
}

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
