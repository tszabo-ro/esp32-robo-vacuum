#include "robot_response.h"

#include <cstring>

namespace robot_response {
namespace {
constexpr std::string_view STATE_LABEL = "Current Robot State is:";
constexpr std::string_view FUEL_LABEL = "FuelPercent,";

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

}  // namespace robot_response
