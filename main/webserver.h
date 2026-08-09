#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "vacuum.h"
#include "mqtt.h"
#include "ota.h"
#include "serial.h"

class WebServer {
public:
    WebServer(Vacuum& vacuum, MqttClient& mqtt, SerialPort& serial, OtaUpdater& ota);
    esp_err_t start();
    void stop();

private:
    static esp_err_t handle_index(httpd_req_t* req);
    static esp_err_t handle_not_found(httpd_req_t* req, httpd_err_code_t error);
    static esp_err_t handle_get_auth(httpd_req_t* req);
    static esp_err_t handle_post_setup(httpd_req_t* req);
    static esp_err_t handle_post_login(httpd_req_t* req);
    static esp_err_t handle_post_logout(httpd_req_t* req);
    static esp_err_t handle_get_status(httpd_req_t* req);
    static esp_err_t handle_post_wifi(httpd_req_t* req);
    static esp_err_t handle_post_mqtt(httpd_req_t* req);
    static esp_err_t handle_post_serial(httpd_req_t* req);
    static esp_err_t handle_post_ap(httpd_req_t* req);
    static esp_err_t handle_post_command(httpd_req_t* req);
    static esp_err_t handle_post_ota(httpd_req_t* req);
    static esp_err_t handle_ws(httpd_req_t* req);

    // --- Access control ---
    //
    // Every handler starts with one of these. They answer the request
    // themselves on refusal, so a handler that forgets to check cannot fall
    // through to the work: it either returns early or never sees a body.

    // True when the request may proceed. On false the reply has already been
    // sent and the handler must stop - returning ESP_OK, so the server drains
    // the request body and does not reset the connection.
    //
    // Deliberately not an esp_err_t: returning one is what let a refusal be
    // read as permission, because the value that came back from sending the
    // rejection was ESP_OK. `origin_required` is false only for safe reads,
    // which browsers send without an Origin header.
    static bool allow_session(httpd_req_t* req, bool origin_required);

    // The same test without a reply. The WebSocket handler needs this: the
    // upgrade response has already been sent by the time it runs, so writing an
    // HTTP error there would put stray bytes into the frame stream.
    static bool session_valid(httpd_req_t* req, bool origin_required);

    // Session required, plus Content-Type: application/json. The content type
    // is what forces a cross-origin POST through a preflight rather than being
    // dispatched as a "simple request" the browser will happily send.
    static bool allow_session_json(httpd_req_t* req);

    // For the unauthenticated POSTs (login, first-run setup): no session, but
    // the same JSON and Origin requirements.
    static bool allow_json(httpd_req_t* req);

    // A present Origin must always match Host. Whether an absent one is
    // acceptable depends on the request: see the definition.
    static bool origin_ok(httpd_req_t* req, bool required);
    static bool content_type_is_json(httpd_req_t* req);

    // Reads the session token from the cookie, or from Authorization: Bearer
    // for scripts. Empty when neither is present.
    static std::string session_token(httpd_req_t* req, bool& from_bearer);

    // Every reply is JSON, including the failures: the server's own error
    // pages are HTML, and a browser that asked for JSON and got markup reports
    // a parse error instead of the reason it was refused.
    static esp_err_t send_json(httpd_req_t* req, const char* json);
    static esp_err_t send_status(httpd_req_t* req, const char* status, const char* message);
    static esp_err_t bad_request(httpd_req_t* req, const char* message);

    // Sends `status` and returns false, for the guards to return directly.
    static bool refuse(httpd_req_t* req, const char* status, const char* message);

    // What a handler returns after a guard has already answered. See the
    // definition: whether the connection is drained or closed depends on how
    // big the client said its body was.
    static esp_err_t refusal_result(httpd_req_t* req);
    static void set_common_headers(httpd_req_t* req);

    static void session_closed(httpd_handle_t handle, int sockfd);

    static int log_vprintf(const char* fmt, va_list args);
    void install_log_hook();

    void add_ws_client(int fd);
    void remove_ws_client(int fd);
    void broadcast_ws(const std::string& msg);
    void broadcast_typed(const char* type, const char* data);

    static void log_broadcast_task(void* arg);
    static void serial_broadcast_task(void* arg);

    // Reads the whole body, or answers the request and returns nothing. A
    // std::optional rather than an empty string because "too large" and
    // "empty" need different handling: the first has already been replied to,
    // and replying twice on one request corrupts the connection.
    static std::optional<std::string> read_body(httpd_req_t* req);

    // Renders bytes from the UART as something a JSON string can carry: raw
    // input may contain NULs or invalid UTF-8, which would truncate or corrupt
    // the frame. Non-printables other than newline and tab become \xNN.
    static std::string escape_serial(const char* data, size_t len);

    // Bodies are configuration blobs, not uploads. The cap is applied before
    // anything is allocated, because Content-Length is attacker-controlled and
    // an allocation failure here is a panic reboot on the only way in.
    static constexpr size_t MAX_BODY_BYTES = 2048;
    static constexpr size_t MAX_WS_FRAME_BYTES = 4096;

    // Matches the server's socket budget: past this, a client is not being
    // served anyway, and the list is what an attacker would otherwise grow.
    static constexpr size_t MAX_WS_CLIENTS = 7;

    Vacuum& vacuum_;
    MqttClient& mqtt_;
    SerialPort& serial_;
    OtaUpdater& ota_;
    httpd_handle_t server_ = nullptr;

    SemaphoreHandle_t clients_mutex_;
    std::vector<int> ws_clients_;

    StreamBufferHandle_t log_stream_;
    TaskHandle_t log_task_ = nullptr;
    TaskHandle_t serial_task_ = nullptr;

    static vprintf_like_t s_original_vprintf;
    static WebServer* s_instance;
};
