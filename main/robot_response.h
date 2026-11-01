#pragma once

#include <cstddef>
#include <string_view>

namespace robot_response {

// Extract the current robot state into a bounded NUL-terminated buffer.
bool parse_state(std::string_view response, char* out, size_t out_size);

// Extract FuelPercent (0-100) from a GetCharger response.
bool parse_battery_percent(std::string_view response, int& out);

}  // namespace robot_response
