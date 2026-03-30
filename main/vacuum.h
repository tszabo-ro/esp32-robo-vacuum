#pragma once

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
    void tick();

    VacuumStatus status();
    static const char* state_to_string(VacuumState state);

private:
    SemaphoreHandle_t mutex_;
    VacuumState state_ = VacuumState::DOCKED;
    int battery_ = 100;
    int returning_ticks_ = 0;
};
