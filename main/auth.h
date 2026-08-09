#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include "esp_err.h"

// Single-account authentication for the web interface.
//
// There is one account, because there is one owner. The password is never
// stored: NVS holds a random salt and a PBKDF2-HMAC-SHA256 digest of it, so
// reading the flash out over USB does not hand over the password itself.
//
// Sessions live in RAM only. A reboot logs everyone out, which is the right
// default for a device whose whole recovery story is "it restarted".
namespace Auth {

// A session token as it appears in the cookie: 128 bits, hex encoded.
inline constexpr size_t TOKEN_CHARS = 32;

// Short enough to type on a phone, long enough to matter. The upper bound
// exists so a request body cannot drive an unbounded PBKDF2.
inline constexpr size_t MIN_PASSWORD = 8;
inline constexpr size_t MAX_PASSWORD = 64;

// Loads the stored digest. Safe to call before NVS holds anything.
void init();

// True once a password has been set. While this is false the device serves
// nothing but the setup endpoint: a device that shipped with no credentials
// must not be usable by whoever reaches it first *and* look configured.
bool configured();

// Replaces the password and invalidates every existing session, so a password
// change locks out anyone already holding a token.
esp_err_t set_password(std::string_view password);

// Constant-time. Returns false when no password has been set yet.
bool verify_password(std::string_view password);

// Repeated failures are throttled rather than counted to a permanent lockout:
// this is the only way into a sealed device, so locking it out for good would
// be a self-inflicted brick.
bool throttled();
void note_failure();
void note_success();

// Returns a new token, or an empty string if no session slot was free.
std::string create_session();

// Validates a token and extends its lifetime. Constant-time in the token
// comparison, so a valid prefix cannot be found one character at a time.
bool validate_session(std::string_view token);

void destroy_session(std::string_view token);
void destroy_all_sessions();

}  // namespace Auth
