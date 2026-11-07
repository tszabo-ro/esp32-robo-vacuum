#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "serial.h"

// Owns UART receive and broadcasts incoming bytes to independent subscribers.
// Clients send through write(); none of them reads directly from the UART.
class SerialConsole {
public:
    using Subscriber = void (*)(void* context, const char* data, size_t length);
    explicit SerialConsole(SerialPort& port);
    bool subscribe(Subscriber subscriber, void* context);
    esp_err_t start();
    int write(std::string_view data);
    bool request(std::string_view data, uint8_t response_end, char* response,
                 size_t response_capacity, size_t& response_length, uint32_t timeout_ms);
    esp_err_t set_baud(uint32_t baud);
    uint32_t baud() const { return port_.baud(); }
    bool started() const { return port_.started() && receive_task_ != nullptr; }

private:
    struct Slot {
        Subscriber callback = nullptr;
        void* context = nullptr;
    };
    static void receive_task(void* arg);
    void broadcast(const char* data, size_t length);
    void capture_response(const char* data, size_t length);

    static constexpr size_t MAX_SUBSCRIBERS = 4;
    SerialPort& port_;
    Slot subscribers_[MAX_SUBSCRIBERS] = {};
    size_t subscriber_count_ = 0;
    TaskHandle_t receive_task_ = nullptr;
    // Serializes whole requests and terminal writes. It deliberately does not
    // guard RX: the UART is full-duplex and the receiver must remain live
    // while a request waits for its reply.
    SemaphoreHandle_t request_mutex_ = nullptr;
    // Guards the lifetime of the caller-owned response buffer below. The RX
    // task takes it only while copying a received chunk, so a timed-out caller
    // can never return while the receiver still holds its stack pointer.
    SemaphoreHandle_t capture_mutex_ = nullptr;
    SemaphoreHandle_t response_ready_ = nullptr;
    char* response_buffer_ = nullptr;
    size_t response_capacity_ = 0;
    size_t response_length_ = 0;
    uint8_t response_end_ = 0;
    bool response_active_ = false;
    bool response_complete_ = false;
};
