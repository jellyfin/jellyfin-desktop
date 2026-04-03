#include <SDL3/SDL.h>
#include <filesystem>
#include "logging.h"
#include "version.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

#include "cjson/cJSON.h"
#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/cef_version.h"
#include "include/cef_version_info.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#ifdef __APPLE__
#include "include/wrapper/cef_library_loader.h"
#include "include/cef_application_mac.h"
#include <CoreFoundation/CoreFoundation.h>

// Initialize CEF-compatible NSApplication before SDL
void initMacApplication();
// Activate window for keyboard focus after SDL window creation
void activateMacWindow(SDL_Window* window);
// Set titlebar color (transparent titlebar chrome)
void setMacTitlebarColor(uint8_t r, uint8_t g, uint8_t b);
// Show/hide traffic light buttons (close, minimize, zoom)
void setMacTrafficLightsVisible(bool visible);
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include "player/macos/media_session_macos.h"
#include "PFMoveApplication.h"
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include "context/wgl_context.h"
#include "context/opengl_frame_context.h"
#include "platform/windows_video_surface.h"
#include "platform/windows_overlay_layer.h"
#include "platform/dcomp_browser_layer.h"
#include "player/windows/media_session_windows.h"
#else
#include "context/egl_context.h"
#include "context/opengl_frame_context.h"
#include "player/mpris/media_session_mpris.h"
#include <unistd.h>  // For close()
#include "platform/event_loop_linux.h"
#ifdef HAVE_KDE_DECORATION_PALETTE
#include "platform/kde_decoration_palette.h"
#endif
#endif
#include "player/media_session.h"
#include "player/media_session_thread.h"
#include "player/video_stack.h"
#include "player/video_renderer.h"
#include "player/mpv_event_thread.h"
#include "player/video_render_controller.h"
#include "cef/cef_app.h"
#include "cef/cef_client.h"
#ifdef _WIN32
#include "cef/cef_thread.h"
#endif
#include "browser/browser_stack.h"
#include "input/input_layer.h"
#include "input/browser_layer.h"
#include "input/menu_layer.h"
#include "input/mpv_layer.h"
#include "input/window_state.h"
#include "ui/menu_overlay.h"
#include "settings.h"
#include "single_instance.h"
#include "window_geometry.h"
#include "window_activation.h"
#ifndef _WIN32
#include <csignal>
static volatile sig_atomic_t g_quit_requested = 0;
static void signalHandler(int) {
    g_quit_requested = 1;
}
#endif

// Overlay fade constants
constexpr float OVERLAY_FADE_DELAY_SEC = 1.0f;
constexpr float OVERLAY_FADE_DURATION_SEC = 0.25f;

// Map CEF cursor type to SDL system cursor
SDL_SystemCursor cefCursorToSDL(cef_cursor_type_t type) {
    switch (type) {
        case CT_POINTER: return SDL_SYSTEM_CURSOR_DEFAULT;
        case CT_CROSS: return SDL_SYSTEM_CURSOR_CROSSHAIR;
        case CT_HAND: return SDL_SYSTEM_CURSOR_POINTER;
        case CT_IBEAM: return SDL_SYSTEM_CURSOR_TEXT;
        case CT_WAIT: return SDL_SYSTEM_CURSOR_WAIT;
        case CT_HELP: return SDL_SYSTEM_CURSOR_DEFAULT;  // No help cursor in SDL
        case CT_EASTRESIZE: return SDL_SYSTEM_CURSOR_E_RESIZE;
        case CT_NORTHRESIZE: return SDL_SYSTEM_CURSOR_N_RESIZE;
        case CT_NORTHEASTRESIZE: return SDL_SYSTEM_CURSOR_NE_RESIZE;
        case CT_NORTHWESTRESIZE: return SDL_SYSTEM_CURSOR_NW_RESIZE;
        case CT_SOUTHRESIZE: return SDL_SYSTEM_CURSOR_S_RESIZE;
        case CT_SOUTHEASTRESIZE: return SDL_SYSTEM_CURSOR_SE_RESIZE;
        case CT_SOUTHWESTRESIZE: return SDL_SYSTEM_CURSOR_SW_RESIZE;
        case CT_WESTRESIZE: return SDL_SYSTEM_CURSOR_W_RESIZE;
        case CT_NORTHSOUTHRESIZE: return SDL_SYSTEM_CURSOR_NS_RESIZE;
        case CT_EASTWESTRESIZE: return SDL_SYSTEM_CURSOR_EW_RESIZE;
        case CT_NORTHEASTSOUTHWESTRESIZE: return SDL_SYSTEM_CURSOR_NESW_RESIZE;
        case CT_NORTHWESTSOUTHEASTRESIZE: return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
        case CT_COLUMNRESIZE: return SDL_SYSTEM_CURSOR_EW_RESIZE;
        case CT_ROWRESIZE: return SDL_SYSTEM_CURSOR_NS_RESIZE;
        case CT_MOVE: return SDL_SYSTEM_CURSOR_MOVE;
        case CT_PROGRESS: return SDL_SYSTEM_CURSOR_PROGRESS;
        case CT_NODROP: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
        case CT_NOTALLOWED: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
        case CT_GRAB: return SDL_SYSTEM_CURSOR_POINTER;
        case CT_GRABBING: return SDL_SYSTEM_CURSOR_POINTER;
        default: return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

static auto _main_start = std::chrono::steady_clock::now();
inline long _ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _main_start).count(); }

// Simple JSON string value extractor (handles escaped quotes)
std::string jsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;  // Skip opening quote
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;  // Skip escape char
        }
        result += json[pos++];
    }
    return result;
}

// Extract integer from JSON
int64_t jsonGetInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string num;
    while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '-')) {
        num += json[pos++];
    }
    return num.empty() ? 0 : std::stoll(num);
}

// Extract integer from JSON with default value
int jsonGetIntDefault(const std::string& json, const std::string& key, int defaultVal) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return defaultVal;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return defaultVal;
    bool negative = false;
    if (json[pos] == '-') { negative = true; pos++; }
    int val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return negative ? -val : val;
}

// Extract double from JSON (with optional hasValue output)
double jsonGetDouble(const std::string& json, const std::string& key, bool* hasValue = nullptr) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        if (hasValue) *hasValue = false;
        return 0.0;
    }
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string num;
    while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '-' || json[pos] == '.' || json[pos] == 'e' || json[pos] == 'E' || json[pos] == '+')) {
        num += json[pos++];
    }
    if (hasValue) *hasValue = !num.empty();
    return num.empty() ? 0.0 : std::stod(num);
}

// Extract first element from JSON array of strings
std::string jsonGetFirstArrayString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    while (pos < json.size() && json[pos] != '[') pos++;
    if (pos >= json.size()) return "";
    pos++;  // Skip [
    while (pos < json.size() && json[pos] != '"' && json[pos] != ']') pos++;
    if (pos >= json.size() || json[pos] == ']') return "";
    pos++;  // Skip opening quote
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) pos++;
        result += json[pos++];
    }
    return result;
}

MediaMetadata parseMetadataJson(const std::string& json) {
    MediaMetadata meta;
    meta.title = jsonGetString(json, "Name");
    // For episodes, use SeriesName as artist; for audio, use Artists array
    meta.artist = jsonGetString(json, "SeriesName");
    if (meta.artist.empty()) {
        meta.artist = jsonGetFirstArrayString(json, "Artists");
    }
    // For episodes, use SeasonName as album; for audio, use Album
    meta.album = jsonGetString(json, "SeasonName");
    if (meta.album.empty()) {
        meta.album = jsonGetString(json, "Album");
    }
    meta.track_number = static_cast<int>(jsonGetInt(json, "IndexNumber"));
    // RunTimeTicks is in 100ns units, convert to microseconds
    meta.duration_us = jsonGetInt(json, "RunTimeTicks") / 10;
    // Detect media type from Type field
    std::string type = jsonGetString(json, "Type");
    if (type == "Audio") {
        meta.media_type = MediaType::Audio;
    } else if (type == "Movie" || type == "Episode" || type == "Video" || type == "MusicVideo") {
        meta.media_type = MediaType::Video;
    }
    return meta;
}

// Parse CSS hex color (#rgb or #rrggbb) into r, g, b components.
// Returns false if the string is not a valid hex color.
static bool parseHexColor(const std::string& color, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (color.empty() || color[0] != '#') return false;
    if (color.size() == 7) {
        unsigned int hex = 0;
        if (sscanf(color.c_str() + 1, "%06x", &hex) != 1) return false;
        r = (hex >> 16) & 0xFF;
        g = (hex >> 8) & 0xFF;
        b = hex & 0xFF;
        return true;
    }
    if (color.size() == 4) {
        unsigned int hex = 0;
        if (sscanf(color.c_str() + 1, "%03x", &hex) != 1) return false;
        r = ((hex >> 8) & 0xF) * 0x11;
        g = ((hex >> 4) & 0xF) * 0x11;
        b = (hex & 0xF) * 0x11;
        return true;
    }
    return false;
}

static void setTitlebarColor([[maybe_unused]] SDL_Window* window, uint8_t r, uint8_t g, uint8_t b) {
#ifdef _WIN32
    HWND hwnd = (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd) {
        // DWMWA_CAPTION_COLOR (35) requires Windows 11 build 22000+; silently ignored on older
        COLORREF color = RGB(r, g, b);
        DwmSetWindowAttribute(hwnd, 35, &color, sizeof(color));
    }
#elif defined(__APPLE__)
    setMacTitlebarColor(r, g, b);
#elif defined(HAVE_KDE_DECORATION_PALETTE)
    setKdeTitlebarColor(r, g, b);
#endif
}

static SDL_HitTestResult SDLCALL windowHitTest(SDL_Window* win, const SDL_Point* area, void* data) {
    constexpr int EDGE = 5;  // pixels
    int w, h;
    SDL_GetWindowSize(win, &w, &h);

    bool left   = area->x < EDGE;
    bool right  = area->x >= w - EDGE;
    bool top    = area->y < EDGE;
    bool bottom = area->y >= h - EDGE;

    SDL_HitTestResult result = SDL_HITTEST_NORMAL;
    if (top && left)          result = SDL_HITTEST_RESIZE_TOPLEFT;
    else if (top && right)    result = SDL_HITTEST_RESIZE_TOPRIGHT;
    else if (bottom && left)  result = SDL_HITTEST_RESIZE_BOTTOMLEFT;
    else if (bottom && right) result = SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    else if (top)             result = SDL_HITTEST_RESIZE_TOP;
    else if (bottom)          result = SDL_HITTEST_RESIZE_BOTTOM;
    else if (left)            result = SDL_HITTEST_RESIZE_LEFT;
    else if (right)           result = SDL_HITTEST_RESIZE_RIGHT;

    return result;
}

int main(int argc, char* argv[]) {
    // CEF subprocesses inherit this env var - skip our arg parsing entirely
    bool is_cef_subprocess = (getenv("JELLYFIN_CEF_SUBPROCESS") != nullptr);

    // Load saved settings first, then let CLI flags override
    SDL_LogPriority log_level = SDL_LOG_PRIORITY_DEBUG;
    bool use_dmabuf = true;
    bool disable_gpu_compositing = false;
    int remote_debugging_port = 0;
    std::string hwdec_str = "auto-safe";
    std::string audio_passthrough_str;
    bool audio_exclusive = false;
    std::string audio_channels_str;
    bool player_mode = false;
    std::vector<std::string> player_playlist;
    std::string player_url;
    if (!is_cef_subprocess) {
        // Load persisted settings as defaults
        Settings::instance().load();
        auto& saved = Settings::instance();
        if (!saved.hwdec().empty()) hwdec_str = saved.hwdec();
        if (!saved.audioPassthrough().empty()) audio_passthrough_str = saved.audioPassthrough();
        audio_exclusive = saved.audioExclusive();
        if (!saved.audioChannels().empty()) audio_channels_str = saved.audioChannels();
        disable_gpu_compositing = saved.disableGpuCompositing();

        // Parse CLI arguments (override saved settings)
        const char* log_level_str = nullptr;
        const char* log_file_path = nullptr;
        std::string saved_log_level = saved.logLevel();
        if (!saved_log_level.empty()) log_level_str = saved_log_level.c_str();
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                printf("Usage: jellyfin-desktop [options]\n"
                       "       jellyfin-desktop --player [options] <file|url>...\n"
                       "\nOptions:\n"
                       "  -h, --help              Show this help message\n"
                       "  -v, --version           Show version information\n"
                       "  --log-level <level>     Set log level (verbose|debug|info|warn|error)\n"
                       "  --log-file <path>       Write logs to file (with timestamps)\n"
#if !defined(__APPLE__) && !defined(_WIN32)
                       "  --disable-dmabuf             Disable DMA-BUF zero-copy CEF rendering\n"
#endif
                       "  --disable-gpu-compositing  Disable Chromium GPU compositing\n"
                       "  --hwdec <mode>          Set mpv hardware decoding mode (default: auto-safe)\n"
                       "  --audio-passthrough <codecs>  Enable audio passthrough (e.g. ac3,dts-hd,eac3,truehd)\n"
                       "  --audio-exclusive       Use exclusive audio output mode\n"
                       "  --audio-channels <layout>  Set audio channel layout (e.g. stereo, 5.1, 7.1)\n"
                       "  --remote-debug-port <port>  Enable Chrome remote debugging on port (1024-65535)\n"
                       "  --player                Standalone player mode (play files/URLs directly)\n"
                       );
                return 0;
            } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
                printf("jellyfin-desktop %s\n", APP_VERSION_STRING);
                printf("  built " __DATE__ " " __TIME__ "\n");
                printf("CEF %s\n", CEF_VERSION);
                return 0;
            } else if (strcmp(argv[i], "--log-level") == 0) {
                log_level_str = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i] : "";
            } else if (strncmp(argv[i], "--log-level=", 12) == 0) {
                log_level_str = argv[i] + 12;
            } else if (strcmp(argv[i], "--log-file") == 0) {
                log_file_path = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i] : "";
            } else if (strncmp(argv[i], "--log-file=", 11) == 0) {
                log_file_path = argv[i] + 11;
            } else if (strcmp(argv[i], "--disable-gpu-compositing") == 0) {
                disable_gpu_compositing = true;
            } else if (strcmp(argv[i], "--disable-dmabuf") == 0) {
                use_dmabuf = false;
            } else if (strcmp(argv[i], "--hwdec") == 0) {
                hwdec_str = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i] : "auto-safe";
            } else if (strncmp(argv[i], "--hwdec=", 8) == 0) {
                hwdec_str = argv[i] + 8;
            } else if (strcmp(argv[i], "--audio-passthrough") == 0) {
                audio_passthrough_str = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i] : "";
            } else if (strncmp(argv[i], "--audio-passthrough=", 20) == 0) {
                audio_passthrough_str = argv[i] + 20;
            } else if (strcmp(argv[i], "--audio-exclusive") == 0) {
                audio_exclusive = true;
            } else if (strcmp(argv[i], "--audio-channels") == 0) {
                audio_channels_str = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i] : "";
            } else if (strncmp(argv[i], "--audio-channels=", 17) == 0) {
                audio_channels_str = argv[i] + 17;
            } else if (strcmp(argv[i], "--remote-debug-port") == 0) {
                const char* val = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i] : "";
                remote_debugging_port = atoi(val);
            } else if (strncmp(argv[i], "--remote-debug-port=", 20) == 0) {
                remote_debugging_port = atoi(argv[i] + 20);
            } else if (strcmp(argv[i], "--player") == 0) {
                player_mode = true;
            } else if (argv[i][0] == '-') {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                return 1;
            } else {
                // Positional argument — treat as playlist item in player mode
                player_playlist.push_back(argv[i]);
            }
        }

        // Validate and apply options (empty = use default/no-op)
        if (log_level_str && log_level_str[0]) {
            int level = parseLogLevel(log_level_str);
            if (level < 0) {
                fprintf(stderr, "Invalid log level: %s\n", log_level_str);
                return 1;
            }
            log_level = static_cast<SDL_LogPriority>(level);
        }
        if (log_file_path && log_file_path[0]) {
            g_log_file = fopen(log_file_path, "w");
            if (!g_log_file) {
                fprintf(stderr, "Failed to open log file: %s\n", log_file_path);
                return 1;
            }
        }
        if (player_mode && player_playlist.empty()) {
            fprintf(stderr, "Error: --player requires at least one file or URL\n");
            return 1;
        }
        if (!player_playlist.empty() && !player_mode) {
            fprintf(stderr, "Unknown arguments. See --help for usage.\n");
            return 1;
        }

        if (player_mode) {
            cJSON* arr = cJSON_CreateArray();
            for (const auto& item : player_playlist) {
                cJSON_AddItemToArray(arr, cJSON_CreateString(item.c_str()));
            }
            char* json = cJSON_PrintUnformatted(arr);
            if (!json) {
                cJSON_Delete(arr);
                fprintf(stderr, "Error: failed to serialize playlist\n");
                return 1;
            }
            CefString encoded = CefURIEncode(json, false);
            player_url = "app://resources/player.html#" + encoded.ToString();
            cJSON_free(json);
            cJSON_Delete(arr);
        }

        initLogging(log_level);

        // Normalize audio passthrough: dts-hd subsumes dts
        if (!audio_passthrough_str.empty() && audio_passthrough_str.find("dts-hd") != std::string::npos) {
            std::string input = audio_passthrough_str;
            audio_passthrough_str.clear();
            size_t pos = 0;
            while (pos < input.size()) {
                size_t comma = input.find(',', pos);
                if (comma == std::string::npos) comma = input.size();
                std::string codec = input.substr(pos, comma - pos);
                if (codec != "dts") {
                    if (!audio_passthrough_str.empty()) audio_passthrough_str += ',';
                    audio_passthrough_str += codec;
                }
                pos = comma + 1;
            }
        }

        // Startup banner
        LOG_INFO(LOG_MAIN, "jellyfin-desktop " APP_VERSION_STRING " built " __DATE__ " " __TIME__);
        LOG_INFO(LOG_MAIN, "CEF " CEF_VERSION);

#if !defined(__APPLE__) && !defined(_WIN32)
        if (!use_dmabuf) {
            LOG_INFO(LOG_MAIN, "DMA-BUF zero-copy CEF rendering disabled via --disable-dmabuf");
        }
#endif
        if (!audio_passthrough_str.empty())
            LOG_INFO(LOG_MAIN, "Audio passthrough: %s", audio_passthrough_str.c_str());
        if (audio_exclusive)
            LOG_INFO(LOG_MAIN, "Audio exclusive mode enabled");
        if (!audio_channels_str.empty())
            LOG_INFO(LOG_MAIN, "Audio channels: %s", audio_channels_str.c_str());
    }
    const char* hwdec = hwdec_str.c_str();
    const char* audio_passthrough = audio_passthrough_str.empty() ? nullptr : audio_passthrough_str.c_str();
    const char* audio_channels = audio_channels_str.empty() ? nullptr : audio_channels_str.c_str();

#ifdef __APPLE__
    // macOS: Get executable path early for CEF framework loading
    char exe_buf[PATH_MAX];
    uint32_t exe_size = sizeof(exe_buf);
    std::filesystem::path exe_path;
    if (_NSGetExecutablePath(exe_buf, &exe_size) == 0) {
        exe_path = std::filesystem::canonical(exe_buf).parent_path();
    } else {
        exe_path = std::filesystem::current_path();
    }

    // macOS: Load CEF framework dynamically (required - linking alone isn't enough)
    // Check if running from app bundle (exe is in Contents/MacOS/) or dev build
    std::filesystem::path cef_framework_path;
    if (exe_path.parent_path().filename() == "Contents") {
        // App bundle: framework is at ../Frameworks/
        cef_framework_path = exe_path.parent_path() / "Frameworks";

        // Point Vulkan loader to bundled MoltenVK ICD (so it works without system Vulkan)
        auto icd_path = exe_path.parent_path() / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json";
        setenv("VK_DRIVER_FILES", icd_path.string().c_str(), 0);      // modern name (1.3.234+)
        setenv("VK_ICD_FILENAMES", icd_path.string().c_str(), 0);     // legacy fallback
    } else {
        // Dev build: framework is at ./Frameworks/
        cef_framework_path = exe_path / "Frameworks";
    }
    std::string framework_lib = (cef_framework_path /
                                 "Chromium Embedded Framework.framework" /
                                 "Chromium Embedded Framework").string();
    LOG_INFO(LOG_CEF, "Loading CEF from: %s", framework_lib.c_str());
    if (!cef_load_library(framework_lib.c_str())) {
        LOG_ERROR(LOG_CEF, "Failed to load CEF framework from: %s", framework_lib.c_str());
        return 1;
    }
    LOG_INFO(LOG_CEF, "CEF framework loaded");

    // CRITICAL: Initialize CEF-compatible NSApplication BEFORE CefExecuteProcess
    // This must happen before any CEF code that might create an NSApplication
    initMacApplication();
#endif

    // Verify runtime CEF matches compile-time version exactly
    // (must be after cef_load_library on macOS, which populates function pointers)
    if (cef_version_info(0) != CEF_VERSION_MAJOR ||
        cef_version_info(1) != CEF_VERSION_MINOR ||
        cef_version_info(2) != CEF_VERSION_PATCH ||
        cef_version_info(3) != CEF_COMMIT_NUMBER ||
        cef_version_info(4) != CHROME_VERSION_MAJOR ||
        cef_version_info(5) != CHROME_VERSION_MINOR ||
        cef_version_info(6) != CHROME_VERSION_BUILD ||
        cef_version_info(7) != CHROME_VERSION_PATCH) {
        LOG_WARN(LOG_CEF, "Runtime CEF %d.%d.%d (chromium %d.%d.%d.%d) does not match compiled "
                 CEF_VERSION,
                 cef_version_info(0), cef_version_info(1), cef_version_info(2),
                 cef_version_info(4), cef_version_info(5), cef_version_info(6), cef_version_info(7));
    }

    // Mark so CEF subprocesses skip arg parsing
    if (!is_cef_subprocess) {
#ifdef _WIN32
        _putenv_s("JELLYFIN_CEF_SUBPROCESS", "1");
#else
        setenv("JELLYFIN_CEF_SUBPROCESS", "1", 1);
#endif

        // Clear args so CEF doesn't see our custom args
        argc = 1;
        argv[1] = nullptr;
    }

    // CEF initialization
#ifdef _WIN32
    CefMainArgs main_args(GetModuleHandle(NULL));
#else
    CefMainArgs main_args(argc, argv);
#endif
    CefRefPtr<App> app(new App());
    app->SetDisableGpuCompositing(disable_gpu_compositing);

    LOG_DEBUG(LOG_CEF, "Calling CefExecuteProcess...");
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    LOG_DEBUG(LOG_CEF, "CefExecuteProcess returned: %d", exit_code);
    if (exit_code >= 0) {
        return exit_code;
    }

#ifdef _WIN32
    // Create a job object so CEF subprocesses are killed when we exit.
    // KILL_ON_JOB_CLOSE ensures children die even on _exit() or crash.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        AssignProcessToJobObject(job, GetCurrentProcess());
    }
#endif

    // Single-instance check: signal existing instance to raise, then exit
    // Skip in player mode — allow multiple instances for testing
    if (!player_mode && trySignalExisting()) {
        return 0;
    }

#if defined(__APPLE__) && defined(NDEBUG)
    // In release builds, offer to move app to /Applications (clears quarantine)
    PFMoveToApplicationsFolderIfNecessary();
#endif

    SDL_SetAppMetadata("Jellyfin Desktop", nullptr, "org.jellyfin.JellyfinDesktop");

    // SDL initialization with OpenGL (for main surface CEF overlay)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR(LOG_MAIN, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Register custom event for cross-thread main loop wake-up
    static Uint32 SDL_EVENT_WAKE = SDL_RegisterEvents(1);
#if !defined(__APPLE__) && !defined(_WIN32)
    EventLoopWake eventLoopWake;
#endif
    auto wakeMainLoop = [
#if !defined(__APPLE__) && !defined(_WIN32)
        &eventLoopWake
#endif
    ]() {
#if !defined(__APPLE__) && !defined(_WIN32)
        eventLoopWake.wake();
#else
        SDL_Event event{};
        event.type = SDL_EVENT_WAKE;
        SDL_PushEvent(&event);
#endif
    };

#ifndef _WIN32
    // macOS/Linux: CEF uses external_message_pump, so we need to wake the main loop
    // when CEF schedules work (otherwise SDL_WaitEvent blocks indefinitely)
    App::SetWakeCallback(wakeMainLoop);
#endif

    // Single-instance listener: raise window when another instance signals us
    std::mutex raise_mutex;
    std::string pending_activation_token;
    bool raise_requested = false;
    if (!player_mode) {
        startListener([&raise_mutex, &pending_activation_token, &raise_requested, &wakeMainLoop](const std::string& token) {
            std::lock_guard<std::mutex> lock(raise_mutex);
            pending_activation_token = token;
            raise_requested = true;
            wakeMainLoop();
        });
    }

    // Create window at saved geometry to avoid resize flash on startup
    const auto& saved_geom = Settings::instance().windowGeometry();
    int width = (saved_geom.width > 0) ? saved_geom.width : 1280;
    int height = (saved_geom.height > 0) ? saved_geom.height : 720;

    SDL_WindowFlags win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef _WIN32
    // Start hidden so DWM attributes and DComp are set before the window is shown,
    // avoiding a flash of default titlebar color and black client area.
    // MAXIMIZED is deferred as pending_flags and applied on ShowWindow.
    win_flags |= SDL_WINDOW_HIDDEN;
    if (saved_geom.maximized) win_flags |= SDL_WINDOW_MAXIMIZED;
#else
    // Wayland: the mpv video subsurface sits below the main EGL surface.
    // Without SDL_WINDOW_TRANSPARENT, SDL sets a full opaque region on the
    // parent wl_surface, telling the compositor it can skip everything below.
    // In fullscreen the compositor takes this literally (direct scanout),
    // making the video layer invisible.
    {
        const char* driver = SDL_GetCurrentVideoDriver();
        if (driver && strcmp(driver, "wayland") == 0)
            win_flags |= SDL_WINDOW_TRANSPARENT;
    }
#endif

    SDL_Window* window = SDL_CreateWindow(
        "Jellyfin Desktop",
        width, height,
        win_flags
    );

    if (!window) {
        LOG_ERROR(LOG_MAIN, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_SetWindowHitTest(window, windowHitTest, nullptr);
    SDL_StartTextInput(window);

#ifdef _WIN32
    // Set window background and titlebar to #101010 before first render
    {
        HWND hwnd = (HWND)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (hwnd) {
            COLORREF bg = RGB(0x10, 0x10, 0x10);
            SetClassLongPtrA(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)CreateSolidBrush(bg));
            // Dark mode titlebar (prevents light titlebar flash on Win10+)
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
            // Explicit caption color (Win11+, silently ignored on older)
            DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &bg, sizeof(bg));
        }
    }
#elif defined(__APPLE__)
    // macOS: initial titlebar color is set in activateMacWindow
#endif

    // Restore saved window geometry (settings already loaded during init)
    restoreWindowGeometry(window);
    SDL_GetWindowSize(window, &width, &height);

#ifdef __APPLE__
    // Window activation is deferred until first WINDOW_EXPOSED event
    // to ensure the window is actually visible before activating
#endif

    AudioConfig audioConfig;
    audioConfig.spdif = audio_passthrough;
    audioConfig.channels = audio_channels;
    audioConfig.exclusive = audio_exclusive;

#ifdef __APPLE__
    // Create video stack
    VideoStack videoStack = VideoStack::create(window, width, height, hwdec, audioConfig);
    if (!videoStack.player || !videoStack.renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    MpvPlayer* mpv = videoStack.player.get();
    VideoRenderer& videoRenderer = *videoStack.renderer;
    bool has_video = false;
    bool video_needs_rerender = false;
    double current_playback_rate = 1.0;

    // HiDPI setup for CEF overlays
    float initial_scale = SDL_GetWindowDisplayScale(window);
    int physical_width = static_cast<int>(width * initial_scale);
    int physical_height = static_cast<int>(height * initial_scale);
    LOG_INFO(LOG_WINDOW, "macOS HiDPI: scale=%.2f logical=%dx%d physical=%dx%d",
             initial_scale, width, height, physical_width, physical_height);

    // Compositor context for BrowserEntry init
    CompositorContext compositor_ctx;
    compositor_ctx.window = window;
#elif defined(_WIN32)
    // Windows: Initialize WGL context for OpenGL rendering
    WGLContext wgl;
    if (!wgl.init(window)) {
        LOG_ERROR(LOG_GL, "WGL init failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Create video stack
    VideoStack videoStack = VideoStack::create(window, width, height, hwdec, audioConfig);
    if (!videoStack.player || !videoStack.renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    MpvPlayer* mpv = videoStack.player.get();
    VideoRenderer& videoRenderer = *videoStack.renderer;
    bool has_video = false;
    bool video_needs_rerender = false;
    double current_playback_rate = 1.0;
    OpenGLFrameContext frameContext(&wgl);

    // Render an initial #101010 frame so the WGL surface matches the window background
    // through the DComp visual tree before CEF's first paint
    frameContext.beginFrame(0x10 / 255.0f, 1.0f);
    frameContext.endFrame();

    // Create DComp overlay layer for CEF rendering (above video in DComp tree)
    // WGL context may have been displaced by D3D11/Vulkan device creation
    wgl.makeCurrent();
    WindowsOverlayLayer overlayLayer;
    bool has_overlay = false;

    // DComp browser layers for zero-copy shared texture rendering
    DCompBrowserLayer mainBrowserLayer;
    DCompBrowserLayer overlayBrowserLayer;
    bool has_dcomp_browsers = false;

    if (videoStack.surface) {
        // Initialize DComp browser layers (one per browser, each with own swap chain)
        // Visual tree: video_visual_ → main_visual_ → overlay_visual_
        bool main_ok = mainBrowserLayer.init(
            videoStack.surface->dcompDevice(),
            videoStack.surface->videoVisual(),
            videoStack.surface->d3dDevice(),
            videoStack.surface->d3dContext(),
            &videoStack.surface->d3dMutex(),
            width, height);

        if (main_ok) {
            bool overlay_ok = overlayBrowserLayer.init(
                videoStack.surface->dcompDevice(),
                mainBrowserLayer.visual(),  // overlay is child of main
                videoStack.surface->d3dDevice(),
                videoStack.surface->d3dContext(),
                &videoStack.surface->d3dMutex(),
                width, height);

            if (overlay_ok) {
                mainBrowserLayer.show();
                overlayBrowserLayer.show();
                has_dcomp_browsers = true;
                LOG_INFO(LOG_PLATFORM, "DComp browser layers initialized (zero-copy shared textures)");
            } else {
                LOG_WARN(LOG_PLATFORM, "Overlay DComp browser layer init failed");
                mainBrowserLayer.cleanup();
            }
        } else {
            LOG_WARN(LOG_PLATFORM, "Main DComp browser layer init failed");
        }

        // Fall back to GL overlay if DComp browser layers failed
        if (!has_dcomp_browsers) {
            has_overlay = overlayLayer.init(
                videoStack.surface->dcompDevice(),
                videoStack.surface->videoVisual(),
                videoStack.surface->d3dDevice(),
                videoStack.surface->d3dContext(),
                &videoStack.surface->d3dMutex(),
                &wgl, width, height);
            if (has_overlay) {
                overlayLayer.show();
            } else {
                LOG_WARN(LOG_GL, "DComp overlay init failed, CEF will render to WGL framebuffer");
            }
        }
    }

    // All DWM attributes, DComp visuals, and initial frames are set up —
    // show the window now to avoid flashing default titlebar/background
    SDL_ShowWindow(window);

    // Re-read dimensions after show — maximize may have changed the window size
    SDL_GetWindowSize(window, &width, &height);

    // Compositor context for BrowserEntry init
    CompositorContext compositor_ctx;
    compositor_ctx.gl_context = &wgl;
    int physical_width = width;
    int physical_height = height;
#else
    // Linux: Initialize EGL context for OpenGL rendering
    EGLContext_ egl;
    if (!egl.init(window)) {
        LOG_ERROR(LOG_GL, "EGL init failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Probe dmabuf support before CefInitialize — CEF's shared texture path
    // requires GBM → EGL → GL dmabuf import of ARGB8888. Drivers that lack
    // this (e.g. NVIDIA proprietary) cause OnAcceleratedPaint to never fire.
    // Disabling dmabuf falls back to OnPaint with CPU pixel buffers while
    // keeping GPU compositing (GPU composites internally, reads back pixels).
    if (use_dmabuf && !egl.supportsDmaBufImport()) {
        LOG_INFO(LOG_MAIN, "EGL does not support ARGB8888 dmabuf import; disabling dmabuf");
        use_dmabuf = false;
    }

    // Create video stack (detects Wayland vs X11 internally)
    VideoStack videoStack = VideoStack::create(window, width, height, &egl, hwdec, audioConfig);
    if (!videoStack.player || !videoStack.renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    MpvPlayer* mpv = videoStack.player.get();
    VideoRenderer& videoRenderer = *videoStack.renderer;
    bool has_video = false;
    bool video_needs_rerender = false;
    double current_playback_rate = 1.0;

    // Frame context (same for both Wayland and X11 - both use EGL)
    OpenGLFrameContext frameContext(&egl);

    // Render an initial #101010 frame so the window doesn't flash the default background
    frameContext.beginFrame(16.0f / 255.0f, 1.0f);
    frameContext.endFrame();

    // Compositor context for BrowserEntry init
    // Use SDL physical size - resize handler will update when Wayland reports actual scale
    int physical_width, physical_height;
    SDL_GetWindowSizeInPixels(window, &physical_width, &physical_height);
    LOG_INFO(LOG_WINDOW, "HiDPI: logical=%dx%d physical=%dx%d",
             width, height, physical_width, physical_height);

    CompositorContext compositor_ctx;
    compositor_ctx.gl_context = &egl;

    eventLoopWake.init(window);
#endif

    initWindowActivation(window);
#ifdef HAVE_KDE_DECORATION_PALETTE
    initKdeDecorationPalette(window);
    setKdeTitlebarColor(0x10, 0x10, 0x10);
#endif

    // CEF settings (CefThread sets external_message_pump)
    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.remote_debugging_port = remote_debugging_port;

#ifdef __APPLE__
    // macOS: Set framework path (cef_framework_path set earlier during CEF loading)
    CefString(&settings.framework_dir_path).FromString((cef_framework_path / "Chromium Embedded Framework.framework").string());
    // Use main executable as subprocess - it handles CefExecuteProcess early
    CefString(&settings.browser_subprocess_path).FromString((exe_path / "jellyfin-desktop").string());
#elif defined(_WIN32)
    // Windows: Get exe path
    wchar_t exe_buf[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    std::filesystem::path exe_path = std::filesystem::path(exe_buf).parent_path();
    CefString(&settings.resources_dir_path).FromString(exe_path.string());
    CefString(&settings.locales_dir_path).FromString((exe_path / "locales").string());
#else
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe").parent_path();
#ifdef CEF_RESOURCES_DIR
    CefString(&settings.resources_dir_path).FromString(CEF_RESOURCES_DIR);
    CefString(&settings.locales_dir_path).FromString(CEF_RESOURCES_DIR "/locales");
#else
    CefString(&settings.resources_dir_path).FromString(exe_path.string());
    CefString(&settings.locales_dir_path).FromString((exe_path / "locales").string());
#endif
#endif

    // Cache path (canonicalize to resolve symlinks like /home -> /var/home on Fedora Kinoite)
    std::filesystem::path cache_path;
#ifdef _WIN32
    if (const char* appdata = std::getenv("LOCALAPPDATA")) {
        cache_path = std::filesystem::path(appdata) / "jellyfin-desktop";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        cache_path = std::filesystem::path(home) / "Library" / "Caches" / "jellyfin-desktop";
    }
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        cache_path = std::filesystem::path(xdg) / "jellyfin-desktop";
    } else if (const char* home = std::getenv("HOME")) {
        cache_path = std::filesystem::path(home) / ".cache" / "jellyfin-desktop";
    }
#endif
    if (!cache_path.empty() && !player_mode) {
        std::filesystem::create_directories(cache_path);
        // Canonicalize after creating to resolve symlinks (CEF compares paths strictly)
        std::error_code ec;
        auto canonical_path = std::filesystem::canonical(cache_path, ec);
        if (!ec) cache_path = canonical_path;
        CefString(&settings.root_cache_path).FromString(cache_path.string());
        CefString(&settings.cache_path).FromString((cache_path / "cache").string());
    }

    // Capture stderr before CEF starts (routes Chromium logs through SDL)
    initStderrCapture();

#ifdef __APPLE__
    // Pre-create Metal compositors BEFORE CefInitialize to avoid startup delay
    // Metal device/pipeline/texture creation takes time; do it while CEF init runs
    std::unique_ptr<MetalCompositor> overlay_compositor;
    if (!player_mode) {
        overlay_compositor = std::make_unique<MetalCompositor>();
        overlay_compositor->init(window, physical_width, physical_height);
        LOG_DEBUG(LOG_COMPOSITOR, "Pre-created overlay Metal compositor");
    }

    auto main_compositor = std::make_unique<MetalCompositor>();
    main_compositor->init(window, physical_width, physical_height);
    LOG_DEBUG(LOG_COMPOSITOR, "Pre-created main Metal compositor");

    // macOS: Use external_message_pump on main thread (CEF doesn't handle separate thread well)
    settings.external_message_pump = true;
    if (!CefInitialize(main_args, settings, app, nullptr)) {
        LOG_ERROR(LOG_CEF, "CefInitialize failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    LOG_INFO(LOG_CEF, "CEF context initialized");
#elif defined(_WIN32)
    // Windows: Start CEF on dedicated thread
    CefThread cefThread;
    if (!cefThread.start(main_args, settings, app)) {
        LOG_ERROR(LOG_CEF, "CefThread start failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
#else
    // Linux: Use external_message_pump on main thread
    // Chromium assumes the main process thread is the UI thread on Linux;
    // running CefInitialize on a dedicated thread causes subtle corruption.
    settings.external_message_pump = true;
    app->SetDisableGpuCompositing(disable_gpu_compositing);
    if (!CefInitialize(main_args, settings, app, nullptr)) {
        LOG_ERROR(LOG_CEF, "CefInitialize failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    LOG_INFO(LOG_CEF, "CEF context initialized");
#endif

#ifndef _WIN32
    // Install after CefInitialize (Chromium overwrites signal handlers during init)
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#endif

    // Browser stack manages all browsers and their paint buffers
    BrowserStack browsers;

// TODO: refactor this ugliness
//   true:  mac/win must be this else be black window
//   false: might fix occasional black screen on linux ?
#if !defined(__APPLE__) && !defined(_WIN32)
    std::atomic<bool> paint_size_matched{false};  // No paint yet — keep loop active until first frame
#else
    std::atomic<bool> paint_size_matched{true};
#endif

    // Player command queue
    struct PlayerCmd {
        std::string cmd;
        std::string url;
        int intArg;
        double doubleArg;
        std::string metadata;  // JSON for load command
    };
    std::mutex cmd_mutex;
    std::vector<PlayerCmd> pending_cmds;

    // Initialize media session with platform backend
    MediaSession mediaSession;
#ifdef __APPLE__
    mediaSession.addBackend(createMacOSMediaBackend(&mediaSession));
#elif defined(_WIN32)
    mediaSession.addBackend(createWindowsMediaBackend(&mediaSession, window));
#else
    std::string mpris_suffix;
    if (player_mode) {
        mpris_suffix = ".player" + std::to_string(getpid());
    }
    mediaSession.addBackend(createMprisBackend(&mediaSession, mpris_suffix));
#endif
    MediaSessionThread mediaSessionThread;
    mediaSessionThread.start(&mediaSession);
    mediaSession.onPlay = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "play", 0, 0.0});
    };
    mediaSession.onPause = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "pause", 0, 0.0});
    };
    mediaSession.onPlayPause = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "play_pause", 0, 0.0});
    };
    mediaSession.onStop = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "stop", 0, 0.0});
    };
    mediaSession.onSeek = [&](int64_t position_us) {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_seek", "", static_cast<int>(position_us / 1000), 0.0});
    };
    mediaSession.onNext = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "next", 0, 0.0});
    };
    mediaSession.onPrevious = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "previous", 0, 0.0});
    };
    mediaSession.onRaise = [&]() {
        SDL_RaiseWindow(window);
    };
    mediaSession.onSetRate = [&](double rate) {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_rate", "", 0, rate});
    };

    // Overlay browser state
    enum class OverlayState { SHOWING, WAITING, FADING, HIDDEN };
    OverlayState overlay_state = player_mode ? OverlayState::HIDDEN : OverlayState::SHOWING;
    std::chrono::steady_clock::time_point overlay_fade_start;
    float overlay_browser_alpha = 1.0f;
    float clear_color = player_mode ? 0.0f : 16.0f / 255.0f;  // #101010 until fade begins (player mode: black immediately)
    std::string pending_server_url;

    // Titlebar color is locked to #101010 until the overlay fade begins,
    // so the titlebar matches the overlay background during loading.
    bool titlebar_color_unlocked = false;
    std::string pending_titlebar_color;  // Stores color received before fade
    std::string current_theme_color;     // Last applied theme color (for restore after video)

    bool titlebar_theme_color = Settings::instance().titlebarThemeColor();
#ifdef __APPLE__
    bool transparent_titlebar = Settings::instance().transparentTitlebar();
#endif

    // Apply a hex color string (e.g. "#1c2a48") to the window titlebar
    auto applyTitlebarColor = [&](const std::string& color) {
        if (!titlebar_theme_color) return;
        uint8_t r, g, b;
        if (parseHexColor(color, r, g, b)) {
            setTitlebarColor(window, r, g, b);
        }
    };

    // Set titlebar black during video, restore theme color when leaving
    auto setVideoTitlebar = [&](bool playing) {
#ifdef __APPLE__
        if (transparent_titlebar && !playing) setMacTrafficLightsVisible(true);
#endif
        if (!titlebar_theme_color || !titlebar_color_unlocked) return;
        if (playing) {
            setTitlebarColor(window, 0, 0, 0);
        } else if (!current_theme_color.empty()) {
            applyTitlebarColor(current_theme_color);
        } else {
            setTitlebarColor(window, 0x10, 0x10, 0x10);
        }
    };

    // Context menu overlay
    MenuOverlay menu;
    if (!menu.init()) {
        LOG_WARN(LOG_MENU, "Failed to init menu overlay (no font found)");
    }

    // Cursor state
    SDL_Cursor* current_cursor = nullptr;
    // Blank cursor for hiding (1x1 transparent) - used when CEF reports CT_NONE
    SDL_Cursor* blank_cursor = nullptr;
    if (SDL_Surface* s = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_ARGB8888)) {
        SDL_memset(s->pixels, 0, s->pitch * s->h);
        blank_cursor = SDL_CreateColorCursor(s, 0, 0);
        SDL_DestroySurface(s);
    }

    // Physical pixel size callback for HiDPI support
    // Use SDL_GetWindowSizeInPixels - reliable after first frame
    auto getPhysicalSize = [window](int& w, int& h) {
        SDL_GetWindowSizeInPixels(window, &w, &h);
    };

    // Create overlay browser entry (not used in player mode)
    CefRefPtr<OverlayClient> overlay_client;
    BrowserEntry* overlay_ptr = nullptr;
    std::unique_ptr<BrowserEntry> overlay_entry;
    if (!player_mode) {
        overlay_entry = std::make_unique<BrowserEntry>();
        overlay_ptr = overlay_entry.get();  // save pointer before move
#ifdef __APPLE__
        // Use pre-created Metal compositor (avoids startup delay)
        overlay_ptr->setCompositor(std::move(overlay_compositor));
#else
        if (!overlay_ptr->initCompositor(compositor_ctx, physical_width, physical_height)) {
            LOG_ERROR(LOG_OVERLAY, "Overlay compositor init failed");
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
#endif
        auto overlay_paint_cb = overlay_ptr->makePaintCallback();

        // Overlay browser client (for loading UI)
        overlay_client = new OverlayClient(width, height,
            [overlay_paint_cb](const void* buffer, int w, int h) {
                static bool first_overlay_paint = true;
                if (first_overlay_paint) {
                    LOG_DEBUG(LOG_OVERLAY, "first paint callback: %dx%d", w, h);
                    first_overlay_paint = false;
                }
                overlay_paint_cb(buffer, w, h);
            },
            [&](const std::string& url) {
                // loadServer callback - start loading main browser
                LOG_INFO(LOG_OVERLAY, "loadServer callback: %s", url.c_str());
                {
                    std::lock_guard<std::mutex> lock(cmd_mutex);
                    pending_server_url = url;
                }
                wakeMainLoop();
            },
            getPhysicalSize,
#if !defined(__APPLE__) && !defined(_WIN32)
            // Accelerated paint callback for overlay
            [overlay_ptr, wakeMainLoop](int fd, uint32_t stride, uint64_t modifier, int w, int h) {
                overlay_ptr->compositor->queueDmabuf(fd, stride, modifier, w, h);
                wakeMainLoop();
            }
#else
            nullptr
#endif
#ifdef __APPLE__
            // IOSurface callback for macOS accelerated paint - queue for import on main thread
            , [overlay_ptr, wakeMainLoop](void* surface, int format, int w, int h) {
                overlay_ptr->compositor->queueIOSurface(surface, format, w, h);
                wakeMainLoop();
            }
#endif
#ifdef _WIN32
            // Windows: DComp shared texture callback for overlay browser
            , has_dcomp_browsers ?
                WinSharedTexturePaintCallback([&overlayBrowserLayer](void* handle, int type, int w, int h) {
                    if (type == PET_VIEW) {
                        overlayBrowserLayer.onPaintView(static_cast<HANDLE>(handle), w, h);
                    }
                    // Overlay browser has no popups (no dropdowns in settings UI)
                }) : nullptr
#endif
        );
        overlay_ptr->client = overlay_client;
        overlay_ptr->getBrowser = [overlay_client]() { return overlay_client->browser(); };
        overlay_ptr->resizeBrowser = [overlay_client](int w, int h, int pw, int ph) { overlay_client->resize(w, h, pw, ph); };
        overlay_ptr->getInputReceiver = [overlay_client]() -> InputReceiver* { return overlay_client.get(); };
        overlay_ptr->isClosed = [overlay_client]() { return overlay_client->isClosed(); };
        overlay_ptr->input_layer = std::make_unique<BrowserLayer>(overlay_client.get());
        overlay_ptr->input_layer->setWindowSize(width, height);
        overlay_ptr->wake_main_loop = wakeMainLoop;
        // overlay_entry add is deferred until after main, so overlay composites on top
    }

    // Track who initiated fullscreen (only changes from NONE, returns to NONE on exit)
    enum class FullscreenSource { NONE, WM, CEF };
    FullscreenSource fullscreen_source = FullscreenSource::NONE;
    bool was_maximized_before_fullscreen = false;

    // Create main browser entry
    LOG_DEBUG(LOG_MAIN, "Creating main browser entry...");
    auto main_entry = std::make_unique<BrowserEntry>();
    BrowserEntry* main_ptr = main_entry.get();  // save pointer before move
#ifdef __APPLE__
    // Use pre-created Metal compositor (avoids startup delay)
    main_ptr->setCompositor(std::move(main_compositor));
#else
    LOG_DEBUG(LOG_MAIN, "Initializing main compositor (%dx%d)...", physical_width, physical_height);
    if (!main_ptr->initCompositor(compositor_ctx, physical_width, physical_height)) {
        LOG_ERROR(LOG_COMPOSITOR, "Main compositor init failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
#endif
#if !defined(__APPLE__)
    main_ptr->compositor->setScale(
        (width > 0) ? static_cast<float>(physical_width) / width : 1.0f);
#endif
    auto main_paint_cb = main_ptr->makePaintCallback();

    // Popup callbacks for accelerated paint path (dropdown menus with dmabuf/DComp)
#if !defined(__APPLE__) && !defined(_WIN32)
    PopupShowCallback popup_show_cb = [main_ptr, wakeMainLoop](bool show) {
        main_ptr->compositor->setPopupVisible(show);
        wakeMainLoop();
    };
    PopupSizeCallback popup_size_cb = [main_ptr](int x, int y, int /*w*/, int /*h*/) {
        main_ptr->compositor->setPopupPosition(x, y);
    };
    AcceleratedPaintCallback accel_popup_cb = [main_ptr, wakeMainLoop](int fd, uint32_t stride, uint64_t modifier, int w, int h) {
        main_ptr->compositor->queuePopupDmabuf(fd, stride, modifier, w, h);
        wakeMainLoop();
    };
#elif defined(_WIN32)
    PopupShowCallback popup_show_cb = has_dcomp_browsers ?
        PopupShowCallback([&mainBrowserLayer](bool show) { mainBrowserLayer.onPopupShow(show); }) : nullptr;
    PopupSizeCallback popup_size_cb = has_dcomp_browsers ?
        PopupSizeCallback([&mainBrowserLayer](int x, int y, int w, int h) { mainBrowserLayer.onPopupSize(x, y, w, h); }) : nullptr;
    AcceleratedPaintCallback accel_popup_cb = nullptr;
#else
    PopupShowCallback popup_show_cb = nullptr;
    PopupSizeCallback popup_size_cb = nullptr;
    AcceleratedPaintCallback accel_popup_cb = nullptr;
#endif

    CefRefPtr<Client> client(new Client(width, height,
        [main_paint_cb, main_ptr, &paint_size_matched](const void* buffer, int w, int h) {
            static int paint_count = 0;
            if (paint_count++ % 100 == 0) {
                LOG_DEBUG(LOG_CEF, "main browser paint #%d: %dx%d", paint_count, w, h);
            }
            main_paint_cb(buffer, w, h);
            // Track if paint matched compositor size
            if (w == static_cast<int>(main_ptr->compositor->width()) &&
                h == static_cast<int>(main_ptr->compositor->height())) {
                paint_size_matched = true;
            }
        },
        [&](const std::string& cmd, const std::string& arg, int intArg, const std::string& metadata) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_cmds.push_back({cmd, arg, intArg, 0.0, metadata});
            wakeMainLoop();  // Wake from idle wait to process command
        },
#if !defined(__APPLE__) && !defined(_WIN32)
        // Accelerated paint callback - queue dmabuf for import on main thread
        [main_ptr, wakeMainLoop](int fd, uint32_t stride, uint64_t modifier, int w, int h) {
            main_ptr->compositor->queueDmabuf(fd, stride, modifier, w, h);
            wakeMainLoop();
        },
#else
        nullptr,  // No GPU accelerated paint on macOS/Windows
#endif
        &menu,
        [&](cef_cursor_type_t type) {
            if (type == CT_NONE && blank_cursor) {
                // Web content set cursor: none (e.g. mouseIdle during video playback)
                if (current_cursor) {
                    SDL_DestroyCursor(current_cursor);
                    current_cursor = nullptr;
                }
                SDL_SetCursor(blank_cursor);
            } else if (type != CT_NONE) {
                SDL_SystemCursor sdl_type = cefCursorToSDL(type);
                if (current_cursor) {
                    SDL_DestroyCursor(current_cursor);
                }
                current_cursor = SDL_CreateSystemCursor(sdl_type);
                SDL_SetCursor(current_cursor);
            }
        },
        [&](bool fullscreen) {
            // Web content requested fullscreen change via JS Fullscreen API
            LOG_DEBUG(LOG_WINDOW, "Fullscreen: CEF requests %s, source=%d",
                      fullscreen ? "enter" : "exit", static_cast<int>(fullscreen_source));
            if (fullscreen) {
                if (fullscreen_source == FullscreenSource::NONE) {
                    fullscreen_source = FullscreenSource::CEF;
                }
                SDL_SetWindowFullscreen(window, true);
            } else {
                // Only honor CEF exit if CEF initiated fullscreen
                if (fullscreen_source == FullscreenSource::CEF) {
                    SDL_SetWindowFullscreen(window, false);
                    fullscreen_source = FullscreenSource::NONE;
                }
                // WM-initiated fullscreen: ignore CEF exit request
            }
        },
        getPhysicalSize,
        [&cmd_mutex, &pending_cmds, &wakeMainLoop](const std::string& color) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_cmds.push_back({"theme_color", color, 0, 0.0});
            wakeMainLoop();
        },
#ifdef __APPLE__
        [&cmd_mutex, &pending_cmds, &wakeMainLoop](bool visible) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_cmds.push_back({"osd_visible", "", visible ? 1 : 0, 0.0});
            wakeMainLoop();
        },
#else
        nullptr,
#endif
        popup_show_cb,
        popup_size_cb,
        accel_popup_cb
#ifdef __APPLE__
        // IOSurface callback for macOS accelerated paint - queue for import on main thread
        , [main_ptr, &paint_size_matched, wakeMainLoop](void* surface, int format, int w, int h) {
            main_ptr->compositor->queueIOSurface(surface, format, w, h);
            if (w == static_cast<int>(main_ptr->compositor->width()) &&
                h == static_cast<int>(main_ptr->compositor->height())) {
                paint_size_matched.store(true, std::memory_order_relaxed);
            }
            wakeMainLoop();
        }
#endif
#ifdef _WIN32
        // Windows: DComp shared texture callbacks for main browser
        , has_dcomp_browsers ?
            WinSharedTexturePaintCallback([&mainBrowserLayer, &paint_size_matched](void* handle, int type, int w, int h) {
                if (type == PET_VIEW) {
                    mainBrowserLayer.onPaintView(static_cast<HANDLE>(handle), w, h);
                    // DComp path: mark size matched so the WasResized retry loop
                    // (for stale-paint recovery) stops.  The DCompBrowserLayer
                    // handles any size mismatch internally via swap chain recreation.
                    paint_size_matched = true;
                } else if (type == PET_POPUP) {
                    mainBrowserLayer.onPaintPopup(static_cast<HANDLE>(handle), w, h);
                }
            }) : nullptr
#endif
    ));
    main_ptr->client = client;
    main_ptr->getBrowser = [client]() { return client->browser(); };
    main_ptr->resizeBrowser = [client](int w, int h, int pw, int ph) { client->resize(w, h, pw, ph); };
    main_ptr->getInputReceiver = [client]() -> InputReceiver* { return client.get(); };
    main_ptr->isClosed = [client]() { return client->isClosed(); };
    main_ptr->input_layer = std::make_unique<BrowserLayer>(client.get());
    main_ptr->input_layer->setWindowSize(width, height);
    main_ptr->wake_main_loop = wakeMainLoop;
    browsers.add("main", std::move(main_entry));
    if (!player_mode) {
        browsers.add("overlay", std::move(overlay_entry));  // overlay on top for fade
    }
    LOG_DEBUG(LOG_MAIN, "Browser entries added%s", player_mode ? " (player mode, no overlay)" : " (main behind, overlay on top)");

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
#ifdef __APPLE__
    window_info.shared_texture_enabled = true;  // macOS: use IOSurface zero-copy
#elif defined(_WIN32)
    window_info.shared_texture_enabled = has_dcomp_browsers;  // Windows: use DComp zero-copy
#else
    window_info.shared_texture_enabled = use_dmabuf;  // Linux: dmabuf zero-copy
#endif
    (void)use_dmabuf;

    CefBrowserSettings browser_settings;
    browser_settings.background_color = 0;
    browser_settings.javascript_access_clipboard = STATE_ENABLED;
    browser_settings.javascript_dom_paste = STATE_ENABLED;
    // Match CEF frame rate to display refresh rate
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    if (mode && mode->refresh_rate > 0) {
        browser_settings.windowless_frame_rate = static_cast<int>(mode->refresh_rate);
        LOG_INFO(LOG_CEF, "CEF frame rate: %.0f Hz", mode->refresh_rate);
    } else {
        browser_settings.windowless_frame_rate = 60;
    }

    // Create overlay browser loading index.html
    if (!player_mode) {
        CefWindowInfo overlay_window_info;
        overlay_window_info.SetAsWindowless(0);
#ifdef __APPLE__
        overlay_window_info.shared_texture_enabled = true;  // macOS: use IOSurface zero-copy
#elif defined(_WIN32)
        overlay_window_info.shared_texture_enabled = has_dcomp_browsers;  // Windows: use DComp zero-copy
#else
        overlay_window_info.shared_texture_enabled = use_dmabuf;  // Linux: dmabuf zero-copy
#endif
        CefBrowserSettings overlay_browser_settings;
        overlay_browser_settings.background_color = 0;
        overlay_browser_settings.windowless_frame_rate = browser_settings.windowless_frame_rate;

        std::string overlay_html_path = "app://resources/index.html";
        bool overlay_created = CefBrowserHost::CreateBrowser(overlay_window_info, overlay_client, overlay_html_path, overlay_browser_settings, nullptr, nullptr);
        LOG_INFO(LOG_CEF, "Overlay CreateBrowser: %s", overlay_created ? "ok" : "FAILED");
    }

    // State tracking
    using Clock = std::chrono::steady_clock;

    // Main browser: load player URL, saved server, or wait for overlay IPC
    if (player_mode) {
        // Player mode: load bundled OSD directly
        LOG_INFO(LOG_MAIN, "Player mode: loading %s", player_url.c_str());
        CefBrowserHost::CreateBrowser(window_info, client, player_url, browser_settings, nullptr, nullptr);
    } else {
        // Normal mode: load saved server or wait for overlay
        std::string saved_url = Settings::instance().serverUrl();
        if (saved_url.empty()) {
            LOG_INFO(LOG_MAIN, "Waiting for overlay to provide server URL");
            CefBrowserHost::CreateBrowser(window_info, client, "about:blank", browser_settings, nullptr, nullptr);
        } else {
            overlay_state = OverlayState::WAITING;
            overlay_fade_start = Clock::now();
            LOG_INFO(LOG_MAIN, "Loading saved server: %s", saved_url.c_str());
            CefBrowserHost::CreateBrowser(window_info, client, saved_url, browser_settings, nullptr, nullptr);
        }
    }
    // Input routing stack - use BrowserStack for input layers
    MenuLayer menu_layer(&menu);
    InputStack input_stack;
    if (player_mode) {
        input_stack.push(browsers.getInputLayer("main"));
    } else {
        input_stack.push(browsers.getInputLayer("overlay"));
    }

    // Track which browser layer is active (for WindowStateNotifier)
    BrowserLayer* active_browser = player_mode
        ? browsers.getInputLayer("main")
        : browsers.getInputLayer("overlay");

    // Push/pop menu layer on open/close
    menu.setOnOpen([&]() { input_stack.push(&menu_layer); });
    menu.setOnClose([&]() { input_stack.remove(&menu_layer); });

    // Window state notifications
    WindowStateNotifier window_state;
    window_state.add(active_browser);
#ifndef __APPLE__
    // Windows/Linux: Pause video on minimize
    MpvLayer mpv_layer(mpv);
    window_state.add(&mpv_layer);
#endif

    bool focus_set = false;
    int current_width = width;
    int current_height = height;
    float current_scale = SDL_GetWindowDisplayScale(window);
    bool video_ready = false;  // Latches true once first frame renders
#ifdef __APPLE__
    bool window_activated = false;  // Activate window on first expose event
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
    auto last_resize_time = Clock::now() - std::chrono::seconds(10);  // Track when resize stopped
#endif

    // Start mpv event thread - processes events and queues them for main thread
    MpvEventThread mpvEvents;
    mpvEvents.start(mpv);

#ifndef __APPLE__
    // Windows and Linux use threaded video rendering
    // Windows: Vulkan gpu-next with DComp
    // Linux/Wayland: Vulkan subsurface
    // Linux/X11: OpenGL with shared EGL context + FBO
    VideoRenderController videoController;
    videoController.startThreaded(&videoRenderer);
    mpv->setRedrawCallback([&videoController]() {
        videoController.notify();
    });
#endif

#ifdef __APPLE__
    // Live resize support - event watcher is called during modal resize loop
    struct LiveResizeContext {
        SDL_Window* window;
        BrowserStack* browsers;
        VideoRenderer* videoRenderer;
        int* current_width;
        int* current_height;
        bool* has_video;
    };
    LiveResizeContext live_resize_ctx = {
        window,
        &browsers,
        &videoRenderer,
        &current_width,
        &current_height,
        &has_video
    };

    auto liveResizeCallback = [](void* userdata, SDL_Event* event) -> bool {
        auto* ctx = static_cast<LiveResizeContext*>(userdata);

        if (event->type == SDL_EVENT_WINDOW_RESIZED) {
            *ctx->current_width = event->window.data1;
            *ctx->current_height = event->window.data2;

            // Tell all browsers the new size
            ctx->browsers->resizeAll(*ctx->current_width, *ctx->current_height);

            // Resize video layer with physical pixel dimensions
            float scale = SDL_GetWindowDisplayScale(ctx->window);
            int physical_w = static_cast<int>(*ctx->current_width * scale);
            int physical_h = static_cast<int>(*ctx->current_height * scale);
            ctx->videoRenderer->resize(physical_w, physical_h);
        }

        // Render on EXPOSED events during live resize
        if (event->type == SDL_EVENT_WINDOW_EXPOSED && event->window.data1 == 1) {
            // Sync presents with CA transaction during resize for fluid resize
            ctx->browsers->setPresentsWithTransaction(true);

            // macOS uses external_message_pump - must pump CEF here during resize
            App::DoWork();

            // Render video if playing
            if (*ctx->has_video && ctx->videoRenderer->hasFrame()) {
                ctx->videoRenderer->render(*ctx->current_width, *ctx->current_height);
            }

            // Flush and composite all browsers (back-to-front order)
            ctx->browsers->renderAll(*ctx->current_width, *ctx->current_height);
        }

        return true;
    };

    SDL_AddEventWatch(liveResizeCallback, &live_resize_ctx);
#elif defined(_WIN32)
    // Live resize support for Windows - the Win32 modal resize loop blocks
    // our main event loop, so we use an event watcher to resize DComp layers
    // and notify CEF as the window edge is dragged.
    struct WinLiveResizeContext {
        SDL_Window* window;
        BrowserStack* browsers;
        DCompBrowserLayer* mainBrowserLayer;
        DCompBrowserLayer* overlayBrowserLayer;
        VideoRenderController* videoController;
        WGLContext* wgl;
        int* current_width;
        int* current_height;
        bool has_dcomp;
    };
    WinLiveResizeContext win_live_resize_ctx = {
        window,
        &browsers,
        &mainBrowserLayer,
        &overlayBrowserLayer,
        &videoController,
        &wgl,
        &current_width,
        &current_height,
        has_dcomp_browsers
    };

    auto winLiveResizeCallback = [](void* userdata, SDL_Event* event) -> bool {
        if (event->type != SDL_EVENT_WINDOW_RESIZED) return true;
        auto* ctx = static_cast<WinLiveResizeContext*>(userdata);

        *ctx->current_width = event->window.data1;
        *ctx->current_height = event->window.data2;

        int physical_w, physical_h;
        SDL_GetWindowSizeInPixels(ctx->window, &physical_w, &physical_h);

        ctx->browsers->resizeAll(*ctx->current_width, *ctx->current_height, physical_w, physical_h);

        if (ctx->has_dcomp) {
            float scale = (*ctx->current_width > 0)
                ? static_cast<float>(physical_w) / *ctx->current_width : 1.0f;
            ctx->mainBrowserLayer->setScale(scale);
            ctx->overlayBrowserLayer->setScale(scale);
        }
        ctx->wgl->resize(*ctx->current_width, *ctx->current_height);
        ctx->videoController->requestResize(physical_w, physical_h);

        return true;
    };

    SDL_AddEventWatch(winLiveResizeCallback, &win_live_resize_ctx);
#endif

#ifndef _WIN32
    // Initial CEF pump to kick off work scheduling
    App::DoWork();
#endif

    // Main loop - simplified (no Vulkan command buffers for main surface)
    bool running = true;
    bool needs_render = true;  // Render first frame
    int slow_frame_count = 0;
    while (running && !client->isClosed()) {
        auto frame_start = Clock::now();
        auto now = frame_start;
        bool activity_this_frame = false;

        // Process mpv events from event thread
        for (const auto& ev : mpvEvents.drain()) {
            switch (ev.type) {
            case MpvEvent::Type::Position:
                client->updatePosition(ev.value);
                mediaSessionThread.setPosition(static_cast<int64_t>(ev.value * 1000.0));
                break;
            case MpvEvent::Type::Duration:
                client->updateDuration(ev.value);
                break;
            case MpvEvent::Type::Speed:
                current_playback_rate = ev.value;
                mediaSessionThread.setRate(ev.value);
                break;
            case MpvEvent::Type::Playing:
                // Restore video state - loadfile replacement fires END_FILE(STOP)
                // for the old file which triggers Canceled, resetting has_video.
                // FILE_LOADED (Playing event) follows, so re-enable video here.
                if (!has_video) {
                    has_video = true;
                    setVideoTitlebar(true);
                    LOG_INFO(LOG_MAIN, "Video restored after file transition");
                    videoRenderer.setVisible(true);
#ifndef __APPLE__
                    videoController.setActive(true);
#endif
                }
                client->emitPlaying();
                mediaSessionThread.setPlaybackState(PlaybackState::Playing);
                break;
            case MpvEvent::Type::Paused:
                if (mpv->isPlaying()) {
                    if (ev.flag) {
                        client->emitPaused();
                        mediaSessionThread.setPlaybackState(PlaybackState::Paused);
                    } else {
                        client->emitPlaying();
                        mediaSessionThread.setPlaybackState(PlaybackState::Playing);
                    }
                }
                break;
            case MpvEvent::Type::Finished:
                LOG_INFO(LOG_MAIN, "Track finished naturally (EOF)");
                has_video = false;
                setVideoTitlebar(false);
                video_ready = false;
#ifdef __APPLE__
                videoRenderer.setVisible(false);
#else
                videoController.setActive(false);
                videoController.resetVideoReady();
#endif
                client->emitFinished();
                mediaSessionThread.setPlaybackState(PlaybackState::Stopped);
                break;
            case MpvEvent::Type::Canceled:
                LOG_DEBUG(LOG_MAIN, "Track canceled (user stop)");
                has_video = false;
                setVideoTitlebar(false);
                video_ready = false;
#ifdef __APPLE__
                videoRenderer.setVisible(false);
#else
                videoController.setActive(false);
                videoController.resetVideoReady();
#endif
                client->emitCanceled();
                mediaSessionThread.setPlaybackState(PlaybackState::Stopped);
                break;
            case MpvEvent::Type::Seeking:
                client->emitSeeking();
                mediaSessionThread.setPosition(static_cast<int64_t>(ev.value * 1000.0));
                mediaSessionThread.emitSeeking();
                break;
            case MpvEvent::Type::Seeked:
                client->updatePosition(ev.value);
                mediaSessionThread.setPosition(static_cast<int64_t>(ev.value * 1000.0));
                mediaSessionThread.emitSeeked(static_cast<int64_t>(ev.value * 1000.0));
                break;
            case MpvEvent::Type::Buffering:
                mediaSessionThread.setPosition(static_cast<int64_t>(ev.value * 1000.0));
                mediaSessionThread.setBuffering(ev.flag);
                break;
            case MpvEvent::Type::CoreIdle:
                mediaSessionThread.setPosition(static_cast<int64_t>(ev.value * 1000.0));
                break;
            case MpvEvent::Type::BufferedRanges: {
                std::string json = "[";
                for (size_t i = 0; i < ev.ranges.size(); i++) {
                    if (i > 0) json += ",";
                    json += "{\"start\":" + std::to_string(ev.ranges[i].first) +
                            ",\"end\":" + std::to_string(ev.ranges[i].second) + "}";
                }
                json += "]";
                client->executeJS("if(window._nativeUpdateBufferedRanges)window._nativeUpdateBufferedRanges(" + json + ");");
                break;
            }
            case MpvEvent::Type::Error:
                LOG_ERROR(LOG_MAIN, "Playback error: %s", ev.error.c_str());
                has_video = false;
                setVideoTitlebar(false);
                video_ready = false;
#ifdef __APPLE__
                videoRenderer.setVisible(false);
#else
                videoController.setActive(false);
                videoController.resetVideoReady();
#endif
                client->emitError(ev.error);
                mediaSessionThread.setPlaybackState(PlaybackState::Stopped);
                break;
            }
        }

        if (!focus_set) {
            window_state.notifyFocusGained();
            focus_set = true;
        }

        // Event-driven: wait for events when idle, poll when active
        bool has_pending = browsers.anyHasPendingContent();
        bool has_pending_cmds = false;
        {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            has_pending_cmds = !pending_cmds.empty();
        }
        SDL_Event event;
        bool have_event;
        if (needs_render || has_video || has_pending || has_pending_cmds || !paint_size_matched) {
            have_event = SDL_PollEvent(&event);
        } else {
#ifdef _WIN32
            if (overlay_state == OverlayState::WAITING) {
                // Overlay fade timer pending — don't block indefinitely.
                // DComp zero-copy paints bypass the main loop wake, so we
                // must wake ourselves to advance the overlay state machine.
                auto remaining = OVERLAY_FADE_DELAY_SEC -
                    std::chrono::duration<float>(Clock::now() - overlay_fade_start).count();
                if (remaining <= 0) {
                    have_event = SDL_PollEvent(&event);
                } else {
                    int timeout_ms = static_cast<int>(remaining * 1000) + 1;
                    have_event = SDL_WaitEventTimeout(&event, timeout_ms);
                }
            } else {
                // Idle: block until SDL event (input, window, or CEF wake callback)
                have_event = SDL_WaitEvent(&event);
            }
#else
            // Drain CEF work before sleeping — handles immediate work without
            // pushing SDL events (OnScheduleMessagePumpWork just sets a flag).
            App::DoWork();

            // Re-check if CEF work generated content
            has_pending = browsers.anyHasPendingContent();
            {
                std::lock_guard<std::mutex> lock(cmd_mutex);
                has_pending_cmds = !pending_cmds.empty();
            }
            if (has_pending || has_pending_cmds) {
                have_event = SDL_PollEvent(&event);
            } else {
#ifdef __APPLE__
                have_event = SDL_WaitEvent(&event);
#else
                have_event = eventLoopWake.waitForEvent(&event);
#endif
            }
#endif
        }

#ifndef _WIN32
        // Check for deferred signal (Ctrl+C / kill) — cannot call SDL_PushEvent
        // from a signal handler (not async-signal-safe), so we set a flag there
        // and convert it to an SDL quit event here on the main thread.
        if (g_quit_requested) {
            g_quit_requested = 0;
            SDL_Event qe{};
            qe.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&qe);
        }
#endif

        while (have_event) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;

            // Input events - set activity flag and route through input stack
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_MOTION:
                activity_this_frame = true;
                [[fallthrough]];
            case SDL_EVENT_TEXT_INPUT:
                input_stack.route(event);
#ifdef __APPLE__
                // Cmd+Q to quit on macOS (no menu bar to provide this)
                if (event.type == SDL_EVENT_KEY_DOWN &&
                    event.key.key == SDLK_Q && (SDL_GetModState() & SDL_KMOD_GUI)) {
                    running = false;
                }
#endif
                break;

            // Window events
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                window_state.notifyFocusGained();
                // Sync browser fullscreen with SDL state on focus gain (WM may have changed it)
                if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) {
                    client->executeJS("document.documentElement.requestFullscreen().catch(()=>{});");
                } else {
                    client->exitFullscreen();
                }
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                window_state.notifyFocusLost();
                break;

            case SDL_EVENT_WINDOW_MINIMIZED:
                window_state.notifyMinimized();
                break;

            case SDL_EVENT_WINDOW_RESTORED:
                window_state.notifyRestored();
                break;

#ifdef __APPLE__
            case SDL_EVENT_WINDOW_EXPOSED:
                if (!window_activated) {
                    activateMacWindow(window);
                    window_activated = true;
                }
                break;
#endif

            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                LOG_DEBUG(LOG_WINDOW, "Fullscreen: SDL enter, source=%d", static_cast<int>(fullscreen_source));
                was_maximized_before_fullscreen =
                    (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
                if (fullscreen_source == FullscreenSource::NONE) {
                    fullscreen_source = FullscreenSource::WM;
                }
                client->executeJS("document.documentElement.requestFullscreen().catch(()=>{});");
                break;

            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                LOG_DEBUG(LOG_WINDOW, "Fullscreen: SDL leave, source=%d", static_cast<int>(fullscreen_source));
                was_maximized_before_fullscreen = false;
                client->exitFullscreen();
                if (fullscreen_source == FullscreenSource::WM) {
                    fullscreen_source = FullscreenSource::NONE;
                }
                break;

            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:  // display event (not window)
            case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:  // display event (not window)
                clampWindowToDisplay(window);
                break;

            case SDL_EVENT_WINDOW_RESIZED: {
                current_width = event.window.data1;
                current_height = event.window.data2;

                // Get physical dimensions for compositor resize
                int physical_w, physical_h;
                SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);

                // Only invalidate paint match when size actually changed,
                // otherwise a duplicate configure event resets the flag after
                // CEF already painted at the correct size (causing stale black border).
                // Skip 0x0 — macOS returns this during resize before the
                // backing store is ready, which would permanently stick the flag.
                if (physical_w > 0 && physical_h > 0 &&
                    (physical_w != static_cast<int>(main_ptr->compositor->width()) ||
                     physical_h != static_cast<int>(main_ptr->compositor->height()))) {
                    paint_size_matched = false;
                }

#ifndef _WIN32
                // Windows: winLiveResizeCallback handles all resize work
                // (browser/DComp/WGL/video) to avoid double-processing.
                browsers.resizeAll(current_width, current_height, physical_w, physical_h);
#ifdef __APPLE__
                videoRenderer.resize(physical_w, physical_h);
#else
                egl.resize(physical_w, physical_h);
                videoController.requestResize(physical_w, physical_h);
                videoRenderer.setDestinationSize(current_width, current_height);
                last_resize_time = Clock::now();
#endif
#endif
                break;
            }

            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
                float new_scale = SDL_GetWindowDisplayScale(window);

                // Resize window to maintain same physical size
                int new_logical_w = static_cast<int>(current_width * current_scale / new_scale);
                int new_logical_h = static_cast<int>(current_height * current_scale / new_scale);
                LOG_INFO(LOG_WINDOW, "Display scale changed: %.2f -> %.2f, resizing %dx%d -> %dx%d",
                         current_scale, new_scale, current_width, current_height, new_logical_w, new_logical_h);
                SDL_SetWindowSize(window, new_logical_w, new_logical_h);

                // Update tracked state
                current_width = new_logical_w;
                current_height = new_logical_h;
                current_scale = new_scale;

                // Get actual physical dimensions after resize
                int physical_w, physical_h;
                SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);

                // Resize all browsers and compositors, notify of scale change
                browsers.resizeAll(new_logical_w, new_logical_h, physical_w, physical_h);
                browsers.notifyAllScreenInfoChanged();
#ifdef _WIN32
                if (has_dcomp_browsers) {
                    mainBrowserLayer.setScale(new_scale);
                    overlayBrowserLayer.setScale(new_scale);
                }
#elif !defined(__APPLE__)
                main_ptr->compositor->setScale(new_scale);
#endif
                break;
            }

            default:
                break;
            }
            have_event = SDL_PollEvent(&event);
        }

        // Raise window if another instance signaled us
        {
            std::lock_guard<std::mutex> lock(raise_mutex);
            if (raise_requested) {
                raise_requested = false;
                activateWindow(window, pending_activation_token);
                pending_activation_token.clear();
            }
        }

#ifdef __APPLE__
        // Flush coalesced scroll events before pumping CEF — sends one
        // combined wheel event per frame instead of one per SDL event,
        // eliminating stutter from uneven event distribution across frames.
        client->flushScroll();
        if (overlay_client)
            overlay_client->flushScroll();
#endif

#ifndef _WIN32
        // macOS/Linux: Pump CEF - scheduling controls actual work frequency
        App::DoWork();
#endif

        // Determine if we need to render this frame
        needs_render = activity_this_frame || has_video || browsers.anyHasPendingContent() || overlay_state == OverlayState::FADING;

        // Process player commands
        {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            for (const auto& cmd : pending_cmds) {
                if (cmd.cmd == "load") {
                    double startSec = static_cast<double>(cmd.intArg) / 1000.0;
                    LOG_INFO(LOG_MAIN, "playerLoad: %s start=%.1fs", cmd.url.c_str(), startSec);
                    // Parse and set media session metadata
                    if (!cmd.metadata.empty() && cmd.metadata != "{}") {
                        MediaMetadata meta = parseMetadataJson(cmd.metadata);
                        LOG_DEBUG(LOG_MAIN, "metadata: title=%s artist=%s", meta.title.c_str(), meta.artist.c_str());
                        mediaSessionThread.setMetadata(meta);
                        // Apply normalization gain (ReplayGain) if present
                        bool hasGain = false;
                        double normGain = jsonGetDouble(cmd.metadata, "NormalizationGain", &hasGain);
                        mpv->setNormalizationGain(hasGain ? normGain : 0.0);
                    } else {
                        mpv->setNormalizationGain(0.0);  // Clear any previous gain
                    }
                    if (mpv->loadFile(cmd.url, startSec)) {
                        has_video = true;
                        setVideoTitlebar(true);
                        LOG_INFO(LOG_MAIN, "Video loaded, has_video=true");
                        videoRenderer.setVisible(true);
#ifdef __APPLE__
                        if (videoRenderer.isHdr()) {
                            videoRenderer.setColorspace();
                        }
#else
                        videoController.setActive(true);
                        if (videoRenderer.isHdr()) {
                            videoController.requestSetColorspace();
                        }
#endif
                        // Apply initial subtitle track if specified
                        int subIdx = jsonGetIntDefault(cmd.metadata, "_subIdx", -1);
                        if (subIdx >= 0) {
                            mpv->setSubtitleTrack(subIdx);
                        }
                        // Apply initial audio track if specified
                        int audioIdx = jsonGetIntDefault(cmd.metadata, "_audioIdx", -1);
                        if (audioIdx >= 0) {
                            mpv->setAudioTrack(audioIdx);
                        }
                        // mpv events will trigger state callbacks
                    } else {
                        client->emitError("Failed to load video");
                    }
                } else if (cmd.cmd == "stop") {
                    mpv->stop();
                    has_video = false;
                    setVideoTitlebar(false);
                    video_ready = false;
#ifdef __APPLE__
                    videoRenderer.setVisible(false);
#else
                    videoController.setActive(false);
                    videoController.resetVideoReady();
#endif
                    // mpv END_FILE event will trigger finished callback
                } else if (cmd.cmd == "pause") {
                    mpv->pause();
                    // mpv pause property change will trigger state callback
                } else if (cmd.cmd == "play") {
                    mpv->play();
                    // mpv pause property change will trigger state callback
                } else if (cmd.cmd == "playpause") {
                    if (mpv->isPaused()) {
                        mpv->play();
                    } else {
                        mpv->pause();
                    }
                    // mpv pause property change will trigger state callback
                } else if (cmd.cmd == "seek") {
                    mpv->seek(static_cast<double>(cmd.intArg) / 1000.0);
                } else if (cmd.cmd == "volume") {
                    mpv->setVolume(cmd.intArg);
                } else if (cmd.cmd == "mute") {
                    mpv->setMuted(cmd.intArg != 0);
                } else if (cmd.cmd == "speed") {
                    double speed = cmd.intArg / 1000.0;
                    mpv->setSpeed(speed);
                } else if (cmd.cmd == "subtitle") {
                    mpv->setSubtitleTrack(cmd.intArg);
                } else if (cmd.cmd == "audio") {
                    mpv->setAudioTrack(cmd.intArg);
                } else if (cmd.cmd == "audioDelay") {
                    if (!cmd.metadata.empty()) {
                        try {
                            double delay = std::stod(cmd.metadata);
                            mpv->setAudioDelay(delay);
                        } catch (...) {
                            LOG_WARN(LOG_MAIN, "Invalid audioDelay value: %s", cmd.metadata.c_str());
                        }
                    }
                } else if (cmd.cmd == "media_metadata") {
                    MediaMetadata meta = parseMetadataJson(cmd.url);
                    LOG_DEBUG(LOG_MAIN, "Media metadata: title=%s", meta.title.c_str());
                    mediaSessionThread.setMetadata(meta);
                } else if (cmd.cmd == "media_position") {
                    int64_t pos_us = static_cast<int64_t>(cmd.intArg) * 1000;
                    mediaSessionThread.setPosition(pos_us);
                } else if (cmd.cmd == "media_state") {
                    if (cmd.url == "Playing") {
                        mediaSessionThread.setPlaybackState(PlaybackState::Playing);
                    } else if (cmd.url == "Paused") {
                        mediaSessionThread.setPlaybackState(PlaybackState::Paused);
                    } else {
                        mediaSessionThread.setPlaybackState(PlaybackState::Stopped);
                    }
                } else if (cmd.cmd == "media_artwork") {
                    LOG_DEBUG(LOG_MAIN, "Media artwork received: %.50s...", cmd.url.c_str());
                    mediaSessionThread.setArtwork(cmd.url);
                } else if (cmd.cmd == "media_queue") {
                    // Decode flags: bit 0 = canNext, bit 1 = canPrev
                    bool canNext = (cmd.intArg & 1) != 0;
                    bool canPrev = (cmd.intArg & 2) != 0;
                    mediaSessionThread.setCanGoNext(canNext);
                    mediaSessionThread.setCanGoPrevious(canPrev);
                } else if (cmd.cmd == "media_notify_rate") {
                    // Ignored — mpv's speed property observation is authoritative
                    (void)cmd.intArg;
                } else if (cmd.cmd == "media_seeked") {
                    // Ignored — mpv's seeking property observation handles this
                    (void)cmd.intArg;
                } else if (cmd.cmd == "media_action") {
                    // Route media session control commands to JS playbackManager
                    std::string js = "if(window._nativeHostInput) window._nativeHostInput(['" + cmd.url + "']);";
                    client->executeJS(js);
                } else if (cmd.cmd == "media_seek") {
                    // Route media session seek to JS playbackManager
                    std::string js = "if(window._nativeSeek) window._nativeSeek(" + std::to_string(cmd.intArg) + ");";
                    client->executeJS(js);
                } else if (cmd.cmd == "media_rate") {
                    // Route media session rate change to JS player
                    client->emitRateChanged(cmd.doubleArg);
                } else if (cmd.cmd == "theme_color") {
                    if (titlebar_color_unlocked) {
                        current_theme_color = cmd.url;
                        if (!has_video) {
                            applyTitlebarColor(cmd.url);
                        }
                    } else {
                        pending_titlebar_color = cmd.url;
                    }
#ifdef __APPLE__
                } else if (cmd.cmd == "osd_visible" && transparent_titlebar) {
                    setMacTrafficLightsVisible(cmd.intArg != 0);
#endif
                }
            }
            pending_cmds.clear();
        }

        // Check for pending server URL from overlay
        {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            if (!pending_server_url.empty()) {
                std::string url = pending_server_url;
                pending_server_url.clear();

                // Only process if we're still showing the overlay form
                // (ignore if already loading/fading from saved server)
                if (overlay_state == OverlayState::SHOWING) {
                    LOG_INFO(LOG_MAIN, "Loading server from overlay: %s", url.c_str());
                    Settings::instance().setServerUrl(url);
                    Settings::instance().save();
                    client->loadUrl(url);
                    overlay_state = OverlayState::WAITING;
                    overlay_fade_start = now;
                } else {
                    LOG_DEBUG(LOG_MAIN, "Ignoring loadServer (overlay_state != SHOWING)");
                }
            }
        }

        // Update overlay state machine
        if (overlay_state == OverlayState::WAITING) {
            auto elapsed = std::chrono::duration<float>(now - overlay_fade_start).count();
            if (elapsed >= OVERLAY_FADE_DELAY_SEC) {
                overlay_state = OverlayState::FADING;
                clear_color = 0.0f;  // Switch to black background
                // Unlock titlebar color and apply any pending theme color
                titlebar_color_unlocked = true;
                if (!pending_titlebar_color.empty()) {
                    current_theme_color = pending_titlebar_color;
                    applyTitlebarColor(pending_titlebar_color);
                    pending_titlebar_color.clear();
                }
                // Switch input from overlay to main browser
                window_state.remove(active_browser);
                active_browser->onFocusLost();
                input_stack.remove(browsers.getInputLayer("overlay"));
                input_stack.push(browsers.getInputLayer("main"));
                active_browser = browsers.getInputLayer("main");
                window_state.add(active_browser);
                active_browser->onFocusGained();
                overlay_fade_start = now;
                LOG_DEBUG(LOG_OVERLAY, "State: WAITING -> FADING");
            }
        } else if (overlay_state == OverlayState::FADING) {
            auto elapsed = std::chrono::duration<float>(now - overlay_fade_start).count();
            float progress = elapsed / OVERLAY_FADE_DURATION_SEC;
            if (progress >= 1.0f) {
                overlay_browser_alpha = 0.0f;
                browsers.setAlpha("overlay", 0.0f);
                overlay_state = OverlayState::HIDDEN;
                // Hide compositor layer and close browser
#ifdef _WIN32
                if (has_dcomp_browsers)
                    overlayBrowserLayer.hide();
#endif
                if (auto* entry = browsers.get("overlay")) {
                    entry->compositor->setVisible(false);
                    if (entry->getBrowser) {
                        if (auto browser = entry->getBrowser()) {
                            browser->GetHost()->CloseBrowser(true);
                        }
                    }
                }
                LOG_DEBUG(LOG_OVERLAY, "State: FADING -> HIDDEN");
            } else {
                overlay_browser_alpha = 1.0f - progress;
                browsers.setAlpha("overlay", overlay_browser_alpha);
#ifdef _WIN32
                if (has_dcomp_browsers)
                    overlayBrowserLayer.setOpacity(overlay_browser_alpha);
#endif
            }
        }

        // Menu overlay blending
        menu.clearRedraw();

        // Import queued GPU textures (cheap pointer swap).  New content
        // from OnAcceleratedPaint triggers compositing even without user input.
#ifdef __APPLE__
        bool imported = browsers.importAll();

        // Ensure async presentation after live resize (which uses transactional)
        browsers.setPresentsWithTransaction(false);
#endif

        // Render video to subsurface/layer
#ifdef __APPLE__
        if (needs_render || imported) {
            if (has_video) {
                bool hasFrame = videoRenderer.hasFrame();
                static int frame_log_count = 0;
                if (hasFrame) {
                    if (videoRenderer.render(current_width, current_height)) {
                        video_ready = true;
                        if (frame_log_count++ < 5) {
                            LOG_INFO(LOG_MAIN, "Video frame rendered (count=%d)", frame_log_count);
                        }
                    }
                }
            }

            // Flush and composite all browsers (back-to-front order)
            browsers.renderAll(current_width, current_height);
        }
#elif defined(_WIN32)
        // Windows: Vulkan gpu-next video via DComp, CEF overlay via DComp
        // Both are DComp visuals with explicit Z-ordering (video behind, CEF in front).
        videoController.render(current_width, current_height);

        // DComp zero-copy: CEF drives rendering via OnAcceleratedPaint callbacks,
        // no main-thread rendering needed. Fall back to GL overlay otherwise.
        if (!has_dcomp_browsers && has_overlay) {
            browsers.setDCompOverlay(true);
            overlayLayer.begin(current_width, current_height);
            browsers.renderAll(current_width, current_height);
            overlayLayer.end();
            browsers.setDCompOverlay(false);
        }

        if (!has_dcomp_browsers) {
            // WGL framebuffer: opaque black placeholder (covered by DComp topmost visuals)
            frameContext.beginFrame(0.0f, 1.0f);
            if (!has_overlay) {
                browsers.renderAll(current_width, current_height);
            }
            frameContext.endFrame();
        }
#else
        // Linux: Get physical dimensions for viewport (HiDPI)
        // Use SDL_GetWindowSizeInPixels instead of int(logical * scale) to avoid
        // truncation rounding mismatch — the EGL surface uses ceil() internally,
        // so truncating can leave a 1px unrendered strip at the right/bottom edge.
        int viewport_w, viewport_h;
        SDL_GetWindowSizeInPixels(window, &viewport_w, &viewport_h);
        glViewport(0, 0, viewport_w, viewport_h);

        frameContext.beginFrame(clear_color, videoController.getClearAlpha());
        videoController.render(viewport_w, viewport_h);

        // Composite video texture (for threaded OpenGL renderers like X11)
        videoRenderer.composite(viewport_w, viewport_h);

        // Flush and composite all browsers (back-to-front order)
        browsers.renderAll(viewport_w, viewport_h);

        frameContext.endFrame();
#endif
        // If CEF painted at stale size during resize, re-request repaint.
        // During rapid resize, WasResized()+Invalidate() from the resize handler
        // can get consumed by an already in-flight paint at the old size.
        // CEF then considers itself up-to-date and never repaints at the new size.
        if (!paint_size_matched) {
            if (auto browser = client->browser()) {
                browser->GetHost()->WasResized();
                browser->GetHost()->Invalidate(PET_VIEW);
            }
        }

        // Log slow frames
        auto frame_end = Clock::now();
        auto frame_ms = std::chrono::duration<double, std::milli>(frame_end - frame_start).count();
        if (frame_ms > 50.0 && has_video) {
            slow_frame_count++;
            if (slow_frame_count <= 10) {
                LOG_WARN(LOG_MAIN, "Slow frame: %.1fms (has_video=%d)", frame_ms, has_video);
            }
        }
    }

    // Cleanup
#ifdef _WIN32
    // Hide window immediately so user doesn't see "not responding" during cleanup
    SDL_HideWindow(window);
#endif
#ifdef __APPLE__
    SDL_RemoveEventWatch(liveResizeCallback, &live_resize_ctx);
#elif defined(_WIN32)
    SDL_RemoveEventWatch(winLiveResizeCallback, &win_live_resize_ctx);
#endif
    mediaSessionThread.stop();
#ifndef __APPLE__
    videoController.stop();
#endif
    mpvEvents.stop();
    mpv->cleanup();

#ifdef __APPLE__
    // macOS: simpler cleanup - CefShutdown handles browser cleanup
    browsers.cleanupCompositors();
    videoRenderer.cleanup();
    VideoStack::cleanupStatics();
    CefShutdown();
#elif defined(_WIN32)
    // Windows: wait for async browser close, then shut down CEF thread.
    // CEF thread must be stopped BEFORE GPU resource cleanup — its 5s
    // _exit() safety net ensures the process (and job-object children)
    // won't linger if any subsequent cleanup hangs.
    LOG_INFO(LOG_MAIN, "Shutdown: closing browsers...");
    browsers.closeAllBrowsers();
    while (!browsers.allBrowsersClosed()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOG_INFO(LOG_MAIN, "Shutdown: browsers closed, shutting down CEF...");
    cefThread.shutdown();
    LOG_INFO(LOG_MAIN, "Shutdown: cleaning compositors...");
    browsers.cleanupCompositors();
    overlayBrowserLayer.cleanup();
    mainBrowserLayer.cleanup();
    overlayLayer.cleanup();
    LOG_INFO(LOG_MAIN, "Shutdown: cleaning video renderer...");
    videoRenderer.cleanup();
    VideoStack::cleanupStatics();
    wgl.cleanup();
    // Save window geometry before force-exit so position persists.
    saveWindowGeometry(window, was_maximized_before_fullscreen);
    // Force-exit: atexit handlers / static destructors from CEF, D3D, or
    // Vulkan drivers can hang.  _exit() bypasses them and closes our job
    // object handle, which kills any lingering CEF subprocesses.
    _exit(0);
#else
    // Linux: pump CEF work during browser close (external_message_pump mode)
    browsers.closeAllBrowsers();
    while (!browsers.allBrowsersClosed()) {
        App::DoWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    browsers.cleanupCompositors();
    videoRenderer.cleanup();
    VideoStack::cleanupStatics();
    egl.cleanup();
    eventLoopWake.cleanup();
    CefShutdown();
#endif
#ifdef HAVE_KDE_DECORATION_PALETTE
    cleanupKdeDecorationPalette();
#endif
    cleanupWindowActivation();
    stopListener();
    shutdownStderrCapture();
    shutdownLogging();
    if (current_cursor) {
        SDL_DestroyCursor(current_cursor);
    }
    if (blank_cursor) {
        SDL_DestroyCursor(blank_cursor);
    }
    saveWindowGeometry(window, was_maximized_before_fullscreen);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
