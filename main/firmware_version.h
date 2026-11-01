#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace firmware_version {

struct Version {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
};

// Parse release tags (`v1.2.3`) and ESP-IDF git-describe versions
// (`v1.2.3-4-gabcdef0[-dirty]`).
bool parse_release_tag(std::string_view text, Version& out);
bool parse_running_version(std::string_view text, Version& out);
bool is_newer(const Version& candidate, const Version& running);

}  // namespace firmware_version
