#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include "esp_err.h"

// Installs firmware pulled over HTTPS into the inactive OTA slot.
//
// The transfer runs on its own task, so the caller (a console command, or an
// HTTP handler) returns immediately. Progress is reported through ESP_LOG,
// which the debug web interface already mirrors to WebSocket clients, so an
// update started from the browser console reports its own progress there.
//
// Server certificates are verified against the ESP-IDF root certificate
// bundle, so any host with a publicly trusted certificate works without
// embedding a per-host certificate.
class OtaUpdater {
public:
    // Starts downloading `url` and reboots into the new image on success.
    // Returns ESP_ERR_INVALID_STATE if an update is already running, or
    // ESP_ERR_NO_MEM if the worker task could not be created.
    //
    // A successful return only means the transfer started; the outcome is
    // reported in the log. Thread-safe.
    esp_err_t start(std::string url);

    // True while a transfer is in flight. Thread-safe.
    bool in_progress() const;

    // Confirms the running image is healthy, cancelling the pending rollback.
    // Does nothing unless this boot is a new image awaiting verification, so
    // it is safe to call on every boot.
    static void mark_running_image_valid();

    // Logs which slot is running and whether it is awaiting verification.
    static void log_running_partition();

private:
    static void update_task(void* arg);
    esp_err_t perform();

    // TLS plus the HTTP client needs considerably more stack than a plain task.
    static constexpr uint32_t TASK_STACK_BYTES = 8192;

    // Log at most one progress line per this many percent.
    static constexpr int PROGRESS_LOG_STEP = 10;

    std::string url_;
    std::atomic<bool> in_progress_{false};
};
