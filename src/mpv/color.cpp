#include "color.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace mpv {

namespace {

bool parseUnitFloat(std::string_view s, double& out) {
    if (s.empty()) return false;
    // libstdc++ only added std::from_chars<double> in gcc 11 — route through
    // strtod instead for portability. Validate no trailing chars and range.
    std::string tmp(s);
    char* end = nullptr;
    double v = strtod(tmp.c_str(), &end);
    if (!end || end == tmp.c_str() || *end != '\0') return false;
    if (!std::isfinite(v) || v < 0.0 || v > 1.0) return false;
    out = v;
    return true;
}

uint8_t scale(double v) {
    return static_cast<uint8_t>(std::lround(v * 255.0));
}

}  // namespace

Color parseColor(std::string_view s) {
    if (s.empty()) return Color{};

    if (s.front() == '#') {
        std::string_view hex = s.substr(1);
        // mpv only accepts 6 or 8 hex digits; CSS-shorthand #RGB is rejected.
        std::string_view rgb;
        if (hex.size() == 6) rgb = hex;
        else if (hex.size() == 8) rgb = hex.substr(2);  // drop leading AA
        else return Color{};
        uint8_t r, g, b;
        if (!parseHexByte(rgb.substr(0, 2), r)) return Color{};
        if (!parseHexByte(rgb.substr(2, 2), g)) return Color{};
        if (!parseHexByte(rgb.substr(4, 2), b)) return Color{};
        return Color{r, g, b};
    }

    if (s.find('/') == std::string_view::npos) return Color{};

    // Slash form: split into 1..4 components.
    double comp[4] = {0, 0, 0, 1};
    int n = 0;
    size_t i = 0;
    while (i <= s.size() && n < 5) {
        size_t j = s.find('/', i);
        std::string_view tok = s.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
        if (n >= 4) return Color{};  // too many fields
        double v;
        if (!parseUnitFloat(tok, v)) return Color{};
        comp[n++] = v;
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    if (n == 0) return Color{};
    // mpv parse_color rules (m_option.c:2119-2122): 1 component = gray,
    // 2 = gray+alpha, 3 = r/g/b, 4 = r/g/b/a. Alpha is always dropped.
    if (n <= 2) return Color{scale(comp[0]), scale(comp[0]), scale(comp[0])};
    return Color{scale(comp[0]), scale(comp[1]), scale(comp[2])};
}

}  // namespace mpv
