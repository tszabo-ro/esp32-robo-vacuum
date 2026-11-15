#pragma once

#include <cstddef>
#include <string_view>

namespace robot_response {

struct Diagnostic {
    int code = 200; // UI_ALERT_INVALID means no active fault/alert.
    char token[96] = {};
};

// Require both GetErr sections; reject malformed/truncated entries atomically.
bool parse_errors(std::string_view response, Diagnostic& error, Diagnostic& alert);
// Known descriptions, with the raw token retained separately for unknown codes.
const char* diagnostic_message(int code);

struct LdsScanSummary {
    unsigned points = 0;
    unsigned valid_points = 0;
};
// Count unique, well-formed angular samples; zero-distance/error samples are
// not usable readings. Does not infer scanner health from an incomplete scan.
LdsScanSummary summarize_lds_scan(std::string_view response);

// Extract the current robot state into a bounded NUL-terminated buffer.
bool parse_state(std::string_view response, char* out, size_t out_size);

// Extract FuelPercent (0-100) from a GetCharger response.
bool parse_battery_percent(std::string_view response, int& out);

// Extract the complete serial-number field from GetVersion into a bounded,
// NUL-terminated buffer.
bool parse_serial_number(std::string_view response, char* out, size_t out_size);

// Derive the 25-character key required by the D3-D7 SetEvent API. The serial
// number contains a 12-character hexadecimal robot identifier after its first
// comma. The output buffer must hold at least 26 bytes including the NUL.
bool derive_set_event_key(std::string_view serial_number, char* out, size_t out_size);

// Translate the deliberately small controller action vocabulary to a Neato
// Smart App event, and build the complete authenticated serial command.
const char* set_event_for_action(std::string_view action);
bool build_set_event_command(const char* event, std::string_view key, char* out, size_t out_size);

}  // namespace robot_response
