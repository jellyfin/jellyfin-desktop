#pragma once

#include "../color.h"
#include <string_view>

namespace cef {

// Parses a CSS color from <meta name="theme-color">: #RGB or #RRGGBB.
// Returns Color{} on malformed input. Does NOT accept mpv forms.
Color parseColor(std::string_view s);

}  // namespace cef
