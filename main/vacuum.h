#pragma once

#include <cstdint>
#include <string_view>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum class VacuumState { DOCKED, CLEANING, IDLE, RETURNING };

struct VacuumStatus {
    VacuumState state;
    int battery_level;
};

class Vacuum {
public:
    Vacuum();

    void start();
    void stop();
    void return_to_base();

    // The command vocabulary, in one place. The MQTT handler and the web
    // handler each used to carry their own copy of this list, and they had
    // already drifted: the broker was told this device supports "pause" while
    // neither dispatcher knew the word. Returns false for an unknown name.
    bool command(std::string_view name);

    // Spawns a task that advances the mock state machine until reboot.
    void start_simulation();

    VacuumStatus status();
    static const char* state_to_string(VacuumState state);

private:
    void tick();
    static void simulation_task(void* arg);

    static constexpr uint32_t SIM_TICK_MS = 2000;

    SemaphoreHandle_t mutex_;
    VacuumState state_ = VacuumState::DOCKED;
    int battery_ = 100;
    int returning_ticks_ = 0;
};
