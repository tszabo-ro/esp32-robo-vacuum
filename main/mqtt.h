#pragma once

#include <string>
#include <string_view>
#include "esp_err.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "vacuum.h"

class MqttClient {
public:
    explicit MqttClient(Vacuum& vacuum);

    esp_err_t init();
    esp_err_t start();
    esp_err_t set_broker(const std::string& uri);
    bool is_connected() const;
    const std::string& broker_uri() const;

    static constexpr const char* VACUUM_ID = "sim_vacuum";

private:
    void on_connected();
    void on_data(std::string_view data);
    void publish_state();

    static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void publish_task(void* arg);

    Vacuum& vacuum_;
    std::string broker_uri_;
    esp_mqtt_client_handle_t client_ = nullptr;
    bool connected_ = false;
};
