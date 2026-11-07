#include "ota_release.h"

#include <cstring>

namespace ota_release {
namespace {

constexpr std::string_view RELEASE_DOWNLOAD_PREFIX =
    "https://github.com/tszabo-ro/esp32-robo-vacuum/releases/download/";
constexpr std::string_view FIRMWARE_ASSET = "neato-mqtt-controller.bin";

}  // namespace

bool is_location_header(std::string_view name)
{
    constexpr std::string_view expected = "location";
    if (name.size() != expected.size()) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        char character = name[i];
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        if (character != expected[i]) return false;
    }
    return true;
}

bool tag_from_redirect(std::string_view location, char* tag, size_t tag_size)
{
    if (!tag || tag_size == 0 || !location.starts_with(RELEASE_DOWNLOAD_PREFIX)) return false;
    location.remove_prefix(RELEASE_DOWNLOAD_PREFIX.size());

    const size_t separator = location.find('/');
    if (separator == 0 || separator == std::string_view::npos
        || location.substr(separator + 1) != FIRMWARE_ASSET
        || separator >= tag_size) {
        return false;
    }

    std::memcpy(tag, location.data(), separator);
    tag[separator] = '\0';
    return true;
}

}  // namespace ota_release
