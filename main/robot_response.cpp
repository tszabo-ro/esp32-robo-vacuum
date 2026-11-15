#include "robot_response.h"

#include <cstdint>
#include <charconv>
#include <cstring>

namespace robot_response {
namespace {
constexpr std::string_view STATE_LABEL = "Current Robot State is:";
constexpr std::string_view FUEL_LABEL = "FuelPercent,";
constexpr std::string_view SERIAL_LABEL = "Serial Number,";
constexpr std::string_view SET_EVENT_PREFIX = "SetEvent event ";
constexpr std::string_view SET_EVENT_KEY = " SKey ";
constexpr std::string_view EVENT_START_HOUSE = "UIMGR_EVENT_SMARTAPP_START_HOUSE_CLEANING";
constexpr std::string_view EVENT_START_SPOT = "UIMGR_EVENT_SMARTAPP_START_SPOT_CLEANING";
constexpr std::string_view EVENT_STOP = "UIMGR_EVENT_SMARTAPP_STOP_CLEANING";
constexpr std::string_view EVENT_DOCK = "UIMGR_EVENT_SMARTAPP_SEND_TO_BASE";
constexpr size_t SET_EVENT_KEY_LENGTH = 25;

size_t find(std::string_view text, std::string_view needle)
{
    const size_t at = text.find(needle);
    return at;
}

bool is_state_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_';
}

bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool copy_trimmed(std::string_view value, char* out, size_t out_size)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (!out || out_size == 0 || value.empty() || value.size() >= out_size) return false;
    for (char c : value) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) return false;
    }
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return true;
}
}  // namespace

LdsScanSummary summarize_lds_scan(std::string_view response)
{
    LdsScanSummary result;
    bool seen[360] = {};
    while (!response.empty()) {
        const size_t newline = response.find('\n');
        auto line = response.substr(0, newline);
        response = newline == std::string_view::npos ? std::string_view{} : response.substr(newline + 1);
        unsigned values[4] = {};
        bool valid = true;
        for (unsigned i = 0; i < 4; ++i) {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            const size_t end = line.find_first_of(",\r \t");
            const auto field = line.substr(0, end);
            const auto parsed = std::from_chars(field.data(), field.data() + field.size(), values[i], i == 3 ? 16 : 10);
            if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) { valid = false; break; }
            if (i < 3) {
                if (end == std::string_view::npos || line[end] != ',') { valid = false; break; }
                line.remove_prefix(end + 1);
            }
        }
        if (!valid || values[0] >= 360 || seen[values[0]]) continue;
        seen[values[0]] = true;
        ++result.points;
        if (values[1] > 0 && values[3] == 0) ++result.valid_points;
    }
    return result;
}

bool parse_errors(std::string_view response, Diagnostic& error, Diagnostic& alert)
{
    Diagnostic parsed[2];
    bool seen[2] = {};
    int section = -1;
    while (!response.empty()) {
        const size_t end = response.find('\n');
        auto line = response.substr(0, end);
        response = end == std::string_view::npos ? std::string_view{} : response.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line == "Error" || line == "Alert") {
            section = line == "Error" ? 0 : 1;
            if (seen[section]) return false;
            continue;
        }
        if (section < 0 || line.empty()) continue;
        if (line.size() < 3 || line[0] < '0' || line[0] > '9'
            || line[1] < '0' || line[1] > '9' || line[2] < '0' || line[2] > '9') return false;
        const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + line[2] - '0';
        line.remove_prefix(3);
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        if (!line.starts_with("-")) return false;
        const size_t open = line.find('('), close = line.find(')');
        if (code < 200 || open == std::string_view::npos || close == std::string_view::npos
            || close <= open + 1) return false;
        auto token = line.substr(open + 1, close - open - 1);
        if (token.size() >= sizeof(parsed[section].token)
            || (!token.starts_with("UI_ERROR_") && !token.starts_with("UI_ALERT_"))) return false;
        for (char c : token) if (!is_state_char(c)) return false;
        parsed[section].code = code;
        std::memcpy(parsed[section].token, token.data(), token.size());
        seen[section] = true;
        section = -1;
    }
    if (!seen[0] || !seen[1]) return false;
    error = parsed[0];
    alert = parsed[1];
    return true;
}

const char* diagnostic_message(int code)
{
    switch (code) {
    case 200: return "None";
    case 203: return "Return to cleaning start location";
    case 249: return "Dust bin is missing";
    case 250: return "Dust bin is full";
    case 251: return "Battery is too hot";
    case 252: return "Unable to return to dock";
    case 254: return "Bumper is stuck";
    case 255: return "Robot detected being picked up or tilted";
    case 257: return "Left wheel is stuck";
    case 258: return "Right wheel is stuck";
    case 259: return "Laser scanner is jammed";
    case 260: return "Laser scanner is disconnected";
    case 261: return "Missing data packets from laser scanner";
    case 262: return "Invalid data packets from laser scanner";
    default: return ""; // UI derives a readable label from the raw token.
    }
}

bool parse_state(std::string_view response, char* out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    const size_t label = find(response, STATE_LABEL);
    if (label == std::string_view::npos) return false;
    size_t start = label + STATE_LABEL.size();
    while (start < response.size() && (response[start] == ' ' || response[start] == '\t')) ++start;
    size_t end = response.find_first_of("\r\n", start);
    if (end == std::string_view::npos) end = response.size();
    while (end > start && (response[end - 1] == ' ' || response[end - 1] == '\t')) --end;
    const size_t length = end - start;
    if (length == 0 || length >= out_size) return false;
    for (size_t i = start; i < end; ++i) {
        if (!is_state_char(response[i])) return false;
    }
    std::memcpy(out, response.data() + start, length);
    out[length] = '\0';
    return true;
}

bool parse_battery_percent(std::string_view response, int& out)
{
    const size_t label = find(response, FUEL_LABEL);
    if (label == std::string_view::npos) return false;
    size_t pos = label + FUEL_LABEL.size();
    while (pos < response.size() && (response[pos] == ' ' || response[pos] == '\t')) ++pos;
    if (pos == response.size() || response[pos] < '0' || response[pos] > '9') return false;

    unsigned value = 0;
    while (pos < response.size() && response[pos] >= '0' && response[pos] <= '9') {
        value = value * 10 + static_cast<unsigned>(response[pos++] - '0');
        if (value > 100) return false;
    }
    if (pos < response.size() && response[pos] != ',' && response[pos] != ' '
        && response[pos] != '\t' && response[pos] != '\r' && response[pos] != '\n') return false;
    out = static_cast<int>(value);
    return true;
}

bool parse_serial_number(std::string_view response, char* out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    size_t line_start = 0;
    while (line_start < response.size()) {
        size_t line_end = response.find_first_of("\r\n", line_start);
        if (line_end == std::string_view::npos) line_end = response.size();
        std::string_view line = response.substr(line_start, line_end - line_start);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.starts_with(SERIAL_LABEL)) {
            return copy_trimmed(line.substr(SERIAL_LABEL.size()), out, out_size);
        }
        line_start = line_end;
        while (line_start < response.size()
               && (response[line_start] == '\r' || response[line_start] == '\n')) ++line_start;
    }
    return false;
}

bool derive_set_event_key(std::string_view serial_number, char* out, size_t out_size)
{
    if (!out || out_size < SET_EVENT_KEY_LENGTH + 1) return false;
    out[0] = '\0';
    const size_t comma = serial_number.find(',');
    if (comma == std::string_view::npos || comma + 1 + 12 > serial_number.size()) return false;
    const std::string_view robot_id = serial_number.substr(comma + 1, 12);
    for (char c : robot_id) {
        if (!is_hex(c)) return false;
    }
    if (comma + 13 < serial_number.size() && serial_number[comma + 13] != ',') return false;

    constexpr uint8_t seed[] = {0x68, 0x36, 0x43, 0x58, 0x09, 0x09, 0x3a, 0x3c, 0x2a, 0x7b, 0x59};
    uint8_t state[256];
    for (unsigned i = 0; i < sizeof(state); ++i) state[i] = static_cast<uint8_t>(i);
    unsigned j = 0;
    for (unsigned i = 0; i < sizeof(state); ++i) {
        j = (j + state[i] + seed[i % sizeof(seed)]) & 0xff;
        const uint8_t swap = state[i];
        state[i] = state[j];
        state[j] = swap;
    }

    constexpr char hex[] = "0123456789abcdef";
    unsigned i = 0;
    j = 0;
    for (size_t k = 0; k < robot_id.size(); ++k) {
        i = (i + 1) & 0xff;
        j = (j + state[i]) & 0xff;
        const uint8_t swap = state[i];
        state[i] = state[j];
        state[j] = swap;
        const uint8_t value = state[(state[i] + state[j]) & 0xff]
                            ^ static_cast<uint8_t>(robot_id[k]);
        out[k * 2] = hex[value >> 4];
        out[k * 2 + 1] = hex[value & 0x0f];
    }
    // This final character is part of Neato's SetEvent key format.
    out[24] = out[6];
    out[25] = '\0';
    return true;
}

const char* set_event_for_action(std::string_view action)
{
    if (action == "start") return EVENT_START_HOUSE.data();
    if (action == "spot") return EVENT_START_SPOT.data();
    if (action == "stop") return EVENT_STOP.data();
    if (action == "dock") return EVENT_DOCK.data();
    return nullptr;
}

bool build_set_event_command(const char* event, std::string_view key, char* out, size_t out_size)
{
    if (!event || !out || key.size() != SET_EVENT_KEY_LENGTH) return false;
    const std::string_view event_view(event);
    const size_t length = SET_EVENT_PREFIX.size() + event_view.size() + SET_EVENT_KEY.size() + key.size();
    if (length + 1 > out_size) return false;
    char* pos = out;
    for (char c : SET_EVENT_PREFIX) *pos++ = c;
    for (char c : event_view) *pos++ = c;
    for (char c : SET_EVENT_KEY) *pos++ = c;
    for (char c : key) *pos++ = c;
    *pos = '\0';
    return true;
}

}  // namespace robot_response
