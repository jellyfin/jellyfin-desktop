#include "color.h"

#include "../color/color.h"

#include <string>

namespace mpv {

Color parseColor(std::string_view s) {
    return Color{jfn_mpv_parse_color(std::string(s).c_str())};
}

}  // namespace mpv
