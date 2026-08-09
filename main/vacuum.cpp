#include "vacuum.h"

#include <algorithm>
#include "esp_log.h"
#include "freertos/task.h"

static constexpr const char* TAG = "vacuum";

namespace {

// Tolerates a mutex that could not be created, the way every other module here
// does. Taking a null handle is an assert inside FreeRTOS, which would turn a
// failed allocation at construction into a panic much later and somewhere else.
class Lock {
public:
    explicit Lock(SemaphoreHandle_t m) : m_(m) { if (m_) xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { if (m_) xSemaphoreGive(m_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t m_;
};

}  // namespace

Vacuum::Vacuum()
    : mutex_(xSemaphoreCreateMutex())
{
    if (!mutex_) ESP_LOGE(TAG, "Could not create the state mutex");
}

void Vacuum::start()
{
    Lock lock(mutex_);
    ESP_LOGI(TAG, "Starting cleaning");
    state_ = VacuumState::CLEANING;
}

void Vacuum::stop()
{
    Lock lock(mutex_);
    ESP_LOGI(TAG, "Stopping");
    state_ = VacuumState::IDLE;
}

void Vacuum::return_to_base()
{
    Lock lock(mutex_);
    ESP_LOGI(TAG, "Returning to base");
    state_ = VacuumState::RETURNING;
    returning_ticks_ = 2;
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
    Lock lock(mutex_);

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
}

void Vacuum::start_simulation()
{
    if (xTaskCreate(simulation_task, "vacuum_sim", 2048, this, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the simulation task; state will never advance");
    }
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
    Lock lock(mutex_);
    VacuumStatus s = {state_, battery_};
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
