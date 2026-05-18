#include "color.h"

#include "../color/color.h"

#include <string>

namespace cef {

Color parseColor(std::string_view s) {
    return Color{jfn_cef_parse_color(std::string(s).c_str())};
}

}  // namespace cef
