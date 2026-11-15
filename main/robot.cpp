#include "robot.h"

#include <cstdio>
#include <cstring>
#include <string_view>
#include "esp_log.h"
#include "freertos/task.h"
#include "robot_response.h"

namespace {
constexpr const char* TAG = "robot";
constexpr uint32_t POLL_PERIOD_MS = 1000;
constexpr uint32_t COMMAND_TIMEOUT_MS = 1800;
constexpr size_t RESPONSE_BYTES = 1024;
// D5 GetVersion includes battery and hardware metadata and exceeds 1 KiB.
constexpr size_t VERSION_RESPONSE_BYTES = 2048;
constexpr size_t COMMAND_BYTES = 128;
// A poll owns a 1 KiB response buffer and logs the full reply. The old 3 KiB
// task stack left only a few bytes free once the UART request, parser and
// newlib's logging locks were nested, causing a stack-protection panic after a
// successful robot exchange.
constexpr uint32_t POLL_TASK_STACK_BYTES = 6144;

class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex)
    {
        if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~Lock() { if (mutex_) xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
private:
    SemaphoreHandle_t mutex_;
};
}

NeatoRobot::NeatoRobot(SerialConsole& console)
    : console_(console)
    , mutex_(xSemaphoreCreateMutex())
    , command_mutex_(xSemaphoreCreateMutex())
{
    if (!mutex_ || !command_mutex_) ESP_LOGE(TAG, "Could not create robot mutexes");
    std::strcpy(status_.state, "unknown");
}

void NeatoRobot::start()
{
    if (task_started_) return;
    task_started_ = xTaskCreate(poll_task, "robot_poll", POLL_TASK_STACK_BYTES, this, 5,
                                &poll_task_) == pdPASS;
    if (!task_started_) ESP_LOGE(TAG, "Could not start the status poller");
}

void NeatoRobot::suspend()
{
    if (suspended_.exchange(true)) return;
    if (poll_task_) vTaskSuspend(poll_task_);
    if (mutex_) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        status_.available = false;
        xSemaphoreGive(mutex_);
    }
    ESP_LOGI(TAG, "Robot polling suspended for OTA update");
}

void NeatoRobot::resume()
{
    if (!suspended_.exchange(false)) return;
    if (poll_task_) vTaskResume(poll_task_);
    ESP_LOGI(TAG, "Robot polling resumed after OTA update failure");
}

RobotStatus NeatoRobot::status() const
{
    RobotStatus result = {};
    if (!mutex_) return result;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    result = status_;
    if (last_update_ != 0) {
        result.age_seconds = static_cast<uint32_t>((xTaskGetTickCount() - last_update_)
                                                   * portTICK_PERIOD_MS / 1000);
    }
    xSemaphoreGive(mutex_);
    return result;
}

bool NeatoRobot::command(const char* action)
{
    if (!action || suspended_) return false;
    const bool clear_errors = std::strcmp(action, "clear") == 0;
    const char* event = robot_response::set_event_for_action(action);
    if ((!event && !clear_errors) || !command_mutex_) return false;

    Lock command_lock(command_mutex_);
    if (!clear_errors && !ensure_set_event_key(SerialConsole::Traffic::Interactive)) {
        ESP_LOGW(TAG, "Could not initialise the robot control key");
        return false;
    }

    char wire[COMMAND_BYTES];
    if (clear_errors) std::strcpy(wire, "SetUIError clearall");
    else if (!robot_response::build_set_event_command(event, set_event_key_, wire, sizeof(wire))) return false;

    char response[512];
    if (!exchange(wire, response, sizeof(response), SerialConsole::Traffic::Interactive)) {
        ESP_LOGW(TAG, "Robot command '%s' did not get a complete reply", action);
        return false;
    }
    if (clear_errors) {
        // Read back rather than optimistically hiding an error that can recur
        // immediately while the underlying hardware fault is still present.
        robot_response::Diagnostic error, alert;
        const bool valid = exchange("GetErr", response, sizeof(response), SerialConsole::Traffic::Interactive)
            && robot_response::parse_errors(response, error, alert);
        Lock status_lock(mutex_);
        status_.errors_available = valid;
        if (valid) {
            status_.error = error;
            status_.alert = alert;
        }
    }
    return true;
}

bool NeatoRobot::ensure_set_event_key(SerialConsole::Traffic traffic)
{
    if (set_event_key_[0] != '\0') return true;
    char response[VERSION_RESPONSE_BYTES];
    char serial_number[96];
    if (!exchange("GetVersion", response, sizeof(response), traffic)) return false;
    if (!robot_response::parse_serial_number(response, serial_number, sizeof(serial_number))) return false;
    return robot_response::derive_set_event_key(serial_number, set_event_key_, sizeof(set_event_key_));
}

bool NeatoRobot::test_lds(LdsDiagnostics& result, char* scan)
{
    if (!scan || !command_mutex_ || suspended_) {
        result.message = "Robot diagnostics are unavailable during an update";
        return false;
    }
    Lock lock(command_mutex_);
    char response[512];
    const auto send = [&](const char* cmd, char* out, size_t capacity) {
        if (!exchange(cmd, out, capacity, SerialConsole::Traffic::Interactive)) return false;
        const std::string_view reply(out);
        return reply.find("Unknown") == std::string_view::npos
            && reply.find("Invalid") == std::string_view::npos
            && reply.find("Error") == std::string_view::npos;
    };
    if (!exchange("GetState", response, sizeof(response), SerialConsole::Traffic::Interactive)
        || (!std::strstr(response, "UIMGR_STATE_IDLE\r")
            && !std::strstr(response, "UIMGR_STATE_IDLE\n")
            && !std::strstr(response, "UIMGR_STATE_STANDBY\r")
            && !std::strstr(response, "UIMGR_STATE_STANDBY\n"))) {
        result.message = "Stop the cleaning session before running the LDS test; robot must be idle or standby";
        return false;
    }
    bool ok = send("TestMode On", response, sizeof(response));
    if (ok) ok = send("SetLDSRotation On", response, sizeof(response));
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ok = exchange("GetMotors Laser", result.motors, sizeof(result.motors), SerialConsole::Traffic::Interactive);
        size_t used = 0;
        const bool scanned = console_.request("GetLDSScan\r\n", 0x1a, scan,
            LdsDiagnostics::SCAN_BYTES, used, 5000, SerialConsole::Traffic::Interactive);
        ok = ok && scanned;
    }
    // Attempt both even if starting the test timed out: the robot may have
    // accepted a command whose reply was lost. Never leave rotation enabled.
    const bool stopped = send("SetLDSRotation Off", response, sizeof(response));
    const bool exited = send("TestMode Off", response, sizeof(response));
    result.cleanup_ok = stopped && exited;
    const bool errors_read = exchange("GetErr", result.errors, sizeof(result.errors), SerialConsole::Traffic::Interactive);
    const auto summary = robot_response::summarize_lds_scan(scan);
    result.message = !result.cleanup_ok ? "Could not confirm scanner shutdown; check the robot"
        : ok && summary.points == 360 && summary.valid_points > 0 ? "Full scan received; inspect rotation speed, samples, and robot errors below"
        : ok ? "Scanner replied but scan data is missing, incomplete, or contains no usable distances"
             : "Test incomplete: a command timed out, was rejected, or exceeded the response limit";
    if (!errors_read) std::strcpy(result.errors, "Could not read robot errors");
    return ok && result.cleanup_ok && summary.points == 360 && summary.valid_points > 0;
}

bool NeatoRobot::poll_once()
{
    char response[RESPONSE_BYTES];
    char robot_state[sizeof(status_.state)];
    if (!exchange("GetState", response, sizeof(response)))
        return false;
    if (!robot_response::parse_state(response, robot_state, sizeof(robot_state))) return false;

    vTaskDelay(pdMS_TO_TICKS(50));
    if (!exchange("GetCharger", response, sizeof(response)))
        return false;

    int battery = 0;
    if (!robot_response::parse_battery_percent(response, battery)) return false;

    robot_response::Diagnostic error, alert;
    const bool errors_available = exchange("GetErr", response, sizeof(response))
        && robot_response::parse_errors(response, error, alert);

    xSemaphoreTake(mutex_, portMAX_DELAY);
    status_.errors_available = errors_available;
    if (errors_available) {
        status_.error = error;
        status_.alert = alert;
    }
    std::strcpy(status_.state, robot_state);
    status_.battery_percent = battery;
    status_.available = true;
    last_update_ = xTaskGetTickCount();
    xSemaphoreGive(mutex_);
    return true;
}

bool NeatoRobot::exchange(const char* command, char* response, size_t capacity,
                          SerialConsole::Traffic traffic)
{
    size_t used = 0;
    char request[COMMAND_BYTES];
    const size_t command_length = std::strlen(command);
    if (command_length + 2 > sizeof(request)) return false;
    std::memcpy(request, command, command_length);
    request[command_length] = '\n';
    request[command_length + 1] = '\0';
    bool complete = console_.request(std::string_view(request, command_length + 1), 0x1a,
                                     response, capacity, used, COMMAND_TIMEOUT_MS, traffic);
    if (complete) {
        const std::string_view wire(command);
        const size_t key_marker = wire.find(" SKey ");
        if (key_marker == std::string_view::npos) {
            ESP_LOGI(TAG, "Sent robot command: %s", command);
            ESP_LOGI(TAG, "Robot replied: %.*s", static_cast<int>(used), response);
        } else {
            // GetState/GetCharger replies are useful diagnostics, but SetEvent
            // replies normally echo the request. Do not publish its control key
            // through the web-visible application log.
            ESP_LOGI(TAG, "Sent authenticated robot command: %.*s",
                     static_cast<int>(key_marker), command);
            ESP_LOGI(TAG, "Robot replied to authenticated command (%u bytes)",
                     static_cast<unsigned>(used));
        }
    }
    return complete;
}

void NeatoRobot::poll_task(void* arg)
{
    auto* self = static_cast<NeatoRobot*>(arg);
    while (true) {
        const TickType_t started = xTaskGetTickCount();
        {
            Lock command_lock(self->command_mutex_);
            if (!self->console_.started() || !self->poll_once()) {
                if (self->mutex_) {
                    xSemaphoreTake(self->mutex_, portMAX_DELAY);
                    self->status_.available = false;
                    self->status_.errors_available = false;
                    xSemaphoreGive(self->mutex_);
                }
            }
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const TickType_t period = pdMS_TO_TICKS(POLL_PERIOD_MS);
        vTaskDelay(elapsed < period ? period - elapsed : 1);
    }
}
