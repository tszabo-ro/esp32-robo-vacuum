#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Pure string and byte helpers, deliberately free of any ESP-IDF dependency.
//
// Everything here parses or renders a value that arrived from outside the
// device - an HTTP header, a broker URI, bytes off the robot's UART, a stored
// digest - which makes this the code where a bug is a security bug. Kept in one
// header with no platform includes so it can be compiled and tested on a host;
// see test/ for the cases these are held to.
namespace text {

// Compares without an early exit, so timing does not reveal how much of the
// input matched.
bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len);

// Replaces a URI's "user:password@" with "***@". esp-mqtt accepts credentials
// embedded in the URI, which is the natural thing for someone to type - and
// that value is logged, and every log line goes to every connected browser.
//
// A URI with no userinfo comes back unchanged, as does one whose '@' is in the
// path rather than the authority.
std::string redact_uri(std::string_view uri);

// Renders bytes from a UART as something a JSON string can carry: raw input may
// contain NULs or invalid UTF-8, which would truncate or corrupt the frame.
// Non-printables other than newline and tab become \xNN; a carriage return is
// dropped, because the browser appends its own line breaks.
std::string escape_serial(const char* data, size_t len);

// True when `origin` is exactly "http://" followed by `host`. Only plain HTTP
// is served, so anything else did not come from this device's own page.
bool origin_matches_host(std::string_view origin, std::string_view host);

// True for "application/json", optionally followed by parameters such as a
// charset. Requiring this is what forces a cross-origin POST through a
// preflight rather than being dispatched as a "simple request".
bool is_json_content_type(std::string_view type);

}  // namespace text
