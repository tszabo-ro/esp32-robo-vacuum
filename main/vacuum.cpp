#include "vacuum.h"

#include <algorithm>
#include "esp_log.h"
#include "freertos/task.h"

static constexpr const char* TAG = "vacuum";

Vacuum::Vacuum()
    : mutex_(xSemaphoreCreateMutex())
{
}

void Vacuum::start()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ESP_LOGI(TAG, "Starting cleaning");
    state_ = VacuumState::CLEANING;
    xSemaphoreGive(mutex_);
}

void Vacuum::stop()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ESP_LOGI(TAG, "Stopping");
    state_ = VacuumState::IDLE;
    xSemaphoreGive(mutex_);
}

void Vacuum::return_to_base()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ESP_LOGI(TAG, "Returning to base");
    state_ = VacuumState::RETURNING;
    returning_ticks_ = 2;
    xSemaphoreGive(mutex_);
}

bool Vacuum::command(std::string_view name)
{
    if (name == "start")          { start();          return true; }
    if (name == "stop")           { stop();           return true; }
    if (name == "return_to_base") { return_to_base(); return true; }
    return false;
}

void Vacuum::tick()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);

    switch (state_) {
    case VacuumState::CLEANING:
        battery_ = std::max(0, battery_ - 1);
        if (battery_ <= 20) {
            ESP_LOGI(TAG, "Low battery (%d%%), returning to base", battery_);
            state_ = VacuumState::RETURNING;
            returning_ticks_ = 2;
        }
        break;

    case VacuumState::RETURNING:
        if (--returning_ticks_ <= 0) {
            ESP_LOGI(TAG, "Docked");
            state_ = VacuumState::DOCKED;
        }
        break;

    case VacuumState::DOCKED:
        battery_ = std::min(100, battery_ + 2);
        break;

    case VacuumState::IDLE:
        break;
    }

    xSemaphoreGive(mutex_);
}

void Vacuum::start_simulation()
{
    xTaskCreate(simulation_task, "vacuum_sim", 2048, this, 5, nullptr);
}

void Vacuum::simulation_task(void* arg)
{
    auto* self = static_cast<Vacuum*>(arg);

    while (true) {
        self->tick();
        vTaskDelay(pdMS_TO_TICKS(SIM_TICK_MS));
    }
}

VacuumStatus Vacuum::status()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    VacuumStatus s = {state_, battery_};
    xSemaphoreGive(mutex_);
    return s;
}

const char* Vacuum::state_to_string(VacuumState state)
{
    switch (state) {
    case VacuumState::DOCKED:    return "docked";
    case VacuumState::CLEANING:  return "cleaning";
    case VacuumState::IDLE:      return "idle";
    case VacuumState::RETURNING: return "returning";
    }
    return "unknown";
}
