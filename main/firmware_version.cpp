#include "firmware_version.h"

namespace firmware_version {
namespace {

bool parse_number(std::string_view text, size_t& pos, uint32_t& value)
{
    if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') return false;
    value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        const uint32_t digit = static_cast<uint32_t>(text[pos++] - '0');
        if (value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

bool parse_base(std::string_view text, size_t& pos, Version& out)
{
    if (text.empty() || text[0] != 'v') return false;
    pos = 1;
    if (!parse_number(text, pos, out.major) || pos >= text.size() || text[pos++] != '.')
        return false;
    if (!parse_number(text, pos, out.minor) || pos >= text.size() || text[pos++] != '.')
        return false;
    return parse_number(text, pos, out.patch);
}

}  // namespace

bool parse_release_tag(std::string_view text, Version& out)
{
    size_t pos = 0;
    return parse_base(text, pos, out) && pos == text.size();
}

bool parse_running_version(std::string_view text, Version& out)
{
    size_t pos = 0;
    if (!parse_base(text, pos, out)) return false;
    if (pos == text.size()) return true;
    if (text.substr(pos) == "-dirty") return true;

    // ESP-IDF uses `git describe`; validate the commit distance and hash,
    // while comparison uses the base release tag.
    if (text[pos++] != '-') return false;
    uint32_t distance = 0;
    if (!parse_number(text, pos, distance) || pos + 2 >= text.size()
        || text[pos++] != '-' || text[pos++] != 'g') return false;
    (void)distance;

    const size_t hash_start = pos;
    while (pos < text.size() && ((text[pos] >= '0' && text[pos] <= '9')
           || (text[pos] >= 'a' && text[pos] <= 'f'))) ++pos;
    if (pos == hash_start) return false;
    if (pos < text.size() && text.substr(pos) == "-dirty") pos += 6;
    return pos == text.size();
}

bool is_newer(const Version& candidate, const Version& running)
{
    if (candidate.major != running.major) return candidate.major > running.major;
    if (candidate.minor != running.minor) return candidate.minor > running.minor;
    return candidate.patch > running.patch;
}

}  // namespace firmware_version
