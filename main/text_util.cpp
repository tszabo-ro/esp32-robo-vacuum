#include "text_util.h"

namespace text {

bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

std::string redact_uri(std::string_view uri)
{
    const size_t scheme_end = uri.find("//");
    if (scheme_end == std::string_view::npos) return std::string(uri);

    const size_t authority = scheme_end + 2;
    const size_t at = uri.find('@', authority);
    if (at == std::string_view::npos) return std::string(uri);

    // The userinfo component is everything between "//" and "@". A '/' before
    // the '@' means the '@' is in the path, not in the authority.
    const size_t slash = uri.find('/', authority);
    if (slash != std::string_view::npos && slash < at) return std::string(uri);

    return std::string(uri.substr(0, authority)) + "***@" + std::string(uri.substr(at + 1));
}

std::string escape_serial(const char* data, size_t len)
{
    static constexpr char HEX[] = "0123456789abcdef";

    std::string out;
    out.reserve(len);

    for (size_t i = 0; i < len; i++) {
        const auto c = static_cast<unsigned char>(data[i]);
        if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) {
            out.push_back(static_cast<char>(c));
        } else if (c == '\r') {
            // Dropped: the browser appends its own line breaks, and a bare CR
            // would otherwise show up as an escape in the middle of a line.
        } else {
            out += "\\x";
            out.push_back(HEX[c >> 4]);
            out.push_back(HEX[c & 0x0f]);
        }
    }
    return out;
}

bool origin_matches_host(std::string_view origin, std::string_view host)
{
    constexpr std::string_view prefix = "http://";
    if (origin.size() <= prefix.size()) return false;
    if (origin.substr(0, prefix.size()) != prefix) return false;
    if (host.empty()) return false;
    return origin.substr(prefix.size()) == host;
}

bool is_json_content_type(std::string_view type)
{
    constexpr std::string_view json = "application/json";
    if (type.substr(0, json.size()) != json) return false;
    return type.size() == json.size() || type[json.size()] == ';';
}

}  // namespace text
