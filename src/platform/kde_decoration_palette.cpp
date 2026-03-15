#include "platform/kde_decoration_palette.h"
#include "logging.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <string>
#include <wayland-client.h>
#include "wayland-protocols/server-decoration-palette-client.h"

namespace {

// Base color scheme template (derived from BreezeDark).
// %HEADER_BG%, %INACTIVE_BG%, %ACTIVE_FG%, %INACTIVE_FG% are substituted at runtime.
constexpr const char* kColorSchemeTemplate = R"([ColorEffects:Disabled]
Color=56,56,56
ColorAmount=0
ColorEffect=0
ContrastAmount=0.65
ContrastEffect=1
IntensityAmount=0.1
IntensityEffect=2

[ColorEffects:Inactive]
ChangeSelectionColor=true
Color=112,111,110
ColorAmount=0.025
ColorEffect=2
ContrastAmount=0.1
ContrastEffect=2
Enable=false
IntensityAmount=0
IntensityEffect=0

[Colors:Button]
BackgroundAlternate=30,87,116
BackgroundNormal=41,44,48
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Complementary]
BackgroundAlternate=30,87,116
BackgroundNormal=32,35,38
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Header]
BackgroundAlternate=%HEADER_BG%
BackgroundNormal=%HEADER_BG%
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=%ACTIVE_FG%
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Header][Inactive]
BackgroundAlternate=%INACTIVE_BG%
BackgroundNormal=%INACTIVE_BG%
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=%INACTIVE_FG%
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Selection]
BackgroundAlternate=30,87,116
BackgroundNormal=61,174,233
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=252,252,252
ForegroundInactive=161,169,177
ForegroundLink=253,188,75
ForegroundNegative=176,55,69
ForegroundNeutral=198,92,0
ForegroundNormal=252,252,252
ForegroundPositive=23,104,57
ForegroundVisited=155,89,182

[Colors:Tooltip]
BackgroundAlternate=32,35,38
BackgroundNormal=41,44,48
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:View]
BackgroundAlternate=29,31,34
BackgroundNormal=20,22,24
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Window]
BackgroundAlternate=41,44,48
BackgroundNormal=32,35,38
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[KDE]
contrast=4

[WM]
activeBackground=%HEADER_BG%
activeBlend=252,252,252
activeForeground=%ACTIVE_FG%
inactiveBackground=%INACTIVE_BG%
inactiveBlend=161,169,177
inactiveForeground=%INACTIVE_FG%

[General]
ColorScheme=JellyfinDesktop
Name=Jellyfin Desktop
)";

struct org_kde_kwin_server_decoration_palette_manager* g_palette_manager = nullptr;
struct org_kde_kwin_server_decoration_palette* g_palette = nullptr;
std::string g_colors_dir;
std::string g_colors_path;  // Current file path (for cleanup)

// sRGB relative luminance (BT.709 coefficients)
bool isDark(uint8_t r, uint8_t g, uint8_t b) {
    double lum = 0.2126 * (r / 255.0) + 0.7152 * (g / 255.0) + 0.0722 * (b / 255.0);
    return lum < 0.5;
}

void replaceAll(std::string& s, const char* token, const char* value) {
    size_t tlen = strlen(token);
    size_t vlen = strlen(value);
    size_t pos = 0;
    while ((pos = s.find(token, pos)) != std::string::npos) {
        s.replace(pos, tlen, value);
        pos += vlen;
    }
}

std::string colorSchemeDir() {
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0])
        return std::string(runtime) + "/jellyfin-desktop";
    return {};
}

// KWin caches palette by path string — same path is a no-op even if file
// contents changed.  Color is encoded in the filename to force a new path.
bool writeColorScheme(uint8_t r, uint8_t g, uint8_t b, const std::string& path) {
    char bg[32];
    snprintf(bg, sizeof(bg), "%d,%d,%d", r, g, b);

    const char* active_fg;
    const char* inactive_fg;
    if (isDark(r, g, b)) {
        active_fg = "252,252,252";
        inactive_fg = "126,126,126";
    } else {
        active_fg = "35,38,41";
        inactive_fg = "35,38,41";
    }

    std::string content(kColorSchemeTemplate);
    replaceAll(content, "%HEADER_BG%", bg);
    replaceAll(content, "%INACTIVE_BG%", bg);
    replaceAll(content, "%ACTIVE_FG%", active_fg);
    replaceAll(content, "%INACTIVE_FG%", inactive_fg);

    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        LOG_ERROR(LOG_PLATFORM, "Failed to write color scheme: %s", path.c_str());
        return false;
    }
    bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    fclose(f);
    if (!ok) {
        LOG_ERROR(LOG_PLATFORM, "Short write to color scheme: %s", path.c_str());
        remove(path.c_str());
    }
    return ok;
}

} // namespace

void initKdeDecorationPalette(SDL_Window* window) {
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    auto* display = static_cast<struct wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    auto* surface = static_cast<struct wl_surface*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));

    if (!display || !surface)
        return;

    auto registry_handler = [](void*, wl_registry* registry,
                               uint32_t name, const char* interface, uint32_t version) {
        if (strcmp(interface, org_kde_kwin_server_decoration_palette_manager_interface.name) == 0) {
            g_palette_manager = static_cast<org_kde_kwin_server_decoration_palette_manager*>(
                wl_registry_bind(registry, name,
                                 &org_kde_kwin_server_decoration_palette_manager_interface, 1));
        }
    };
    static const wl_registry_listener listener = {
        .global = +registry_handler,
        .global_remove = [](void*, wl_registry*, uint32_t) {},
    };
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, nullptr);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);

    if (!g_palette_manager) {
        LOG_INFO(LOG_PLATFORM, "KDE decoration palette protocol not available (not KWin?)");
        return;
    }

    g_palette = org_kde_kwin_server_decoration_palette_manager_create(g_palette_manager, surface);
    if (!g_palette) {
        LOG_WARN(LOG_PLATFORM, "Failed to create KDE decoration palette object");
        return;
    }

    g_colors_dir = colorSchemeDir();
    if (g_colors_dir.empty()) {
        LOG_WARN(LOG_PLATFORM, "XDG_RUNTIME_DIR not set, KDE palette disabled");
        org_kde_kwin_server_decoration_palette_release(g_palette);
        g_palette = nullptr;
        return;
    }
    mkdir(g_colors_dir.c_str(), 0700);

    LOG_INFO(LOG_PLATFORM, "KDE decoration palette ready (dir: %s)", g_colors_dir.c_str());
}

void setKdeTitlebarColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_palette)
        return;

    char filename[64];
    snprintf(filename, sizeof(filename), "JellyfinDesktop-%02x%02x%02x.colors", r, g, b);
    std::string new_path = g_colors_dir + "/" + filename;

    if (new_path == g_colors_path)
        return;

    if (!writeColorScheme(r, g, b, new_path))
        return;

    if (!g_colors_path.empty())
        remove(g_colors_path.c_str());
    g_colors_path = new_path;

    org_kde_kwin_server_decoration_palette_set_palette(g_palette, g_colors_path.c_str());
}

void cleanupKdeDecorationPalette() {
    if (g_palette) {
        org_kde_kwin_server_decoration_palette_release(g_palette);
        g_palette = nullptr;
    }
    if (g_palette_manager) {
        // Manager has no destructor request in the protocol; just null it
        g_palette_manager = nullptr;
    }

    if (!g_colors_path.empty()) {
        remove(g_colors_path.c_str());
        g_colors_path.clear();
    }
}
