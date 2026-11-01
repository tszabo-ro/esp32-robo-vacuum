#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "serial_console.h"

struct RobotStatus {
    char state[64];
    int battery_percent;
    bool available;
    uint32_t age_seconds;
};

// Polls the Neato D5 over its UART and keeps a small, thread-safe status cache.
class NeatoRobot {
public:
    explicit NeatoRobot(SerialConsole& console);
    void start();
    RobotStatus status() const;
    bool command(const char* action);

private:
    bool exchange(const char* command, char* response, size_t capacity);
    static void poll_task(void* arg);
    bool poll_once();
    SerialConsole& console_;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    RobotStatus status_ = {{0}, -1, false, 0};
    TickType_t last_update_ = 0;
    bool task_started_ = false;
};
