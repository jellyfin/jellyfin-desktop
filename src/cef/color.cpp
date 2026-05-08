#include "color.h"

namespace cef {

namespace {

bool parseHexNibble(char c, uint8_t& out) {
    if (c >= '0' && c <= '9') { out = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { out = 10 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'F') { out = 10 + (c - 'A'); return true; }
    return false;
}

}  // namespace

Color parseColor(std::string_view s) {
    if (s.empty() || s.front() != '#') return Color{};
    std::string_view hex = s.substr(1);
    if (hex.size() == 3) {
        uint8_t r, g, b;
        if (!parseHexNibble(hex[0], r)) return Color{};
        if (!parseHexNibble(hex[1], g)) return Color{};
        if (!parseHexNibble(hex[2], b)) return Color{};
        return Color{static_cast<uint8_t>(r * 0x11),
                     static_cast<uint8_t>(g * 0x11),
                     static_cast<uint8_t>(b * 0x11)};
    }
    if (hex.size() == 6) {
        uint8_t r, g, b;
        if (!parseHexByte(hex.substr(0, 2), r)) return Color{};
        if (!parseHexByte(hex.substr(2, 2), g)) return Color{};
        if (!parseHexByte(hex.substr(4, 2), b)) return Color{};
        return Color{r, g, b};
    }
    return Color{};
}

}  // namespace cef
