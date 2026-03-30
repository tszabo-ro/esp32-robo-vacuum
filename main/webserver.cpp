#include "webserver.h"
#include "wifi.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include "cJSON.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_wifi.h"

static constexpr const char* TAG = "webserver";

// Embedded HTML file
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

vprintf_like_t WebServer::s_original_vprintf = nullptr;
WebServer* WebServer::s_instance = nullptr;

WebServer::WebServer(Vacuum& vacuum, MqttClient& mqtt)
    : vacuum_(vacuum)
    , mqtt_(mqtt)
    , clients_mutex_(xSemaphoreCreateMutex())
    , log_stream_(xStreamBufferCreate(4096, 1))
{
}

esp_err_t WebServer::start()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    struct Route {
        const char* uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t*);
        bool websocket;
    };

    Route routes[] = {
        {"/",             HTTP_GET,  handle_index,        false},
        {"/api/status",   HTTP_GET,  handle_get_status,   false},
        {"/api/wifi",     HTTP_POST, handle_post_wifi,    false},
        {"/api/mqtt",     HTTP_POST, handle_post_mqtt,    false},
        {"/api/command",  HTTP_POST, handle_post_command, false},
        {"/ws",           HTTP_GET,  handle_ws,           true},
    };

    for (auto& r : routes) {
        httpd_uri_t uri = {
            .uri = r.uri,
            .method = r.method,
            .handler = r.handler,
            .user_ctx = this,
            .is_websocket = r.websocket,
            .handle_ws_control_frames = r.websocket,
        };
        httpd_register_uri_handler(server_, &uri);
    }

    s_instance = this;
    install_log_hook();
    xTaskCreate(log_broadcast_task, "ws_log", 4096, this, 5, &log_task_);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}

void WebServer::stop()
{
    if (log_task_) {
        vTaskDelete(log_task_);
        log_task_ = nullptr;
    }
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    if (s_original_vprintf) {
        esp_log_set_vprintf(s_original_vprintf);
        s_original_vprintf = nullptr;
    }
    s_instance = nullptr;
}

// --- HTTP handlers ---

esp_err_t WebServer::handle_index(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

esp_err_t WebServer::handle_get_status(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);
    auto [state, battery] = self->vacuum_.status();

    wifi_ap_record_t ap = {};
    bool wifi_connected = wifi_is_connected();
    if (wifi_connected) esp_wifi_sta_get_ap_info(&ap);

    cJSON* root = cJSON_CreateObject();

    cJSON* wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_connected);
    if (wifi_connected)
        cJSON_AddStringToObject(wifi, "ssid", reinterpret_cast<const char*>(ap.ssid));

    cJSON* mqtt_obj = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt_obj, "connected", self->mqtt_.is_connected());
    auto& broker = self->mqtt_.broker_uri();
    if (!broker.empty())
        cJSON_AddStringToObject(mqtt_obj, "broker", broker.c_str());

    cJSON* vac = cJSON_AddObjectToObject(root, "vacuum");
    cJSON_AddStringToObject(vac, "state", Vacuum::state_to_string(state));
    cJSON_AddNumberToObject(vac, "battery", battery);

    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());

    char* json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ret;
}

esp_err_t WebServer::handle_post_wifi(httpd_req_t* req)
{
    auto body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    auto* ssid = cJSON_GetObjectItem(root, "ssid");
    auto* pass = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid/password");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_set_credentials(ssid->valuestring, pass->valuestring);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, err == ESP_OK ? R"({"ok":true})" : R"({"ok":false,"error":"connection failed"})");
}

esp_err_t WebServer::handle_post_mqtt(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);
    auto body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    auto* uri = cJSON_GetObjectItem(root, "uri");
    if (!cJSON_IsString(uri)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing uri");
        return ESP_FAIL;
    }

    esp_err_t err = self->mqtt_.set_broker(std::string(uri->valuestring));
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, err == ESP_OK ? R"({"ok":true})" : R"({"ok":false,"error":"connection failed"})");
}

esp_err_t WebServer::handle_post_command(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);
    auto body = read_body(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    auto* cmd = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    std::string_view command(cmd->valuestring);
    if (command == "start") self->vacuum_.start();
    else if (command == "stop") self->vacuum_.stop();
    else if (command == "return_to_base") self->vacuum_.return_to_base();
    else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown command");
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, R"({"ok":true})");
}

// --- WebSocket ---

esp_err_t WebServer::handle_ws(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);

    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket client connected: fd=%d", fd);
        self->add_ws_client(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;

    // Get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;
    if (frame.len == 0) return ESP_OK;

    std::string buf(frame.len, '\0');
    frame.payload = reinterpret_cast<uint8_t*>(buf.data());
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) return ret;

    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        cJSON* msg = cJSON_Parse(buf.c_str());
        if (msg) {
            auto* type = cJSON_GetObjectItem(msg, "type");
            if (cJSON_IsString(type) && std::string_view(type->valuestring) == "console_in") {
                auto* cmd = cJSON_GetObjectItem(msg, "cmd");
                if (cJSON_IsString(cmd)) {
                    int cmd_ret = 0;
                    esp_console_run(cmd->valuestring, &cmd_ret);

                    char resp[64];
                    snprintf(resp, sizeof(resp), R"({"type":"console_out","ret":%d})", cmd_ret);
                    httpd_ws_frame_t resp_frame = {};
                    resp_frame.type = HTTPD_WS_TYPE_TEXT;
                    resp_frame.payload = reinterpret_cast<uint8_t*>(resp);
                    resp_frame.len = strlen(resp);
                    httpd_ws_send_frame(req, &resp_frame);
                }
            }
            cJSON_Delete(msg);
        }
    } else if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket client disconnected: fd=%d", fd);
        self->remove_ws_client(fd);
    }

    return ESP_OK;
}

void WebServer::add_ws_client(int fd)
{
    xSemaphoreTake(clients_mutex_, portMAX_DELAY);
    ws_clients_.push_back(fd);
    xSemaphoreGive(clients_mutex_);
}

void WebServer::remove_ws_client(int fd)
{
    xSemaphoreTake(clients_mutex_, portMAX_DELAY);
    ws_clients_.erase(
        std::remove(ws_clients_.begin(), ws_clients_.end(), fd),
        ws_clients_.end());
    xSemaphoreGive(clients_mutex_);
}

void WebServer::broadcast_ws(const std::string& msg)
{
    xSemaphoreTake(clients_mutex_, portMAX_DELAY);

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(msg.data()));
    frame.len = msg.size();

    for (auto it = ws_clients_.begin(); it != ws_clients_.end();) {
        if (httpd_ws_get_fd_info(server_, *it) != HTTPD_WS_CLIENT_WEBSOCKET) {
            it = ws_clients_.erase(it);
        } else {
            httpd_ws_send_frame_async(server_, *it, &frame);
            ++it;
        }
    }

    xSemaphoreGive(clients_mutex_);
}

// --- Log capture ---

int WebServer::log_vprintf(const char* fmt, va_list args)
{
    // Always print to serial
    va_list args_copy;
    va_copy(args_copy, args);
    int ret = s_original_vprintf(fmt, args_copy);
    va_end(args_copy);

    if (!s_instance || !s_instance->log_stream_) return ret;

    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        size_t to_write = len < static_cast<int>(sizeof(buf)) ? len : sizeof(buf) - 1;
        if (xPortInIsrContext()) {
            BaseType_t woken = pdFALSE;
            xStreamBufferSendFromISR(s_instance->log_stream_, buf, to_write, &woken);
            if (woken) portYIELD_FROM_ISR();
        } else {
            xStreamBufferSend(s_instance->log_stream_, buf, to_write, 0);
        }
    }

    return ret;
}

void WebServer::install_log_hook()
{
    s_original_vprintf = esp_log_set_vprintf(log_vprintf);
}

void WebServer::log_broadcast_task(void* arg)
{
    auto* self = static_cast<WebServer*>(arg);
    char buf[512];

    while (true) {
        size_t received = xStreamBufferReceive(self->log_stream_, buf, sizeof(buf) - 1,
                                               pdMS_TO_TICKS(100));
        if (received == 0) continue;
        buf[received] = '\0';

        // Split on newlines and send each line
        char* line_start = buf;
        for (size_t i = 0; i < received; i++) {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                if (line_start[0] != '\0') {
                    cJSON* msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(msg, "type", "log");
                    cJSON_AddStringToObject(msg, "data", line_start);
                    char* json = cJSON_PrintUnformatted(msg);
                    self->broadcast_ws(json);
                    cJSON_free(json);
                    cJSON_Delete(msg);
                }
                line_start = &buf[i + 1];
            }
        }
        // Handle remaining text without newline
        if (line_start < buf + received && line_start[0] != '\0') {
            cJSON* msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "log");
            cJSON_AddStringToObject(msg, "data", line_start);
            char* json = cJSON_PrintUnformatted(msg);
            self->broadcast_ws(json);
            cJSON_free(json);
            cJSON_Delete(msg);
        }
    }
}

// --- Helpers ---

std::string WebServer::read_body(httpd_req_t* req)
{
    std::string body(req->content_len, '\0');
    int offset = 0;
    while (offset < static_cast<int>(req->content_len)) {
        int received = httpd_req_recv(req, body.data() + offset, req->content_len - offset);
        if (received <= 0) break;
        offset += received;
    }
    return body;
}
