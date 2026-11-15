#include "robot_response.h"

#include <cstdint>
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
