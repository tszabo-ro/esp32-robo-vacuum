#pragma once

#include <string>
#include <vector>
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "vacuum.h"
#include "mqtt.h"
#include "serial.h"

class WebServer {
public:
    WebServer(Vacuum& vacuum, MqttClient& mqtt, SerialPort& serial);
    esp_err_t start();
    void stop();

private:
    static esp_err_t handle_index(httpd_req_t* req);
    static esp_err_t handle_get_status(httpd_req_t* req);
    static esp_err_t handle_post_wifi(httpd_req_t* req);
    static esp_err_t handle_post_mqtt(httpd_req_t* req);
    static esp_err_t handle_post_serial(httpd_req_t* req);
    static esp_err_t handle_post_command(httpd_req_t* req);
    static esp_err_t handle_ws(httpd_req_t* req);

    static int log_vprintf(const char* fmt, va_list args);
    void install_log_hook();

    void add_ws_client(int fd);
    void remove_ws_client(int fd);
    void broadcast_ws(const std::string& msg);

    static void log_broadcast_task(void* arg);
    static void serial_broadcast_task(void* arg);
    static std::string read_body(httpd_req_t* req);

    // Renders bytes from the UART as something a JSON string can carry: raw
    // input may contain NULs or invalid UTF-8, which would truncate or corrupt
    // the frame. Non-printables other than newline and tab become \xNN.
    static std::string escape_serial(const char* data, size_t len);

    Vacuum& vacuum_;
    MqttClient& mqtt_;
    SerialPort& serial_;
    httpd_handle_t server_ = nullptr;

    SemaphoreHandle_t clients_mutex_;
    std::vector<int> ws_clients_;

    StreamBufferHandle_t log_stream_;
    TaskHandle_t log_task_ = nullptr;
    TaskHandle_t serial_task_ = nullptr;

    static vprintf_like_t s_original_vprintf;
    static WebServer* s_instance;
};
