#include "mqtt.h"
#include "text_util.h"

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

}  // namespace

MqttClient::MqttClient(Vacuum& vacuum)
    : vacuum_(vacuum)
    , mutex_(xSemaphoreCreateMutex())
    , state_topic_(std::string(VACUUM_ID) + "/state")
    , command_topic_(std::string(VACUUM_ID) + "/command")
    , availability_topic_(std::string(VACUUM_ID) + "/availability")
{
    if (!mutex_) ESP_LOGE(TAG, "Could not create the client mutex");
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
        && xTaskCreate(publish_task, "mqtt_pub", 4096, this, 5, &publish_task_) != pdPASS) {
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
    cJSON_AddStringToObject(config, "name", "Sim Vacuum");
    cJSON_AddStringToObject(config, "unique_id", VACUUM_ID);
    cJSON_AddStringToObject(config, "command_topic", command_topic_.c_str());
    cJSON_AddStringToObject(config, "state_topic", state_topic_.c_str());
    cJSON_AddStringToObject(config, "json_attributes_topic", state_topic_.c_str());
    cJSON_AddStringToObject(config, "availability_topic", availability_topic_.c_str());
    cJSON_AddStringToObject(config, "payload_available", "online");
    cJSON_AddStringToObject(config, "payload_not_available", "offline");

    // Only what Vacuum::command actually accepts. "pause" was advertised here
    // and implemented nowhere, so pressing Pause in Home Assistant logged an
    // unknown command and did nothing.
    cJSON* features = cJSON_AddArrayToObject(config, "supported_features");
    for (auto feature : {"start", "stop", "return_home", "battery"}) {
        cJSON_AddItemToArray(features, cJSON_CreateString(feature));
    }

    char* json = cJSON_PrintUnformatted(config);
    if (json) {
        auto discovery_topic = "homeassistant/vacuum/" + std::string(VACUUM_ID) + "/config";
        esp_mqtt_client_publish(client, discovery_topic.c_str(), json, 0, 1, 1);
        cJSON_free(json);
    }
    cJSON_Delete(config);

    esp_mqtt_client_publish(client, availability_topic_.c_str(), "online", 0, 1, 1);
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

    if (!vacuum_.command(data)) {
        ESP_LOGW(TAG, "Unknown command: %.*s", static_cast<int>(data.size()), data.data());
    }
}

void MqttClient::publish_state()
{
    Lock lock(mutex_);
    if (!client_ || !connected_) return;

    auto s = vacuum_.status();

    cJSON* state = cJSON_CreateObject();
    cJSON_AddStringToObject(state, "state", Vacuum::state_to_string(s.state));
    cJSON_AddNumberToObject(state, "battery_level", s.battery_level);

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
        break;
    case MQTT_EVENT_DATA:
        // A payload split across events carries a null topic on every part
        // after the first, which there is then no way to attribute.
        if (event->topic && event->topic_len > 0) {
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
        self->publish_state();
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_PERIOD_MS));
    }
}
