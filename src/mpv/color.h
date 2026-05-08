#pragma once

#include "../color.h"
#include <string_view>

namespace mpv {

// Parses any form mpv emits or accepts (third_party/mpv/options/m_option.c
// :2079-2147). Note mpv's print_color emits #AARRGGBB — alpha is FIRST.
// Returns Color{} on malformed input. Does NOT accept CSS #RGB.
Color parseColor(std::string_view s);

}  // namespace mpv
