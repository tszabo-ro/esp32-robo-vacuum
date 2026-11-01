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
    , response_stream_(xStreamBufferCreate(2048, 1))
{
    if (!request_mutex_ || !response_stream_) {
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

bool SerialConsole::request(std::string_view data, uint8_t response_end, char* response,
                            size_t response_capacity, size_t& response_length,
                            uint32_t timeout_ms)
{
    if (!request_mutex_ || !response_stream_ || !started() || data.empty()
        || !response || response_capacity < 2) return false;

    xSemaphoreTake(request_mutex_, portMAX_DELAY);
    xStreamBufferReset(response_stream_);
    response_length = 0;
    awaiting_response_ = true;
    if (port_.write(data) != static_cast<int>(data.size())) {
        awaiting_response_ = false;
        xSemaphoreGive(request_mutex_);
        return false;
    }

    const TickType_t started = xTaskGetTickCount();
    bool complete = false;
    while (response_length + 1 < response_capacity) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const uint32_t elapsed_ms = static_cast<uint32_t>(elapsed * portTICK_PERIOD_MS);
        if (elapsed_ms >= timeout_ms) break;
        const uint32_t remaining = timeout_ms - elapsed_ms;
        char byte = 0;
        if (xStreamBufferReceive(response_stream_, &byte, 1,
                                 pdMS_TO_TICKS(remaining)) != 1) break;
        if (static_cast<uint8_t>(byte) == response_end) {
            complete = true;
            break;
        }
        response[response_length++] = byte;
    }
    response[response_length] = '\0';
    awaiting_response_ = false;
    xSemaphoreGive(request_mutex_);
    return complete;
}

void SerialConsole::broadcast(const char* data, size_t length)
{
    if (awaiting_response_.load() && response_stream_) {
        xStreamBufferSend(response_stream_, data, length, 0);
    }
    for (size_t i = 0; i < subscriber_count_; ++i) {
        subscribers_[i].callback(subscribers_[i].context, data, length);
    }
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
