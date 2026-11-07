#include "serial_console.h"

#include "esp_log.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "serial_console";
constexpr size_t RX_CHUNK_BYTES = 256;
}

SerialConsole::SerialConsole(SerialPort& port)
    : port_(port)
    , request_mutex_(xSemaphoreCreateMutex())
    , capture_mutex_(xSemaphoreCreateMutex())
    , response_ready_(xSemaphoreCreateBinary())
{
    if (!request_mutex_ || !capture_mutex_ || !response_ready_) {
        ESP_LOGE(TAG, "Could not create request/response resources");
    }
}

bool SerialConsole::subscribe(Subscriber subscriber, void* context)
{
    if (!subscriber || subscriber_count_ >= MAX_SUBSCRIBERS || receive_task_) return false;
    subscribers_[subscriber_count_++] = {subscriber, context};
    return true;
}

esp_err_t SerialConsole::start()
{
    if (receive_task_) return ESP_OK;
    if (!port_.started()) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(receive_task, "serial_rx", 3072, this, 5, &receive_task_) != pdPASS) {
        receive_task_ = nullptr;
        ESP_LOGE(TAG, "Could not start UART receiver");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int SerialConsole::write(std::string_view data)
{
    if (request_mutex_) xSemaphoreTake(request_mutex_, portMAX_DELAY);
    int written = port_.write(data);
    if (request_mutex_) xSemaphoreGive(request_mutex_);
    return written;
}

esp_err_t SerialConsole::set_baud(uint32_t baud)
{
    if (request_mutex_) xSemaphoreTake(request_mutex_, portMAX_DELAY);
    const esp_err_t err = port_.set_baud(baud);
    if (request_mutex_) xSemaphoreGive(request_mutex_);
    return err;
}

bool SerialConsole::request(std::string_view data, uint8_t response_end, char* response,
                            size_t response_capacity, size_t& response_length,
                            uint32_t timeout_ms)
{
    if (!request_mutex_ || !capture_mutex_ || !response_ready_ || !started() || data.empty()
        || !response || response_capacity < 2) return false;

    xSemaphoreTake(request_mutex_, portMAX_DELAY);
    // Clear a completion from a previous request before publishing this
    // caller's stack buffer to the receive task.
    xSemaphoreTake(response_ready_, 0);
    xSemaphoreTake(capture_mutex_, portMAX_DELAY);
    response_buffer_ = response;
    response_capacity_ = response_capacity;
    response_length_ = 0;
    response_end_ = response_end;
    response_complete_ = false;
    response_active_ = true;
    xSemaphoreGive(capture_mutex_);

    if (port_.write(data) != static_cast<int>(data.size())) {
        xSemaphoreTake(capture_mutex_, portMAX_DELAY);
        response_active_ = false;
        response_buffer_ = nullptr;
        xSemaphoreGive(capture_mutex_);
        xSemaphoreGive(request_mutex_);
        return false;
    }

    xSemaphoreTake(response_ready_, pdMS_TO_TICKS(timeout_ms));
    xSemaphoreTake(capture_mutex_, portMAX_DELAY);
    const bool complete = response_complete_;
    response_length = response_length_;
    response[response_length] = '\0';
    response_active_ = false;
    response_buffer_ = nullptr;
    xSemaphoreGive(capture_mutex_);
    xSemaphoreGive(request_mutex_);
    return complete;
}

void SerialConsole::broadcast(const char* data, size_t length)
{
    capture_response(data, length);
    for (size_t i = 0; i < subscriber_count_; ++i) {
        subscribers_[i].callback(subscribers_[i].context, data, length);
    }
}

void SerialConsole::capture_response(const char* data, size_t length)
{
    if (!capture_mutex_ || !response_ready_) return;

    bool ready = false;
    xSemaphoreTake(capture_mutex_, portMAX_DELAY);
    if (response_active_) {
        for (size_t i = 0; i < length; ++i) {
            if (static_cast<uint8_t>(data[i]) == response_end_) {
                response_complete_ = true;
                response_active_ = false;
                ready = true;
                break;
            }
            if (response_length_ + 1 >= response_capacity_) {
                ESP_LOGW(TAG, "Robot response exceeded %u bytes",
                         static_cast<unsigned>(response_capacity_));
                response_active_ = false;
                ready = true;
                break;
            }
            response_buffer_[response_length_++] = data[i];
        }
    }
    xSemaphoreGive(capture_mutex_);
    if (ready) xSemaphoreGive(response_ready_);
}

void SerialConsole::receive_task(void* arg)
{
    auto* self = static_cast<SerialConsole*>(arg);
    char buf[RX_CHUNK_BYTES];
    while (true) {
        size_t received = self->port_.read(buf, sizeof(buf), 100);
        if (received > 0) self->broadcast(buf, received);
    }
}
