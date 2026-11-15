#include "mqtt.h"
#include "text_util.h"
#include "robot_mqtt.h"

#include <string>
#include <string_view>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static constexpr const char* TAG = "mqtt";
static constexpr const char* NVS_NAMESPACE = "mqtt_cfg";

// Long enough for a hostname, a port and a path; short enough that it cannot be
// the thing that fills a 16 KB NVS partition.
static constexpr size_t MAX_BROKER_URI = 128;

static constexpr uint32_t PUBLISH_PERIOD_MS = 2000;

namespace {

class Lock {
public:
    explicit Lock(SemaphoreHandle_t m) : m_(m) { if (m_) xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { if (m_) xSemaphoreGive(m_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t m_;
};

// esp-mqtt accepts mqtt, mqtts, ws and wss. Anything else is a typo, and
// storing a typo means every boot from here on logs a failed init and silently
// runs without MQTT.
bool scheme_is_supported(std::string_view uri)
{
    for (auto scheme : {"mqtt://", "mqtts://", "ws://", "wss://"}) {
        const std::string_view s(scheme);
        if (uri.size() > s.size() && uri.substr(0, s.size()) == s) return true;
    }
    return false;
}

void add_robot_device(cJSON* config)
{
    cJSON* device = cJSON_AddObjectToObject(config, "device");
    cJSON* identifiers = cJSON_AddArrayToObject(device, "identifiers");
    cJSON_AddItemToArray(identifiers, cJSON_CreateString(MqttClient::VACUUM_ID));
    cJSON_AddStringToObject(device, "name", "Neato D5");
    cJSON_AddStringToObject(device, "manufacturer", "Neato Robotics");
    cJSON_AddStringToObject(device, "model", "Botvac D5");
}

}  // namespace

MqttClient::MqttClient(NeatoRobot& robot)
    : robot_(robot)
    , command_queue_(xQueueCreate(4, sizeof(const char*)))
    , mutex_(xSemaphoreCreateMutex())
    , state_topic_(std::string(VACUUM_ID) + "/state")
    , command_topic_(std::string(VACUUM_ID) + "/command")
    , availability_topic_(std::string(VACUUM_ID) + "/availability")
{
    if (!mutex_) ESP_LOGE(TAG, "Could not create the client mutex");
    if (!command_queue_) ESP_LOGE(TAG, "Could not create the robot command queue");
}

static esp_err_t load_broker_uri(std::string& uri)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;

    size_t len = 0;
    err = nvs_get_str(handle, "uri", nullptr, &len);
    if (err == ESP_OK && len > 0 && len <= MAX_BROKER_URI + 1) {
        uri.resize(len - 1);
        err = nvs_get_str(handle, "uri", uri.data(), &len);
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(handle);
    return err;
}

esp_err_t MqttClient::init()
{
    std::string uri;
    esp_err_t err = load_broker_uri(uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No MQTT broker configured. Set one from the web interface.");
        return ESP_ERR_NOT_FOUND;
    }

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri.c_str();
    // The bundle is already compiled in and the firmware updater uses it, but
    // this client did not - which left an mqtts:// URI with no trust anchor at
    // all, so it simply failed and pushed people back onto plaintext.
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.session.last_will.topic = availability_topic_.c_str();
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.retain = 1;
    cfg.session.last_will.qos = 1;

    // esp_mqtt_client_init deep-copies the URI and the topic, so the local and
    // the member outliving this call are both fine - it reads like a lifetime
    // bug and is not one.
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create MQTT client for '%s'", text::redact_uri(uri).c_str());
        return ESP_FAIL;
    }

    err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, event_handler, this);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not register the MQTT event handler: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client);
        return err;
    }

    {
        Lock lock(mutex_);
        broker_uri_ = uri;
        client_ = client;
    }

    // Redacted: esp-mqtt reads credentials out of the URI, and every log line
    // this device produces is broadcast to each connected browser.
    ESP_LOGI(TAG, "MQTT client initialized, broker: %s", text::redact_uri(uri).c_str());
    return ESP_OK;
}

esp_err_t MqttClient::start()
{
    if (!command_queue_) return ESP_ERR_NO_MEM;
    esp_mqtt_client_handle_t client = nullptr;
    {
        Lock lock(mutex_);
        client = client_;
    }
    if (!client) return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) return err;

    // Created at most once for the life of the device. The publisher no-ops
    // while there is no client, so a broker change does not need a new one.
    if (!publish_task_
        && xTaskCreate(publish_task, "mqtt_pub", 6144, this, 5, &publish_task_) != pdPASS) {
        publish_task_ = nullptr;
        ESP_LOGE(TAG, "Could not start the publisher task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t MqttClient::set_broker(const std::string& uri)
{
    // Validated before it is stored. It used to be written to NVS first and
    // tested afterwards, so a typo was persisted permanently and every
    // subsequent boot logged a failed init and ran with no MQTT at all.
    if (uri.size() > MAX_BROKER_URI || !scheme_is_supported(uri)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "uri", uri.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the broker URI: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Broker URI saved: %s", text::redact_uri(uri).c_str());

    // The lock is held across the teardown, not just across nulling the
    // pointer. Nulling first only narrows the window: the publisher can already
    // be past its own check and inside esp_mqtt_client_publish, holding a
    // pointer to a client this call is about to free.
    {
        Lock lock(mutex_);
        if (client_) {
            esp_mqtt_client_handle_t dying = client_;
            client_ = nullptr;
            connected_ = false;
            esp_mqtt_client_stop(dying);
            esp_mqtt_client_destroy(dying);
        }
    }

    err = init();
    if (err != ESP_OK) return err;
    return start();
}

bool MqttClient::is_connected() const
{
    return connected_;
}

std::string MqttClient::broker_uri() const
{
    Lock lock(mutex_);
    return broker_uri_;
}

void MqttClient::on_connected(esp_mqtt_client_handle_t client)
{
    // Uses the handle the event carried rather than client_, so this never
    // touches shared state and never needs the lock - which is what keeps it
    // from deadlocking against a set_broker that is tearing the client down.
    ESP_LOGI(TAG, "Connected to broker");
    connected_ = true;

    cJSON* config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "name", "Neato D5");
    cJSON_AddStringToObject(config, "unique_id", VACUUM_ID);
    add_robot_device(config);
    cJSON_AddStringToObject(config, "command_topic", command_topic_.c_str());
    cJSON_AddStringToObject(config, "state_topic", state_topic_.c_str());
    cJSON_AddStringToObject(config, "json_attributes_topic", state_topic_.c_str());
    cJSON_AddStringToObject(config, "availability_topic", availability_topic_.c_str());
    cJSON_AddStringToObject(config, "payload_available", "online");
    cJSON_AddStringToObject(config, "payload_not_available", "offline");

    // Battery is an attribute; it is not a supported_features entry in HA's
    // current MQTT vacuum schema. Advertise only implemented robot actions.
    cJSON* features = cJSON_AddArrayToObject(config, "supported_features");
    for (auto feature : {"start", "stop", "return_home", "status", "clean_spot"}) {
        cJSON_AddItemToArray(features, cJSON_CreateString(feature));
    }

    char* json = cJSON_PrintUnformatted(config);
    if (json) {
        auto discovery_topic = "homeassistant/vacuum/" + std::string(VACUUM_ID) + "/config";
        esp_mqtt_client_publish(client, discovery_topic.c_str(), json, 0, 1, 1);
        cJSON_free(json);
    }
    cJSON_Delete(config);

    // Current HA versions expose vacuum battery as a separate sensor. Keep
    // battery_level in the vacuum attributes as well for existing dashboards.
    cJSON* battery = cJSON_CreateObject();
    cJSON_AddStringToObject(battery, "name", "Neato D5 Battery");
    cJSON_AddStringToObject(battery, "unique_id", "sim_vacuum_battery");
    add_robot_device(battery);
    cJSON_AddStringToObject(battery, "state_topic", state_topic_.c_str());
    cJSON_AddStringToObject(battery, "availability_topic", availability_topic_.c_str());
    cJSON_AddStringToObject(battery, "device_class", "battery");
    cJSON_AddStringToObject(battery, "state_class", "measurement");
    cJSON_AddStringToObject(battery, "unit_of_measurement", "%");
    cJSON_AddStringToObject(battery, "value_template", "{{ value_json.battery_level }}");
    char* battery_json = cJSON_PrintUnformatted(battery);
    if (battery_json) {
        esp_mqtt_client_publish(client, "homeassistant/sensor/sim_vacuum_battery/config", battery_json, 0, 1, 1);
        cJSON_free(battery_json);
    }
    cJSON_Delete(battery);

    esp_mqtt_client_publish(client, availability_topic_.c_str(), "offline", 0, 1, 1);
    esp_mqtt_client_subscribe(client, command_topic_.c_str(), 1);
}

void MqttClient::on_data(std::string_view topic, std::string_view data)
{
    // The topic was never checked, so anything the broker pushed on any topic
    // was executed as a command. A subscription is not a guarantee: a broker,
    // or anyone able to publish through one, decides what arrives here.
    if (topic != command_topic_) {
        ESP_LOGD(TAG, "Ignoring a message on '%.*s'", static_cast<int>(topic.size()), topic.data());
        return;
    }

    const char* action = robot_mqtt::command_action(data);
    if (!action) { ESP_LOGW(TAG, "Rejected unsupported MQTT robot command"); return; }
    if (!command_queue_ || xQueueSend(command_queue_, &action, 0) != pdTRUE)
        ESP_LOGW(TAG, "Robot command queue full; MQTT command dropped");
}

void MqttClient::publish_state()
{
    Lock lock(mutex_);
    if (!client_ || !connected_) return;

    const auto s = robot_.status();
    const bool available = s.available && s.age_seconds <= 5;
    esp_mqtt_client_publish(client_, availability_topic_.c_str(), available ? "online" : "offline", 0, 1, 1);
    if (!available) return;

    cJSON* state = cJSON_CreateObject();
    const char* activity = robot_mqtt::activity(s.ui_state, s.state, s.external_power,
                                               s.errors_available && s.error.code != 200);
    if (activity) cJSON_AddStringToObject(state, "state", activity);
    else cJSON_AddNullToObject(state, "state");
    cJSON_AddNumberToObject(state, "battery_level", s.battery_percent);
    cJSON_AddStringToObject(state, "robot_state", s.state);
    cJSON_AddStringToObject(state, "ui_state", s.ui_state);
    cJSON_AddBoolToObject(state, "errors_available", s.errors_available);
    if (s.errors_available) {
        cJSON_AddNumberToObject(state, "error_code", s.error.code);
        cJSON_AddStringToObject(state, "error", s.error.token);
        cJSON_AddStringToObject(state, "error_message", robot_response::diagnostic_message(s.error.code));
        cJSON_AddNumberToObject(state, "alert_code", s.alert.code);
        cJSON_AddStringToObject(state, "alert", s.alert.token);
    } else {
        cJSON_AddNullToObject(state, "error_code");
        cJSON_AddStringToObject(state, "error", "");
        cJSON_AddStringToObject(state, "error_message", "Error status unavailable");
        cJSON_AddNullToObject(state, "alert_code");
        cJSON_AddStringToObject(state, "alert", "");
    }

    char* json = cJSON_PrintUnformatted(state);
    if (json) {
        esp_mqtt_client_publish(client_, state_topic_.c_str(), json, 0, 0, 1);
        cJSON_free(json);
    }
    cJSON_Delete(state);
}

void MqttClient::event_handler(void* arg, esp_event_base_t, int32_t id, void* data)
{
    auto* self = static_cast<MqttClient*>(arg);
    auto* event = static_cast<esp_mqtt_event_handle_t>(data);

    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
        self->on_connected(event->client);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        self->connected_ = false;
        if (self->command_queue_) xQueueReset(self->command_queue_);
        break;
    case MQTT_EVENT_DATA:
        // A payload split across events carries a null topic on every part
        // after the first, which there is then no way to attribute.
        if (!event->retain && event->topic && event->topic_len > 0 && event->data && event->data_len > 0
            && event->current_data_offset == 0 && event->data_len == event->total_data_len) {
            self->on_data({event->topic, static_cast<size_t>(event->topic_len)},
                          {event->data, static_cast<size_t>(event->data_len)});
        }
        break;
    default:
        break;
    }
}

void MqttClient::publish_task(void* arg)
{
    auto* self = static_cast<MqttClient*>(arg);

    while (true) {
        const char* action = nullptr;
        if (xQueueReceive(self->command_queue_, &action, pdMS_TO_TICKS(PUBLISH_PERIOD_MS)) == pdTRUE) {
            const auto s = self->robot_.status();
            if (!self->connected_ || !s.available || s.age_seconds > 5)
                ESP_LOGW(TAG, "MQTT robot command dropped: robot or broker unavailable");
            else if (!self->robot_.command(action)) ESP_LOGW(TAG, "MQTT robot command failed: %s", action);
        }
        self->publish_state();
    }
}
