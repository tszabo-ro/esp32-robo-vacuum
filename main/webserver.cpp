#include "webserver.h"
#include "auth.h"
#include "wifi.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"

static constexpr const char* TAG = "webserver";

// Embedded HTML file
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

vprintf_like_t WebServer::s_original_vprintf = nullptr;
WebServer* WebServer::s_instance = nullptr;

namespace {

// Field limits, applied where the value enters the device rather than where it
// is stored. Every one of these is the length the underlying API can actually
// take: the driver's SSID field is 32 bytes, WPA2 caps a passphrase at 63, and
// NVS refuses a string past 4000. Checking here means a storage layer never
// has to fail on a value the network chose.
constexpr size_t MAX_SSID = 32;
constexpr size_t MAX_WIFI_PASSWORD = 63;
constexpr size_t MAX_BROKER_URI = 128;
constexpr size_t MAX_OTA_URL = 256;
constexpr size_t MAX_COMMAND = 32;
constexpr size_t MAX_SERIAL_TX = 512;

// Two timeouts is ~6 seconds at the receive timeout configured below. The
// server runs one task for all sessions, so a body that has not arrived by then
// is costing every other client, not just its own.
constexpr int MAX_RECV_TIMEOUTS = 2;

constexpr size_t MAX_HEADER_VALUE = 128;

// The URLs each platform fetches to decide whether a network really reaches the
// internet, and the exact answer each one wants to hear.
struct ConnectivityProbe {
    const char* path;
    const char* status;
    const char* content_type;
    const char* body;  // nullptr sends no body, for the 204 probes
};

constexpr ConnectivityProbe PROBES[] = {
    {"/hotspot-detect.html", "200 OK", "text/html",
     "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},
    {"/library/test/success.html", "200 OK", "text/html",
     "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},
    {"/generate_204", "204 No Content", "text/plain", nullptr},
    {"/gen_204", "204 No Content", "text/plain", nullptr},
    {"/connecttest.txt", "200 OK", "text/plain", "Microsoft Connect Test"},
    {"/ncsi.txt", "200 OK", "text/plain", "Microsoft NCSI"},
    {"/success.txt", "200 OK", "text/plain", "success\n"},
};

// Whether to hold a joining client in its sign-in window.
//
// Only while the device still needs setting up. That is the one moment the
// window earns its place: a freshly flashed device has to be found and given a
// password, and the window opening by itself is what makes that happen without
// anyone being told an address.
//
// Once a password exists there is nothing more to do in a small window bolted
// to the top of the screen, and holding a client there is actively harmful - it
// drags every request back into the window and makes an ordinary browser
// useless on the network. So from then on the probe is answered the way it
// wants, the client marks the network usable, and the window closes.
bool portal_should_capture()
{
    return !Auth::configured();
}

// Owns a parsed document so a handler's error paths cannot leak it. Every
// early return in these handlers used to need its own cJSON_Delete.
class JsonDoc {
public:
    explicit JsonDoc(cJSON* root) : root_(root) {}
    ~JsonDoc() { cJSON_Delete(root_); }
    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;

    cJSON* get() const { return root_; }
    explicit operator bool() const { return root_ != nullptr; }

private:
    cJSON* root_;
};

class Lock {
public:
    explicit Lock(SemaphoreHandle_t m) : m_(m) { if (m_) xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { if (m_) xSemaphoreGive(m_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t m_;
};

// Returns the field only when it is a string no longer than max_len. A too-long
// value is treated as absent so the caller reports it as a bad request rather
// than passing it on to a layer that would truncate it silently.
const char* string_field(cJSON* root, const char* key, size_t max_len)
{
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) return nullptr;
    if (strlen(item->valuestring) > max_len) return nullptr;
    return item->valuestring;
}

bool read_header(httpd_req_t* req, const char* field, std::string& out)
{
    const size_t len = httpd_req_get_hdr_value_len(req, field);
    if (len == 0 || len > MAX_HEADER_VALUE) return false;

    out.assign(len, '\0');
    if (httpd_req_get_hdr_value_str(req, field, out.data(), len + 1) != ESP_OK) return false;
    return true;
}

}  // namespace

WebServer::WebServer(Vacuum& vacuum, MqttClient& mqtt, SerialPort& serial, OtaUpdater& ota)
    : vacuum_(vacuum)
    , mqtt_(mqtt)
    , serial_(serial)
    , ota_(ota)
    , clients_mutex_(xSemaphoreCreateMutex())
    , log_stream_(xStreamBufferCreate(4096, 1))
{
    if (!clients_mutex_) ESP_LOGE(TAG, "Could not create the client mutex");
    if (!log_stream_) ESP_LOGE(TAG, "Could not create the log stream buffer");
}

esp_err_t WebServer::start()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    config.close_fn = session_closed;
    // Logging in runs PBKDF2, which needs more room than a JSON handler.
    config.stack_size = 6144;

    // Down from five seconds. Bodies here are configuration blobs of a few
    // hundred bytes, so three is generous for anything legitimate, and it is
    // the only lever on a stall no handler can reach: a request that announces
    // a body and then sends nothing is held in the server's parser, which loops
    // reading until the request is complete, before any handler is called. That
    // costs the whole timeout on the one task serving every session. The parser
    // cannot be pre-empted from here, so the cost is shortened rather than
    // removed.
    config.recv_wait_timeout = 3;

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

    // Authentication is enforced inside each handler rather than by a flag
    // here, so that adding a route cannot accidentally inherit "public" from a
    // table column nobody looked at. The three public ones are public because
    // they have to be: the page has to render before there is a session, and
    // the login and first-run endpoints are how a session is obtained.
    Route routes[] = {
        {"/",             HTTP_GET,  handle_index,        false},  // public
        {"/api/auth",     HTTP_GET,  handle_get_auth,     false},  // public
        {"/api/setup",    HTTP_POST, handle_post_setup,   false},  // public until configured
        {"/api/login",    HTTP_POST, handle_post_login,   false},  // public
        {"/api/logout",   HTTP_POST, handle_post_logout,  false},
        {"/api/status",   HTTP_GET,  handle_get_status,   false},
        {"/api/wifi",     HTTP_POST, handle_post_wifi,    false},
        {"/api/mqtt",     HTTP_POST, handle_post_mqtt,    false},
        {"/api/serial",   HTTP_POST, handle_post_serial,  false},
        {"/api/ap",       HTTP_POST, handle_post_ap,      false},
        {"/api/command",  HTTP_POST, handle_post_command, false},
        {"/api/ota",      HTTP_POST, handle_post_ota,     false},
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
            .supported_subprotocol = nullptr,
        };
        // A silently unregistered route is a 404 with no log line, which reads
        // as a firmware bug from the browser side.
        esp_err_t reg = httpd_register_uri_handler(server_, &uri);
        if (reg != ESP_OK) {
            ESP_LOGE(TAG, "Could not register %s: %s", r.uri, esp_err_to_name(reg));
        }
    }

    // Sends anything unrecognised to the interface instead of a 404. On the
    // rescue network this is what satisfies a client's captive-portal probe:
    // DNS points every name here, the probe asks for some vendor URL, and this
    // redirect turns it into the setup page - which the client then opens by
    // itself rather than reporting a network with no internet and leaving.
    esp_err_t not_found = httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND,
                                                     handle_not_found);
    if (not_found != ESP_OK) {
        ESP_LOGW(TAG, "Could not register the redirect handler: %s", esp_err_to_name(not_found));
    }

    s_instance = this;
    install_log_hook();
    xTaskCreate(log_broadcast_task, "ws_log", 4096, this, 5, &log_task_);
    xTaskCreate(serial_broadcast_task, "ws_serial", 4096, this, 5, &serial_task_);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}

void WebServer::stop()
{
    if (log_task_) {
        vTaskDelete(log_task_);
        log_task_ = nullptr;
    }
    if (serial_task_) {
        vTaskDelete(serial_task_);
        serial_task_ = nullptr;
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

// Runs when a session ends, however it ends. Relying on a CLOSE frame instead
// left dead descriptors in the list: a client that just drops its TCP
// connection never sends one, and the number is then recycled for the next
// client, which then appears in the list twice.
void WebServer::session_closed(httpd_handle_t, int sockfd)
{
    if (s_instance) s_instance->remove_ws_client(sockfd);
    // The server hands ownership of the descriptor to this callback, so it will
    // not be closed anywhere else.
    close(sockfd);
}

// --- Responses ---

void WebServer::set_common_headers(httpd_req_t* req)
{
    // Configuration and status are per-session and must not be cached by an
    // intermediary or replayed from disk after a logout.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}

esp_err_t WebServer::send_json(httpd_req_t* req, const char* json)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

esp_err_t WebServer::send_status(httpd_req_t* req, const char* status, const char* message)
{
    set_common_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    char* json = cJSON_PrintUnformatted(root);
    esp_err_t ret = httpd_resp_sendstr(req, json ? json : R"({"ok":false})");
    cJSON_free(json);
    cJSON_Delete(root);
    return ret;
}

esp_err_t WebServer::bad_request(httpd_req_t* req, const char* message)
{
    return send_status(req, "400 Bad Request", message);
}

// --- Access control ---

bool WebServer::origin_ok(httpd_req_t* req, bool required)
{
    std::string origin;
    if (!read_header(req, "Origin", origin)) {
        // Browsers do not send Origin on a same-origin GET, so demanding it
        // everywhere would reject the page's own status polling. Not sending it
        // is only acceptable for a request that changes nothing: a hostile page
        // can still cause such a GET, but with no CORS headers in the reply it
        // cannot read the answer, and there is no side effect to cause.
        //
        // Every method that does change something - and the WebSocket upgrade,
        // which the same-origin policy does not cover at all - passes required.
        // Browsers always attach Origin to those.
        return !required;
    }

    std::string host;
    if (!read_header(req, "Host", host)) return false;

    // Origin is "scheme://authority"; Host is the authority alone. Only plain
    // HTTP is served, so anything else did not come from this device's page.
    std::string_view view(origin);
    constexpr std::string_view prefix = "http://";
    if (view.substr(0, prefix.size()) != prefix) return false;

    return view.substr(prefix.size()) == host;
}

std::string WebServer::session_token(httpd_req_t* req, bool& from_bearer)
{
    from_bearer = false;

    std::string header;
    if (read_header(req, "Authorization", header)) {
        constexpr std::string_view bearer = "Bearer ";
        if (std::string_view(header).substr(0, bearer.size()) == bearer) {
            from_bearer = true;
            return header.substr(bearer.size());
        }
    }

    char cookie[Auth::TOKEN_CHARS + 1] = {};
    size_t len = sizeof(cookie);
    if (httpd_req_get_cookie_val(req, "sid", cookie, &len) == ESP_OK) {
        return std::string(cookie);
    }
    return {};
}

bool WebServer::session_valid(httpd_req_t* req, bool origin_required)
{
    if (!Auth::configured()) return false;

    bool from_bearer = false;
    const std::string token = session_token(req, from_bearer);
    if (token.empty()) return false;

    // A cookie is attached by the browser to any request, including one a
    // hostile page made and including a WebSocket upgrade. So a cookie only
    // counts when the request also says it came from this device's own page. A
    // bearer token needs no such check: a cross-origin form cannot set that
    // header, and a fetch that tries is stopped by the preflight.
    if (!from_bearer && !origin_ok(req, origin_required)) return false;

    return Auth::validate_session(token);
}

// The guards answer with false and never with an esp_err_t.
//
// They used to return send_status's own result, which is ESP_OK whenever the
// rejection was delivered successfully - so every caller's `if (err != ESP_OK)`
// read a refusal as permission, and the handler did the work anyway after
// saying no.
//
// The obvious repair, returning ESP_FAIL, was worse in a quieter way. httpd
// treats a failed handler as a dead session and closes it without draining the
// request body, so a refused POST is answered and then reset - and a client
// that gets an RST with the response still in flight may never see why it was
// refused. A bool cannot be mistaken for either, and lets the handler return
// ESP_OK so the body is drained and the connection stays usable.
bool WebServer::refuse(httpd_req_t* req, const char* status, const char* message)
{
    send_status(req, status, message);
    return false;
}

esp_err_t WebServer::refusal_result(httpd_req_t* req)
{
    // What a handler returns once a guard has answered for it.
    //
    // ESP_OK lets the server drain the body of the request it just refused,
    // which keeps the connection clean and makes sure the client actually
    // receives the reason. That is right for a body the size of a
    // configuration blob.
    //
    // It is badly wrong for a body the client merely *claimed* was enormous.
    // Draining means waiting for megabytes that are not coming, which parks the
    // server's single task for the whole receive timeout and ends in the
    // server's own 408 - an unauthenticated request costing seconds of the only
    // management interface, repeatable at will. Past the cap the connection is
    // closed instead: the answer has already gone out, and nothing about the
    // rest of that request is worth waiting for.
    return req->content_len > MAX_BODY_BYTES ? ESP_FAIL : ESP_OK;
}

bool WebServer::allow_session(httpd_req_t* req, bool origin_required)
{
    // Split from session_valid so the reasons can differ in the reply. The
    // WebSocket path cannot use this one: by the time a handler runs there, the
    // upgrade response has already gone out, and a second HTTP response would
    // be written into the socket as frame data.
    if (!Auth::configured()) {
        return refuse(req, "403 Forbidden", "setup required");
    }

    bool from_bearer = false;
    const std::string token = session_token(req, from_bearer);
    if (token.empty()) {
        return refuse(req, "401 Unauthorized", "not logged in");
    }
    if (!from_bearer && !origin_ok(req, origin_required)) {
        return refuse(req, "403 Forbidden", "bad origin");
    }
    if (!Auth::validate_session(token)) {
        return refuse(req, "401 Unauthorized", "session expired");
    }
    return true;
}

bool WebServer::content_type_is_json(httpd_req_t* req)
{
    std::string type;
    if (!read_header(req, "Content-Type", type)) return false;

    // "application/json" optionally followed by parameters, e.g. a charset.
    std::string_view view(type);
    constexpr std::string_view json = "application/json";
    if (view.substr(0, json.size()) != json) return false;
    return view.size() == json.size() || view[json.size()] == ';';
}

bool WebServer::allow_json(httpd_req_t* req)
{
    // Without this, a cross-origin POST qualifies as a "simple request" and is
    // dispatched with no preflight - the response is opaque to the attacker,
    // but the side effect has already happened.
    if (!content_type_is_json(req)) {
        return refuse(req, "415 Unsupported Media Type", "expected application/json");
    }
    // Required, not merely checked when present: a browser always sends Origin
    // on a POST, so an absent one on a state-changing request did not come from
    // a browser page - and the two unauthenticated POSTs this guards, login and
    // first-run setup, are exactly the ones with no session to lean on.
    if (!origin_ok(req, true)) {
        return refuse(req, "403 Forbidden", "bad origin");
    }
    return true;
}

bool WebServer::allow_session_json(httpd_req_t* req)
{
    if (!content_type_is_json(req)) {
        return refuse(req, "415 Unsupported Media Type", "expected application/json");
    }
    // Not allow_json: that one demands an Origin outright, which is right for
    // the two endpoints with no session behind them, but would reject a script
    // holding a bearer token - and a bearer token cannot be replayed by a
    // hostile page in the first place, so it does not need the check.
    return allow_session(req, true);
}

// --- HTTP handlers ---

esp_err_t WebServer::handle_index(httpd_req_t* req)
{
    // The page is inline script and inline style, so 'unsafe-inline' is
    // unavoidable without rewriting it. What the policy still buys is the part
    // that matters here: no script may be loaded from anywhere else, and
    // nothing on the page may talk to any host but this one, so a value that
    // did get injected into the DOM has nowhere to send what it reads.
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

esp_err_t WebServer::handle_not_found(httpd_req_t* req, httpd_err_code_t)
{
    std::string_view path(req->uri);
    const size_t query = path.find('?');
    if (query != std::string_view::npos) path = path.substr(0, query);

    if (!portal_should_capture()) {
        for (const auto& probe : PROBES) {
            if (path != probe.path) continue;

            httpd_resp_set_status(req, probe.status);
            httpd_resp_set_type(req, probe.content_type);
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_send(req, probe.body, probe.body ? HTTPD_RESP_USE_STRLEN : 0);
            return ESP_OK;
        }
    }

    // Relative, so it works whatever name the client used to get here - the
    // device's address on the rescue network, its address on the LAN, or the
    // vendor hostname a captive-portal probe asked for.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><a href=\"/\">Neato controller</a></body></html>");
    return ESP_OK;
}

// Public on purpose: the page needs to know which of the three screens to show
// - first-run setup, login, or the interface - before it has a session. It
// reports no device state, only which of those applies.
esp_err_t WebServer::handle_get_auth(httpd_req_t* req)
{
    const bool authenticated = session_valid(req, false);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "setup_required", !Auth::configured());
    cJSON_AddBoolToObject(root, "authenticated", authenticated);

    // The page is told where it lives rather than carrying an address of its
    // own. A literal baked into the HTML is one more copy to drift when the
    // access point moves, and it is wrong outright on the LAN.
    cJSON_AddStringToObject(root, "setup_hostname", WIFI_SETUP_HOSTNAME);
    const std::string ap_address = wifi_ap_address();
    if (!ap_address.empty()) cJSON_AddStringToObject(root, "setup_address", ap_address.c_str());

    char* json = cJSON_PrintUnformatted(root);
    esp_err_t ret = send_json(req, json ? json
                                        : R"({"setup_required":true,"authenticated":false})");
    cJSON_free(json);
    cJSON_Delete(root);
    return ret;
}

static void set_session_cookie(httpd_req_t* req, const std::string& token)
{
    // HttpOnly keeps the token out of reach of any script that does get onto
    // the page. SameSite=Strict is the second line under the Origin check: the
    // browser will not attach this cookie to a request another site started.
    // No Secure flag, because the device serves plain HTTP - setting it would
    // stop the cookie being sent at all.
    //
    // Static, not a local: httpd_resp_set_hdr stores the pointer rather than
    // copying, and the value has to still be there when the response is sent.
    // Safe because the server runs one task, so only one response is ever being
    // built at a time.
    static char header[128];
    snprintf(header, sizeof(header),
             "sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200", token.c_str());
    httpd_resp_set_hdr(req, "Set-Cookie", header);
}

esp_err_t WebServer::handle_post_setup(httpd_req_t* req)
{
    // Only reachable once. After that this is an ordinary password change,
    // which belongs behind a session.
    if (Auth::configured()) {
        return send_status(req, "409 Conflict", "already set up");
    }

    if (!allow_json(req)) return refusal_result(req);

    auto body = read_body(req);
    if (!body) return refusal_result(req);  // read_body has already answered

    JsonDoc doc(cJSON_Parse(body->c_str()));
    if (!doc) return bad_request(req, "Invalid JSON");

    const char* password = string_field(doc.get(), "password", Auth::MAX_PASSWORD);
    const char* ap_password = string_field(doc.get(), "ap_password", MAX_WIFI_PASSWORD);
    if (!password || !ap_password) {
        return bad_request(req, "Missing password/ap_password");
    }
    if (strlen(password) < Auth::MIN_PASSWORD) {
        return bad_request(req, "Password must be at least 8 characters");
    }

    // The rescue network's default passphrase is printed in the repository, and
    // its SSID is derived from the MAC, so a device left on the default is
    // findable and joinable by anyone in range. Setup is the one moment the
    // owner is guaranteed to be present, so it is where that gets fixed.
    if (wifi_is_default_ap_password(ap_password)) {
        return bad_request(req, "Choose an access point password other than the default");
    }
    esp_err_t err = wifi_set_ap_password(ap_password);
    if (err != ESP_OK) {
        return bad_request(req, err == ESP_ERR_INVALID_ARG
            ? "Access point password must be 8 to 63 characters"
            : "Could not store the access point password");
    }

    err = Auth::set_password(password);
    if (err != ESP_OK) {
        return send_status(req, "500 Internal Server Error", "Could not store the password");
    }

    const std::string token = Auth::create_session();
    if (token.empty()) {
        return send_status(req, "503 Service Unavailable", "No session slot free");
    }
    set_session_cookie(req, token);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "token", token.c_str());
    char* json = cJSON_PrintUnformatted(root);
    esp_err_t ret = send_json(req, json ? json : R"({"ok":true})");
    cJSON_free(json);
    cJSON_Delete(root);
    return ret;
}

esp_err_t WebServer::handle_post_login(httpd_req_t* req)
{
    if (!Auth::configured()) {
        return send_status(req, "403 Forbidden", "setup required");
    }

    if (!allow_json(req)) return refusal_result(req);

    // Throttled rather than locked out permanently: this interface is the only
    // way into a sealed device, so a permanent lockout would be self-inflicted.
    if (Auth::throttled()) {
        return send_status(req, "429 Too Many Requests", "Too many attempts, wait a moment");
    }

    auto body = read_body(req);
    if (!body) return refusal_result(req);

    JsonDoc doc(cJSON_Parse(body->c_str()));
    if (!doc) return bad_request(req, "Invalid JSON");

    const char* password = string_field(doc.get(), "password", Auth::MAX_PASSWORD);
    if (!password) return bad_request(req, "Missing password");

    if (!Auth::verify_password(password)) {
        Auth::note_failure();
        ESP_LOGW(TAG, "Failed login attempt");
        return send_status(req, "401 Unauthorized", "Incorrect password");
    }
    Auth::note_success();

    const std::string token = Auth::create_session();
    if (token.empty()) {
        return send_status(req, "503 Service Unavailable", "No session slot free");
    }
    set_session_cookie(req, token);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "token", token.c_str());
    char* json = cJSON_PrintUnformatted(root);
    esp_err_t ret = send_json(req, json ? json : R"({"ok":true})");
    cJSON_free(json);
    cJSON_Delete(root);
    return ret;
}

esp_err_t WebServer::handle_post_logout(httpd_req_t* req)
{
    if (!allow_session_json(req)) return refusal_result(req);

    bool from_bearer = false;
    Auth::destroy_session(session_token(req, from_bearer));

    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return send_json(req, R"({"ok":true})");
}

esp_err_t WebServer::handle_get_status(httpd_req_t* req)
{
    // Origin not demanded: this is a read with no side effect, the page polls
    // it every three seconds, and a browser omits Origin on a same-origin GET.
    // A cross-origin page can cause the request but cannot read the reply.
    if (!allow_session(req, false)) return refusal_result(req);

    auto* self = static_cast<WebServer*>(req->user_ctx);
    auto [state, battery] = self->vacuum_.status();

    wifi_ap_record_t ap = {};
    bool wifi_connected = wifi_is_connected();
    if (wifi_connected && esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ap = {};
    }

    cJSON* root = cJSON_CreateObject();

    cJSON* wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_connected);
    // The LED knows more than this endpoint used to: "associated but no lease"
    // and "trying" look identical through a bool, and they have different
    // causes. Reported here too, so a browser can say what the flashes mean.
    cJSON_AddStringToObject(wifi, "state", wifi_state_to_string(wifi_state()));
    cJSON_AddNumberToObject(wifi, "rssi", wifi_rssi());
    if (wifi_connected)
        cJSON_AddStringToObject(wifi, "ssid", reinterpret_cast<const char*>(ap.ssid));

    // Surfaced so the fallback network's name can be noted down while the
    // device is still reachable, rather than guessed at when it is not.
    cJSON* apo = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddBoolToObject(apo, "active", wifi_ap_active());
    auto ap_ssid = wifi_ap_ssid();
    if (!ap_ssid.empty()) cJSON_AddStringToObject(apo, "ssid", ap_ssid.c_str());
    // An NVS erase - which nvs_flash_init() performs on its own when it finds
    // no free pages - silently puts the published default back. Surfaced so
    // that shows up in the interface instead of only in a boot log nobody
    // reads on a sealed robot.
    cJSON_AddBoolToObject(apo, "password_is_default", wifi_ap_password_is_default());

    cJSON* mqtt_obj = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt_obj, "connected", self->mqtt_.is_connected());
    // By value, not by reference: init() reassigns this member from another
    // task, and a reference into it would be freed mid-response.
    const std::string broker = self->mqtt_.broker_uri();
    if (!broker.empty())
        cJSON_AddStringToObject(mqtt_obj, "broker", MqttClient::redact_uri(broker).c_str());

    cJSON* ser = cJSON_AddObjectToObject(root, "serial");
    cJSON_AddNumberToObject(ser, "baud", self->serial_.baud());
    cJSON_AddBoolToObject(ser, "started", self->serial_.started());

    cJSON* vac = cJSON_AddObjectToObject(root, "vacuum");
    cJSON_AddStringToObject(vac, "state", Vacuum::state_to_string(state));
    cJSON_AddNumberToObject(vac, "battery", battery);

    // Reported over HTTP because the version is otherwise only visible in the
    // boot log. Once the robot is closed and updates arrive over the air, this
    // is the only way to confirm what is actually running.
    const esp_app_desc_t* app = esp_app_get_description();
    const esp_partition_t* running = esp_ota_get_running_partition();
    cJSON* fw = cJSON_AddObjectToObject(root, "firmware");
    cJSON_AddStringToObject(fw, "version", app ? app->version : "unknown");
    if (running) cJSON_AddStringToObject(fw, "partition", running->label);

    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());

    char* json = cJSON_PrintUnformatted(root);
    esp_err_t ret = send_json(req, json ? json : R"({})");
    cJSON_free(json);
    cJSON_Delete(root);
    return ret;
}

esp_err_t WebServer::handle_post_wifi(httpd_req_t* req)
{
    if (!allow_session_json(req)) return refusal_result(req);

    auto body = read_body(req);
    if (!body) return refusal_result(req);

    JsonDoc doc(cJSON_Parse(body->c_str()));
    if (!doc) return bad_request(req, "Invalid JSON");

    const char* ssid = string_field(doc.get(), "ssid", MAX_SSID);
    const char* pass = string_field(doc.get(), "password", MAX_WIFI_PASSWORD);
    if (!ssid || !pass) {
        return bad_request(req, "SSID must be 1-32 characters and the password at most 63");
    }
    if (ssid[0] == '\0') return bad_request(req, "SSID must not be empty");

    esp_err_t err = wifi_set_credentials(ssid, pass);
    if (err != ESP_OK) {
        return send_status(req, "500 Internal Server Error", "Could not store the credentials");
    }

    // Answers immediately and reports the outcome through /api/status, because
    // the connection attempt now runs elsewhere. Waiting for it here parked the
    // server's only task for thirty seconds - and if the request had arrived
    // over the station interface, dropping that interface destroyed the socket
    // the answer was going to travel on.
    return send_json(req, R"({"ok":true,"status":"connecting"})");
}

esp_err_t WebServer::handle_post_mqtt(httpd_req_t* req)
{
    if (!allow_session_json(req)) return refusal_result(req);

    auto* self = static_cast<WebServer*>(req->user_ctx);
    auto body = read_body(req);
    if (!body) return refusal_result(req);

    JsonDoc doc(cJSON_Parse(body->c_str()));
    if (!doc) return bad_request(req, "Invalid JSON");

    const char* uri = string_field(doc.get(), "uri", MAX_BROKER_URI);
    if (!uri) return bad_request(req, "URI missing or longer than 128 characters");

    esp_err_t err = self->mqtt_.set_broker(uri);
    if (err == ESP_ERR_INVALID_ARG) {
        return bad_request(req, "URI must start with mqtt://, mqtts://, ws:// or wss://");
    }
    if (err != ESP_OK) {
        // Distinguished from a rejected URI: the value was stored and the
        // client could not be brought up, which is a different thing to fix.
        return send_json(req, R"({"ok":false,"error":"broker saved, but the client did not start"})");
    }
    return send_json(req, R"({"ok":true})");
}

esp_err_t WebServer::handle_post_ap(httpd_req_t* req)
{
    if (!allow_session_json(req)) return refusal_result(req);

    auto body = read_body(req);
    if (!body) return refusal_result(req);

    JsonDoc doc(cJSON_Parse(body->c_str()));
    if (!doc) return bad_request(req, "Invalid JSON");

    const char* password = string_field(doc.get(), "password", MAX_WIFI_PASSWORD);
    if (!password) return bad_request(req, "Password missing or longer than 63 characters");

    esp_err_t err = wifi_set_ap_password(password);
    if (err == ESP_ERR_INVALID_ARG) {
        return bad_request(req, "Password must be 8 to 63 characters");
    }
    if (err != ESP_OK) {
        // Was previously reported as "password too short" whatever went wrong,
        // which sends someone with a full NVS off retyping a fine password.
        return send_status(req, "500 Internal Server Error", "Could not store the password");
    }
    return send_json(req, R"({"ok":true})");
}

esp_err_t WebServer::handle_post_serial(httpd_req_t* req)
{
    if (!allow_session_json(req)) return refusal_result(req);

    auto* self = static_cast<WebServer*>(req->user_ctx);
    auto body = read_body(req);
    if (!body) return refusal_result(req);

    JsonDoc doc(cJSON_Parse(body->c_str()));
    if (!doc) return bad_request(req, "Invalid JSON");

    cJSON* baud = cJSON_GetObjectItem(doc.get(), "baud");
    if (!cJSON_IsNumber(baud)) return bad_request(req, "Missing baud");
    // Range-checked as a double first: converting an out-of-range or negative
    // double to an unsigned integer is undefined, so the check has to happen
    // before the cast rather than on the result.
    if (baud->valuedouble < 0 || baud->valuedouble > 4000000) {
        return bad_request(req, "Baud rate out of range");
    }

    esp_err_t err = self->serial_.set_baud(static_cast<uint32_t>(baud->valuedouble));
    if (err == ESP_ERR_INVALID_ARG) return bad_request(req, "Baud rate out of range");
    if (err != ESP_OK) {
        return send_status(req, "500 Internal Server Error", "Could not change the baud rate");
    }
    return send_json(req, R"({"ok":true})");
}

esp_err_t WebServer::handle_post_command(httpd_req_t* req)
{
    if (!allow_session_json(req)) return refusal_result(req);

    auto* self = static_cast<WebServer*>(req->user_ctx);
    auto body = read_body(req);
    if (!body) return refusal_result(req);

    JsonDoc doc(cJSON_Parse(body->c_str()));
    if (!doc) return bad_request(req, "Invalid JSON");

    const char* cmd = string_field(doc.get(), "command", MAX_COMMAND);
    if (!cmd) return bad_request(req, "Missing command");

    if (!self->vacuum_.command(cmd)) return bad_request(req, "Unknown command");
    return send_json(req, R"({"ok":true})");
}

esp_err_t WebServer::handle_post_ota(httpd_req_t* req)
{
    if (!allow_session_json(req)) return refusal_result(req);

    auto* self = static_cast<WebServer*>(req->user_ctx);
    auto body = read_body(req);
    if (!body) return refusal_result(req);

    JsonDoc doc(cJSON_Parse(body->c_str()));
    if (!doc) return bad_request(req, "Invalid JSON");

    const char* url = string_field(doc.get(), "url", MAX_OTA_URL);
    if (!url) return bad_request(req, "URL missing or longer than 256 characters");

    // Checked here as well as in the updater, so a bad value is a 400 the
    // browser can show rather than a failure that only appears in the log
    // several seconds later. Images are not signed, so this endpoint installs
    // whatever the named host serves: the session is the only thing standing
    // between the network and the firmware, which is why it is behind one.
    constexpr std::string_view https = "https://";
    if (std::string_view(url).substr(0, https.size()) != https) {
        return bad_request(req, "URL must be https://");
    }

    esp_err_t err = self->ota_.start(url);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_status(req, "409 Conflict", "An update is already running");
    }
    if (err != ESP_OK) {
        return send_status(req, "500 Internal Server Error", "Could not start the update");
    }
    ESP_LOGW(TAG, "Firmware update started from the web interface");
    return send_json(req, R"({"ok":true,"status":"downloading"})");
}

// --- WebSocket ---

esp_err_t WebServer::handle_ws(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);

    if (req->method == HTTP_GET) {
        // The upgrade is where this has to be checked. A WebSocket is not
        // covered by the same-origin policy, so without a check here any page
        // in any tab could open one to this device and both send and read.
        //
        // The server completes the handshake before a handler ever runs, so the
        // refusal is a closed socket rather than a 401: returning ESP_FAIL ends
        // the session, and the client is never added, so it is never sent
        // anything and no frame from it is ever read.
        // Origin demanded here: a browser always sends it on an upgrade, and
        // this is the request the same-origin policy does nothing about.
        if (!session_valid(req, true)) {
            ESP_LOGW(TAG, "Rejected an unauthenticated WebSocket upgrade");
            return ESP_FAIL;
        }

        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket client connected: fd=%d", fd);
        self->add_ws_client(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;

    // Header only: this fills in the length the client declared, which is the
    // number that must be checked before anything is allocated for it. The
    // 64-bit payload length field lets a 14-byte frame header ask for
    // half a gigabyte.
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    // Handled before the zero-length early return, because a bare CLOSE is
    // legal and carries no payload - checking length first meant a client that
    // closed politely was never removed from the list.
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket client disconnected: fd=%d", fd);
        self->remove_ws_client(fd);
        return ESP_OK;
    }

    if (frame.len == 0) return ESP_OK;
    if (frame.len > MAX_WS_FRAME_BYTES) {
        ESP_LOGW(TAG, "Dropping a %u byte WebSocket frame", static_cast<unsigned>(frame.len));
        return ESP_FAIL;  // closes the session
    }

    std::string buf(frame.len, '\0');
    frame.payload = reinterpret_cast<uint8_t*>(buf.data());
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) return ret;

    if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    JsonDoc doc(cJSON_Parse(buf.c_str()));
    if (!doc) return ESP_OK;

    const char* type = string_field(doc.get(), "type", 32);
    if (!type) return ESP_OK;

    // Only one inbound message type remains. The other one ran whatever string
    // arrived as a console command, which made every registered command -
    // including the firmware updater and the credential setter - callable by
    // anyone who could open this socket.
    if (std::string_view(type) == "serial_in") {
        const char* data = string_field(doc.get(), "data", MAX_SERIAL_TX);
        if (data) {
            // Sent verbatim: the browser decides the line ending, so the
            // terminal can talk to devices wanting CR, LF, CRLF or nothing.
            self->serial_.write(data);
        }
    }

    return ESP_OK;
}

void WebServer::add_ws_client(int fd)
{
    Lock lock(clients_mutex_);

    // File descriptors are recycled, and with LRU purging they are recycled
    // often. Without this a reconnect on a recycled number appended a second
    // copy that the liveness check could never remove, because the descriptor
    // genuinely is a live socket - and every log line then went out twice.
    if (std::find(ws_clients_.begin(), ws_clients_.end(), fd) != ws_clients_.end()) return;

    if (ws_clients_.size() >= MAX_WS_CLIENTS) {
        ESP_LOGW(TAG, "Too many WebSocket clients, refusing fd=%d", fd);
        return;
    }
    ws_clients_.push_back(fd);
}

void WebServer::remove_ws_client(int fd)
{
    Lock lock(clients_mutex_);
    ws_clients_.erase(
        std::remove(ws_clients_.begin(), ws_clients_.end(), fd),
        ws_clients_.end());
}

void WebServer::broadcast_ws(const std::string& msg)
{
    // The descriptor list is copied under the lock and the sends happen outside
    // it. Sending is a blocking write: a client that completes the handshake
    // and then never reads fills its window, and holding the lock across that
    // stalled the other broadcast task and stopped the UART being drained.
    std::vector<int> fds;
    {
        Lock lock(clients_mutex_);
        fds = ws_clients_;
    }
    if (fds.empty() || !server_) return;

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(msg.data()));
    frame.len = msg.size();

    for (int fd : fds) {
        if (httpd_ws_get_fd_info(server_, fd) != HTTPD_WS_CLIENT_WEBSOCKET
            || httpd_ws_send_frame_async(server_, fd, &frame) != ESP_OK) {
            remove_ws_client(fd);
        }
    }
}

void WebServer::broadcast_typed(const char* type, const char* data)
{
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", type);
    cJSON_AddStringToObject(msg, "data", data);

    char* json = cJSON_PrintUnformatted(msg);
    // Returns null when the heap is exhausted, which is exactly the state this
    // runs in after something else has eaten it - and the old code passed that
    // null straight into a std::string.
    if (json) {
        broadcast_ws(json);
        cJSON_free(json);
    }
    cJSON_Delete(msg);
}

// --- Serial bridge ---

std::string WebServer::escape_serial(const char* data, size_t len)
{
    static constexpr char HEX[] = "0123456789abcdef";

    std::string out;
    out.reserve(len);

    for (size_t i = 0; i < len; i++) {
        const auto c = static_cast<unsigned char>(data[i]);
        if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) {
            out.push_back(static_cast<char>(c));
        } else if (c == '\r') {
            // Dropped: the browser appends its own line breaks, and a bare CR
            // would otherwise show up as an escape in the middle of a line.
        } else {
            out += "\\x";
            out.push_back(HEX[c >> 4]);
            out.push_back(HEX[c & 0x0f]);
        }
    }
    return out;
}

void WebServer::serial_broadcast_task(void* arg)
{
    auto* self = static_cast<WebServer*>(arg);
    char buf[256];

    while (true) {
        // read() blocks only when the driver is installed. Startup tolerates a
        // UART that failed to open, and without this the loop then spun at full
        // speed at priority 5, starving the idle task into a watchdog reset on
        // a device that is deliberately clocked down to save power.
        if (!self->serial_.started()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        size_t received = self->serial_.read(buf, sizeof(buf), 100);
        if (received == 0) continue;

        self->broadcast_typed("serial", escape_serial(buf, received).c_str());
    }
}

// --- Log capture ---

int WebServer::log_vprintf(const char* fmt, va_list args)
{
    // Always print to serial. Read once into a local: the hook is published
    // before the previous handler has been stored, so there is a window in
    // which another task logging through here would dereference a null.
    vprintf_like_t original = s_original_vprintf;

    int ret = 0;
    if (original) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = original(fmt, args_copy);
        va_end(args_copy);
    }

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

    // Everything ESP_LOG produces goes to every connected client, which is why
    // the socket is authenticated: the stream carries the station's SSID, the
    // broker URI and anything a future log line happens to include.
    std::string partial;

    while (true) {
        size_t received = xStreamBufferReceive(self->log_stream_, buf, sizeof(buf) - 1,
                                               pdMS_TO_TICKS(100));
        if (received == 0) continue;
        buf[received] = '\0';

        // A read can end mid-line. The tail is carried into the next read
        // instead of being sent as its own message, which used to render one
        // log line as two rows in the browser.
        partial.append(buf, received);

        size_t start = 0;
        for (size_t nl; (nl = partial.find('\n', start)) != std::string::npos; start = nl + 1) {
            if (nl > start) {
                self->broadcast_typed("log", partial.substr(start, nl - start).c_str());
            }
        }
        partial.erase(0, start);

        // A device that never emits a newline must not grow this without bound.
        if (partial.size() > sizeof(buf)) {
            self->broadcast_typed("log", partial.c_str());
            partial.clear();
        }
    }
}

// --- Helpers ---

std::optional<std::string> WebServer::read_body(httpd_req_t* req)
{
    // Checked before anything is allocated. content_len is the client's
    // Content-Length header verbatim and the server imposes no cap of its own,
    // so a sixty-byte request with no body at all could otherwise ask for a
    // megabyte - and with exceptions enabled and no handler anywhere in the
    // tree, a failed allocation is std::terminate, which is a panic reboot on
    // the device's only management interface.
    if (req->content_len > MAX_BODY_BYTES) {
        send_status(req, "413 Content Too Large", "Body too large");
        return std::nullopt;
    }

    std::string body(req->content_len, '\0');
    size_t offset = 0;
    int timeouts = 0;

    while (offset < req->content_len) {
        int received = httpd_req_recv(req, body.data() + offset, req->content_len - offset);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            // Retryable, unlike a closed socket - but not indefinitely, because
            // one task serves every session.
            if (++timeouts > MAX_RECV_TIMEOUTS) {
                send_status(req, "408 Request Timeout", "Body did not arrive");
                return std::nullopt;
            }
            continue;
        }
        if (received <= 0) {
            send_status(req, "400 Bad Request", "Truncated body");
            return std::nullopt;
        }
        offset += static_cast<size_t>(received);
    }

    // A short body used to be padded with NULs and parsed anyway. Cut after a
    // complete-looking object it parses cleanly and applies half a
    // configuration.
    if (offset != req->content_len) {
        send_status(req, "400 Bad Request", "Truncated body");
        return std::nullopt;
    }
    return body;
}
