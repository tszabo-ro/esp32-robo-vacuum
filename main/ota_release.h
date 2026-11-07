#pragma once

#include <cstddef>
#include <string_view>

// Validation for the bounded redirect used by the OTA update checker. Kept
// independent of ESP-IDF so malformed release responses can be host-tested.
namespace ota_release {

bool is_location_header(std::string_view name);

// Extracts the release tag from GitHub's redirect to our firmware asset. The
// output is NUL-terminated on success; false leaves it untouched.
bool tag_from_redirect(std::string_view location, char* tag, size_t tag_size);

}  // namespace ota_release
