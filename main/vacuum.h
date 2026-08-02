#pragma once

#include <cstdint>
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
