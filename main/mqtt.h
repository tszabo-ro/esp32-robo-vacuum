#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "vacuum.h"

class MqttClient {
public:
    explicit MqttClient(Vacuum& vacuum);

    esp_err_t init();
    esp_err_t start();

    // Stores the broker URI and restarts the client against it. Returns
    // ESP_ERR_INVALID_ARG for a URI this device cannot use, so a typo is
    // rejected rather than persisted and retried on every boot.
    esp_err_t set_broker(const std::string& uri);

    bool is_connected() const;

    // By value. This used to hand out a reference to a member that init()
    // reassigns from another task, so a status response could be formatting a
    // string that had just been freed underneath it.
    std::string broker_uri() const;

    // Replaces "user:password@" with "***@". esp-mqtt accepts credentials
    // embedded in the URI, which is the natural thing for someone to type - and
    // this value is logged, and every log line goes to every connected browser.
    static std::string redact_uri(std::string_view uri);

    static constexpr const char* VACUUM_ID = "sim_vacuum";

private:
    void on_connected(esp_mqtt_client_handle_t client);
    void on_data(std::string_view topic, std::string_view data);
    void publish_state();

    static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void publish_task(void* arg);

    Vacuum& vacuum_;

    // Guards client_ and broker_uri_ against the publisher task and the web
    // server's task. Deliberately never taken from the MQTT event task:
    // set_broker holds it across esp_mqtt_client_stop/destroy, which block
    // waiting on that task, so an event handler waiting for the lock would
    // deadlock the pair.
    mutable SemaphoreHandle_t mutex_;

    std::string broker_uri_;
    esp_mqtt_client_handle_t client_ = nullptr;

    // Written from the MQTT event task, read from two others.
    std::atomic<bool> connected_{false};

    // Created once. It used to be spawned by every start(), so each broker
    // change left another 4 KB task publishing the same state forever.
    TaskHandle_t publish_task_ = nullptr;

    // Built once rather than reassembled on every publish, one of which runs
    // every two seconds for the life of the device.
    const std::string state_topic_;
    const std::string command_topic_;
    const std::string availability_topic_;
};
