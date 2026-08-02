#include "mqtt.h"

#include <string>
#include <string_view>
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static constexpr const char* TAG = "mqtt";
static constexpr const char* NVS_NAMESPACE = "mqtt_cfg";

MqttClient::MqttClient(Vacuum& vacuum)
    : vacuum_(vacuum)
{
}

static esp_err_t load_broker_uri(std::string& uri)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;

    size_t len = 0;
    err = nvs_get_str(handle, "uri", nullptr, &len);
    if (err == ESP_OK && len > 0) {
        uri.resize(len - 1);
        err = nvs_get_str(handle, "uri", uri.data(), &len);
    }
    nvs_close(handle);
    return err;
}

esp_err_t MqttClient::init()
{
    std::string uri;
    esp_err_t err = load_broker_uri(uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No MQTT broker configured. Use 'mqtt_set <uri>' to set one.");
        return ESP_ERR_NOT_FOUND;
    }

    broker_uri_ = uri;
    auto lwt_topic = std::string(VACUUM_ID) + "/availability";

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri.c_str();
    cfg.session.last_will.topic = lwt_topic.c_str();
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.retain = 1;
    cfg.session.last_will.qos = 1;

    client_ = esp_mqtt_client_init(&cfg);
    if (!client_) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        client_, MQTT_EVENT_ANY, event_handler, this));

    ESP_LOGI(TAG, "MQTT client initialized, broker: %s", uri.c_str());
    return ESP_OK;
}

esp_err_t MqttClient::start()
{
    if (!client_) return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_mqtt_client_start(client_);
    if (err != ESP_OK) return err;

    xTaskCreate(publish_task, "mqtt_pub", 4096, this, 5, nullptr);
    return ESP_OK;
}

esp_err_t MqttClient::set_broker(const std::string& uri)
{
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_str(handle, "uri", uri.c_str()));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    ESP_LOGI(TAG, "Broker URI saved: %s", uri.c_str());

    if (client_) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        connected_ = false;
    }

    return init() == ESP_OK ? start() : ESP_FAIL;
}

bool MqttClient::is_connected() const
{
    return connected_;
}

const std::string& MqttClient::broker_uri() const
{
    return broker_uri_;
}

void MqttClient::on_connected()
{
    ESP_LOGI(TAG, "Connected to broker");
    connected_ = true;

    auto id = std::string(VACUUM_ID);

    // Publish HA MQTT discovery config
    cJSON* config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "name", "Sim Vacuum");
    cJSON_AddStringToObject(config, "unique_id", VACUUM_ID);
    cJSON_AddStringToObject(config, "command_topic", (id + "/command").c_str());
    cJSON_AddStringToObject(config, "state_topic", (id + "/state").c_str());
    cJSON_AddStringToObject(config, "json_attributes_topic", (id + "/state").c_str());
    cJSON_AddStringToObject(config, "availability_topic", (id + "/availability").c_str());
    cJSON_AddStringToObject(config, "payload_available", "online");
    cJSON_AddStringToObject(config, "payload_not_available", "offline");

    cJSON* features = cJSON_AddArrayToObject(config, "supported_features");
    for (auto feature : {"start", "pause", "stop", "return_home", "battery"}) {
        cJSON_AddItemToArray(features, cJSON_CreateString(feature));
    }

    char* json = cJSON_PrintUnformatted(config);
    auto discovery_topic = "homeassistant/vacuum/" + id + "/config";
    esp_mqtt_client_publish(client_, discovery_topic.c_str(), json, 0, 1, 1);
    cJSON_free(json);
    cJSON_Delete(config);

    // Publish availability
    auto avail_topic = id + "/availability";
    esp_mqtt_client_publish(client_, avail_topic.c_str(), "online", 0, 1, 1);

    // Subscribe to commands
    auto cmd_topic = id + "/command";
    esp_mqtt_client_subscribe(client_, cmd_topic.c_str(), 1);
}

void MqttClient::on_data(std::string_view data)
{
    if (data == "start") {
        vacuum_.start();
    } else if (data == "stop") {
        vacuum_.stop();
    } else if (data == "return_to_base") {
        vacuum_.return_to_base();
    } else {
        ESP_LOGW(TAG, "Unknown command: %.*s", static_cast<int>(data.size()), data.data());
    }
}

void MqttClient::publish_state()
{
    if (!connected_) return;

    auto s = vacuum_.status();

    cJSON* state = cJSON_CreateObject();
    cJSON_AddStringToObject(state, "state", Vacuum::state_to_string(s.state));
    cJSON_AddNumberToObject(state, "battery_level", s.battery_level);
    char* json = cJSON_PrintUnformatted(state);

    auto topic = std::string(VACUUM_ID) + "/state";
    esp_mqtt_client_publish(client_, topic.c_str(), json, 0, 0, 1);

    cJSON_free(json);
    cJSON_Delete(state);
}

void MqttClient::event_handler(void* arg, esp_event_base_t, int32_t id, void* data)
{
    auto* self = static_cast<MqttClient*>(arg);
    auto* event = static_cast<esp_mqtt_event_handle_t>(data);

    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        self->on_connected();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        self->connected_ = false;
        break;
    case MQTT_EVENT_DATA:
        self->on_data({event->data, static_cast<size_t>(event->data_len)});
        break;
    default:
        break;
    }
}

void MqttClient::publish_task(void* arg)
{
    auto* self = static_cast<MqttClient*>(arg);

    while (true) {
        self->publish_state();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
