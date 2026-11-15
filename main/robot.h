#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <string_view>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
    // Stops the poller while an OTA transfer needs the radio and CPU. UART
    // reception remains active, so resuming does not require reinitialising
    // the robot link.
    void suspend();
    void resume();
    bool suspended() const { return suspended_; }
    RobotStatus status() const;
    bool command(const char* action);

private:
    bool ensure_set_event_key(SerialConsole::Traffic traffic);
    bool exchange(const char* command, char* response, size_t capacity,
                  SerialConsole::Traffic traffic = SerialConsole::Traffic::Background);
    static void poll_task(void* arg);
    bool poll_once();
    SerialConsole& console_;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    SemaphoreHandle_t command_mutex_ = nullptr;
    RobotStatus status_ = {{0}, -1, false, 0};
    char set_event_key_[26] = {};
    TickType_t last_update_ = 0;
    TaskHandle_t poll_task_ = nullptr;
    std::atomic<bool> suspended_{false};
    bool task_started_ = false;
};
