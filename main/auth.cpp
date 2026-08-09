#include "auth.h"

#include <cstring>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/pkcs5.h"
#include "nvs_flash.h"

static constexpr const char* TAG = "auth";
static constexpr const char* NVS_NAMESPACE = "auth";

namespace {

constexpr size_t SALT_BYTES = 16;
constexpr size_t DIGEST_BYTES = 32;

// PBKDF2 rounds. The C3 runs at 80 MHz with hardware SHA, which puts this at
// roughly a tenth of a second - unnoticeable on a login, but it turns an
// offline guess of a recovered flash image from free into expensive.
constexpr unsigned PBKDF2_ITERATIONS = 20000;

// Four is more than one household needs, and a fixed table means a client
// cannot grow the device's memory use by logging in repeatedly.
constexpr size_t MAX_SESSIONS = 4;

// Long enough that the settings page is not interrupted mid-edit, short enough
// that a forgotten phone on the guest network does not stay logged in forever.
// Refreshed on every validated request, so an active session does not expire.
constexpr int64_t SESSION_TTL_US = 12LL * 60 * 60 * 1000000;

// Failed logins are slowed down rather than blocked: after this many in a row,
// each further attempt is rejected until the window passes.
constexpr int FAILURES_BEFORE_THROTTLE = 5;
constexpr int64_t THROTTLE_WINDOW_US = 30LL * 1000000;

struct Session {
    char token[Auth::TOKEN_CHARS + 1] = {};
    int64_t expires_us = 0;
};

SemaphoreHandle_t s_mutex = nullptr;
Session s_sessions[MAX_SESSIONS];

bool s_configured = false;
uint8_t s_salt[SALT_BYTES] = {};
uint8_t s_digest[DIGEST_BYTES] = {};

int s_failures = 0;
int64_t s_throttled_until_us = 0;

class Lock {
public:
    Lock() { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
    ~Lock() { if (s_mutex) xSemaphoreGive(s_mutex); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

// Compares without an early exit, so timing does not reveal how much of the
// input matched.
bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

esp_err_t derive(std::string_view password, const uint8_t* salt, uint8_t* out)
{
    const int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        reinterpret_cast<const unsigned char*>(password.data()), password.size(),
        salt, SALT_BYTES,
        PBKDF2_ITERATIONS, DIGEST_BYTES, out);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

void random_hex(char* out, size_t chars)
{
    static constexpr char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < chars; i += 2) {
        const auto byte = static_cast<uint8_t>(esp_random() & 0xff);
        out[i] = HEX[byte >> 4];
        if (i + 1 < chars) out[i + 1] = HEX[byte & 0x0f];
    }
    out[chars] = '\0';
}

}  // namespace

void Auth::init()
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Could not create the session mutex; the web interface will stay locked");
        return;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "No password set yet. The web interface will ask for one on first use.");
        return;
    }

    size_t salt_len = sizeof(s_salt);
    size_t digest_len = sizeof(s_digest);
    esp_err_t err = nvs_get_blob(handle, "salt", s_salt, &salt_len);
    if (err == ESP_OK) err = nvs_get_blob(handle, "digest", s_digest, &digest_len);
    nvs_close(handle);

    if (err == ESP_OK && salt_len == sizeof(s_salt) && digest_len == sizeof(s_digest)) {
        s_configured = true;
        ESP_LOGI(TAG, "Web interface password loaded");
    } else {
        // Deliberately not an abort and not a silent pass: a half-written
        // record must fail closed, but it must also be recoverable by setting
        // a password again rather than by reflashing.
        std::memset(s_salt, 0, sizeof(s_salt));
        std::memset(s_digest, 0, sizeof(s_digest));
        ESP_LOGW(TAG, "No usable password record. The web interface will ask for one on first use.");
    }
}

bool Auth::configured()
{
    Lock lock;
    return s_configured;
}

esp_err_t Auth::set_password(std::string_view password)
{
    if (password.size() < MIN_PASSWORD || password.size() > MAX_PASSWORD) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t salt[SALT_BYTES];
    esp_fill_random(salt, sizeof(salt));

    uint8_t digest[DIGEST_BYTES];
    esp_err_t err = derive(password, salt, digest);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not derive the password digest");
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, "salt", salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(handle, "digest", digest, sizeof(digest));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return err;

    {
        Lock lock;
        std::memcpy(s_salt, salt, sizeof(s_salt));
        std::memcpy(s_digest, digest, sizeof(s_digest));
        s_configured = true;
        s_failures = 0;
        s_throttled_until_us = 0;
        // Everyone re-authenticates: a password change is exactly the moment
        // you want existing sessions to stop working.
        for (auto& s : s_sessions) s = Session{};
    }

    ESP_LOGI(TAG, "Web interface password set");
    return ESP_OK;
}

bool Auth::verify_password(std::string_view password)
{
    if (password.size() > MAX_PASSWORD) return false;

    uint8_t salt[SALT_BYTES];
    uint8_t expected[DIGEST_BYTES];
    {
        Lock lock;
        if (!s_configured) return false;
        std::memcpy(salt, s_salt, sizeof(salt));
        std::memcpy(expected, s_digest, sizeof(expected));
    }

    uint8_t actual[DIGEST_BYTES];
    if (derive(password, salt, actual) != ESP_OK) return false;

    return constant_time_equal(actual, expected, DIGEST_BYTES);
}

bool Auth::throttled()
{
    Lock lock;
    return esp_timer_get_time() < s_throttled_until_us;
}

void Auth::note_failure()
{
    Lock lock;
    if (++s_failures >= FAILURES_BEFORE_THROTTLE) {
        s_throttled_until_us = esp_timer_get_time() + THROTTLE_WINDOW_US;
        s_failures = 0;
        ESP_LOGW(TAG, "Too many failed logins, refusing attempts for %d seconds",
                 static_cast<int>(THROTTLE_WINDOW_US / 1000000));
    }
}

void Auth::note_success()
{
    Lock lock;
    s_failures = 0;
    s_throttled_until_us = 0;
}

std::string Auth::create_session()
{
    const int64_t now = esp_timer_get_time();

    Lock lock;
    for (auto& s : s_sessions) {
        if (s.token[0] != '\0' && s.expires_us > now) continue;

        random_hex(s.token, TOKEN_CHARS);
        s.expires_us = now + SESSION_TTL_US;
        return std::string(s.token);
    }

    ESP_LOGW(TAG, "All %d session slots are in use", static_cast<int>(MAX_SESSIONS));
    return {};
}

bool Auth::validate_session(std::string_view token)
{
    if (token.size() != TOKEN_CHARS) return false;

    const int64_t now = esp_timer_get_time();

    Lock lock;
    for (auto& s : s_sessions) {
        if (s.token[0] == '\0') continue;
        if (s.expires_us <= now) {
            s = Session{};
            continue;
        }
        if (constant_time_equal(reinterpret_cast<const uint8_t*>(s.token),
                                reinterpret_cast<const uint8_t*>(token.data()),
                                TOKEN_CHARS)) {
            s.expires_us = now + SESSION_TTL_US;
            return true;
        }
    }
    return false;
}

void Auth::destroy_session(std::string_view token)
{
    if (token.size() != TOKEN_CHARS) return;

    Lock lock;
    for (auto& s : s_sessions) {
        if (s.token[0] == '\0') continue;
        if (constant_time_equal(reinterpret_cast<const uint8_t*>(s.token),
                                reinterpret_cast<const uint8_t*>(token.data()),
                                TOKEN_CHARS)) {
            s = Session{};
            return;
        }
    }
}

void Auth::destroy_all_sessions()
{
    Lock lock;
    for (auto& s : s_sessions) s = Session{};
}
