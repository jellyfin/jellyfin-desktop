#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

inline bool parseHexByte(std::string_view s, uint8_t& out) {
    if (s.size() != 2) return false;
    unsigned v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || p != s.data() + s.size()) return false;
    out = static_cast<uint8_t>(v);
    return true;
}

constexpr char hexdigit(uint32_t c, int i) {
    uint8_t n = (c >> (20 - i * 4)) & 0xF;
    return n < 10 ? '0' + n : 'a' + (n - 10);
}

struct Color {
    uint32_t rgb;
    uint8_t r, g, b;
    char hex[8];  // "#RRGGBB\0"
    constexpr Color(uint32_t c = 0) :
        rgb(c),
        r((c >> 16) & 0xFF),
        g((c >> 8) & 0xFF),
        b(c & 0xFF),
        hex{'#', hexdigit(c,0), hexdigit(c,1), hexdigit(c,2),
            hexdigit(c,3), hexdigit(c,4), hexdigit(c,5), '\0'} {}
    constexpr Color(uint8_t rr, uint8_t gg, uint8_t bb)
        : Color(static_cast<uint32_t>(rr) << 16
              | static_cast<uint32_t>(gg) << 8
              | static_cast<uint32_t>(bb)) {}
    constexpr bool operator==(const Color& o) const { return rgb == o.rgb; }
};

// Startup background color (loading screen / overlay).
constexpr Color kBgColor{0x101010};
