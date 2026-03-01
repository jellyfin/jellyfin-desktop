#include <SDL3/SDL.h>
#include <filesystem>
#include "logging.h"
#include "version.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

#include "include/cef_app.h"
#include "include/cef_version.h"
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
// Wait for NSApplication events (integrates Cocoa + CFRunLoop)
void waitForMacEvent();
// Wake the NSApplication event loop from another thread
void wakeMacEventLoop();
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include "player/macos/media_session_macos.h"
#include "PFMoveApplication.h"
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "context/wgl_context.h"
#include "context/opengl_frame_context.h"
#include "player/windows/media_session_windows.h"
#else
#include "context/egl_context.h"
#include "context/opengl_frame_context.h"
#include "player/mpris/media_session_mpris.h"
#include <unistd.h>  // For close()
#endif
#include "player/media_session.h"
#include "player/media_session_thread.h"
#include "player/video_stack.h"
#include "player/video_renderer.h"
#include "player/mpv_event_thread.h"
#include "player/video_render_controller.h"
#include "cef/cef_app.h"
#include "cef/cef_client.h"
#include "cef/cef_thread.h"
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

// Overlay fade constants
constexpr float OVERLAY_FADE_DELAY_SEC = 1.0f;
constexpr float OVERLAY_FADE_DURATION_SEC = 0.25f;

// Double/triple click detection
constexpr int MULTI_CLICK_DISTANCE = 4;
constexpr Uint64 MULTI_CLICK_TIME = 500;

// Convert SDL modifier state to CEF modifier flags
// CEF flags: SHIFT=1<<1, CTRL=1<<2, ALT=1<<3, CMD=1<<7
int sdlModsToCef(SDL_Keymod sdlMods) {
    int cef = 0;
    if (sdlMods & SDL_KMOD_SHIFT) cef |= (1 << 1);  // EVENTFLAG_SHIFT_DOWN
    if (sdlMods & SDL_KMOD_CTRL)  cef |= (1 << 2);  // EVENTFLAG_CONTROL_DOWN
    if (sdlMods & SDL_KMOD_ALT)   cef |= (1 << 3);  // EVENTFLAG_ALT_DOWN
#ifdef __APPLE__
    if (sdlMods & SDL_KMOD_GUI)   cef |= (1 << 7);  // EVENTFLAG_COMMAND_DOWN (Cmd key)
#endif
    return cef;
}

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

    // Signal main loop to stay in poll mode while cursor is at a resize edge.
    // This ensures configure events during interactive resize are processed
    // without blocking, since SDL_WaitEvent processes them one-at-a-time.
    auto* at_edge = static_cast<std::atomic<bool>*>(data);
    at_edge->store(result != SDL_HITTEST_NORMAL, std::memory_order_relaxed);

    return result;
}

int main(int argc, char* argv[]) {
    // CEF subprocesses inherit this env var - skip our arg parsing entirely
    bool is_cef_subprocess = (getenv("JELLYFIN_CEF_SUBPROCESS") != nullptr);

    // Parse arguments (main process only)
    SDL_LogPriority log_level = SDL_LOG_PRIORITY_INFO;
    bool use_dmabuf = false;  // Disable DMA-BUF by default (can cause system freezes)
    bool disable_gpu_compositing = false;
    const char* hwdec = "auto-safe";
    if (!is_cef_subprocess) {
        const char* log_level_str = nullptr;
        const char* log_file_path = nullptr;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                printf("Usage: jellyfin-desktop-cef [options]\n"
                       "\nOptions:\n"
                       "  -h, --help              Show this help message\n"
                       "  -v, --version           Show version information\n"
                       "  --log-level <level>     Set log level (verbose|debug|info|warn|error)\n"
                       "  --log-file <path>       Write logs to file (with timestamps)\n"
#if !defined(__APPLE__) && !defined(_WIN32)
                       "  --dmabuf                Enable DMA-BUF zero-copy CEF rendering (experimental)\n"
#endif
                       "  --disable-gpu-compositing  Disable Chromium GPU compositing\n"
                       "  --hwdec <mode>          Set mpv hardware decoding mode (default: auto-safe)\n"
                       );
                return 0;
            } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
                printf("jellyfin-desktop-cef %s\n", APP_VERSION_STRING);
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
            } else if (strcmp(argv[i], "--dmabuf") == 0) {
                use_dmabuf = true;
            } else if (strcmp(argv[i], "--hwdec") == 0) {
                hwdec = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i] : "auto-safe";
            } else if (strncmp(argv[i], "--hwdec=", 8) == 0) {
                hwdec = argv[i] + 8;
            } else if (argv[i][0] == '-') {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                return 1;
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
            g_log_file = fopen(log_file_path, "a");
            if (!g_log_file) {
                fprintf(stderr, "Failed to open log file: %s\n", log_file_path);
                return 1;
            }
        }

        initLogging(log_level);

        // Startup banner
        LOG_INFO(LOG_MAIN, "jellyfin-desktop-cef " APP_VERSION_STRING " built " __DATE__ " " __TIME__);
        LOG_INFO(LOG_MAIN, "CEF " CEF_VERSION);
#if !defined(__APPLE__) && !defined(_WIN32)
        if (use_dmabuf) {
            LOG_INFO(LOG_MAIN, "DMA-BUF zero-copy CEF rendering enabled (experimental)");
        }
#endif
    }

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

    // Single-instance check: signal existing instance to raise, then exit
    if (trySignalExisting()) {
        return 0;
    }

#if defined(__APPLE__) && defined(NDEBUG)
    // In release builds, offer to move app to /Applications (clears quarantine)
    PFMoveToApplicationsFolderIfNecessary();
#endif

    SDL_SetAppMetadata("Jellyfin Desktop CEF", nullptr, "org.jellyfin.JellyfinDesktopCEF");

    // SDL initialization with OpenGL (for main surface CEF overlay)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR(LOG_MAIN, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Register custom event for cross-thread main loop wake-up
    static Uint32 SDL_EVENT_WAKE = SDL_RegisterEvents(1);
    auto wakeMainLoop = []() {
        SDL_Event event{};
        event.type = SDL_EVENT_WAKE;
        SDL_PushEvent(&event);
#ifdef __APPLE__
        // Wake NSApplication event loop (we use waitForMacEvent instead of SDL_WaitEvent)
        wakeMacEventLoop();
#endif
    };

#ifdef __APPLE__
    // macOS: CEF uses external_message_pump, so we need to wake the main loop
    // when CEF schedules work (otherwise SDL_WaitEvent blocks indefinitely)
    App::SetWakeCallback(wakeMainLoop);
#endif

    // Single-instance listener: raise window when another instance signals us
    std::mutex raise_mutex;
    std::string pending_activation_token;
    bool raise_requested = false;
    startListener([&raise_mutex, &pending_activation_token, &raise_requested, &wakeMainLoop](const std::string& token) {
        std::lock_guard<std::mutex> lock(raise_mutex);
        pending_activation_token = token;
        raise_requested = true;
        wakeMainLoop();
    });

    int width = 1280;
    int height = 720;

    // Use plain Wayland window - we create our own EGL context
    // SDL_WINDOW_HIGH_PIXEL_DENSITY enables HiDPI support
    SDL_Window* window = SDL_CreateWindow(
        "Jellyfin Desktop CEF",
        width, height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );

    if (!window) {
        LOG_ERROR(LOG_MAIN, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    std::atomic<bool> cursor_at_resize_edge{false};
    SDL_SetWindowHitTest(window, windowHitTest, &cursor_at_resize_edge);
    SDL_StartTextInput(window);

    // Restore saved window geometry (size, position, maximized)
    restoreWindowGeometry(window);
    SDL_GetWindowSize(window, &width, &height);

#ifdef __APPLE__
    // Window activation is deferred until first WINDOW_EXPOSED event
    // to ensure the window is actually visible before activating
#endif

#ifdef __APPLE__
    // Create video stack
    VideoStack videoStack = VideoStack::create(window, width, height, hwdec);
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
    VideoStack videoStack = VideoStack::create(window, width, height, &wgl, hwdec);
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

    // Create video stack (detects Wayland vs X11 internally)
    VideoStack videoStack = VideoStack::create(window, width, height, &egl, hwdec);
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

    // Compositor context for BrowserEntry init
    // Use SDL physical size - resize handler will update when Wayland reports actual scale
    int physical_width, physical_height;
    SDL_GetWindowSizeInPixels(window, &physical_width, &physical_height);
    LOG_INFO(LOG_WINDOW, "HiDPI: logical=%dx%d physical=%dx%d",
             width, height, physical_width, physical_height);

    CompositorContext compositor_ctx;
    compositor_ctx.gl_context = &egl;
#endif

    initWindowActivation(window);

    // Load settings
    Settings::instance().load();

    // CEF settings (CefThread sets external_message_pump)
    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;

#ifdef __APPLE__
    // macOS: Set framework path (cef_framework_path set earlier during CEF loading)
    CefString(&settings.framework_dir_path).FromString((cef_framework_path / "Chromium Embedded Framework.framework").string());
    // Use main executable as subprocess - it handles CefExecuteProcess early
    CefString(&settings.browser_subprocess_path).FromString((exe_path / "jellyfin-desktop-cef").string());
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
        cache_path = std::filesystem::path(appdata) / "jellyfin-desktop-cef";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        cache_path = std::filesystem::path(home) / "Library" / "Caches" / "jellyfin-desktop-cef";
    }
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        cache_path = std::filesystem::path(xdg) / "jellyfin-desktop-cef";
    } else if (const char* home = std::getenv("HOME")) {
        cache_path = std::filesystem::path(home) / ".cache" / "jellyfin-desktop-cef";
    }
#endif
    if (!cache_path.empty()) {
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
    auto overlay_compositor = std::make_unique<MetalCompositor>();
    overlay_compositor->init(window, physical_width, physical_height);
    LOG_DEBUG(LOG_COMPOSITOR, "Pre-created overlay Metal compositor");

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
#else
    // Windows/Linux: Start CEF on dedicated thread
    CefThread cefThread;
    if (!cefThread.start(main_args, settings, app)) {
        LOG_ERROR(LOG_CEF, "CefThread start failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
#endif

    // Browser stack manages all browsers and their paint buffers
    BrowserStack browsers;
    bool paint_size_matched = true;  // Track if last paint matched compositor size

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
    mediaSession.addBackend(createMprisBackend(&mediaSession));
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
    OverlayState overlay_state = OverlayState::SHOWING;
    std::chrono::steady_clock::time_point overlay_fade_start;
    float overlay_browser_alpha = 1.0f;
    float clear_color = 16.0f / 255.0f;  // #101010 until fade begins
    std::string pending_server_url;

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

    // Create overlay browser entry
    auto overlay_entry = std::make_unique<BrowserEntry>();
    BrowserEntry* overlay_ptr = overlay_entry.get();  // save pointer before move
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
    CefRefPtr<OverlayClient> overlay_client(new OverlayClient(width, height,
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
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_server_url = url;
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
        , [overlay_ptr](void* surface, int format, int w, int h) {
            overlay_ptr->compositor->queueIOSurface(surface, format, w, h);
        }
#endif
    ));
    overlay_ptr->client = overlay_client;
    overlay_ptr->getBrowser = [overlay_client]() { return overlay_client->browser(); };
    overlay_ptr->resizeBrowser = [overlay_client](int w, int h, int pw, int ph) { overlay_client->resize(w, h, pw, ph); };
    overlay_ptr->getInputReceiver = [overlay_client]() -> InputReceiver* { return overlay_client.get(); };
    overlay_ptr->isClosed = [overlay_client]() { return overlay_client->isClosed(); };
    overlay_ptr->input_layer = std::make_unique<BrowserLayer>(overlay_client.get());
    overlay_ptr->input_layer->setWindowSize(width, height);
    overlay_ptr->wake_main_loop = wakeMainLoop;
    browsers.add("overlay", std::move(overlay_entry));

    // Track who initiated fullscreen (only changes from NONE, returns to NONE on exit)
    enum class FullscreenSource { NONE, WM, CEF };
    FullscreenSource fullscreen_source = FullscreenSource::NONE;
    bool was_maximized_before_fullscreen = false;

    // Create main browser entry
    auto main_entry = std::make_unique<BrowserEntry>();
    BrowserEntry* main_ptr = main_entry.get();  // save pointer before move
#ifdef __APPLE__
    // Use pre-created Metal compositor (avoids startup delay)
    main_ptr->setCompositor(std::move(main_compositor));
#else
    if (!main_ptr->initCompositor(compositor_ctx, physical_width, physical_height)) {
        LOG_ERROR(LOG_COMPOSITOR, "Main compositor init failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
#endif
    auto main_paint_cb = main_ptr->makePaintCallback();

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
        getPhysicalSize
#ifdef __APPLE__
        // IOSurface callback for macOS accelerated paint - queue for import on main thread
        , [main_ptr](void* surface, int format, int w, int h) {
            main_ptr->compositor->queueIOSurface(surface, format, w, h);
        }
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

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
#ifdef __APPLE__
    window_info.shared_texture_enabled = true;  // macOS: use IOSurface zero-copy
#elif defined(_WIN32)
    window_info.shared_texture_enabled = false;  // No DirectX interop yet, use OnPaint
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
    CefWindowInfo overlay_window_info;
    overlay_window_info.SetAsWindowless(0);
#ifdef __APPLE__
    overlay_window_info.shared_texture_enabled = true;  // macOS: use IOSurface zero-copy
#elif defined(_WIN32)
    overlay_window_info.shared_texture_enabled = false;  // No DirectX interop yet, use OnPaint
#else
    overlay_window_info.shared_texture_enabled = use_dmabuf;  // Linux: dmabuf zero-copy
#endif
    CefBrowserSettings overlay_browser_settings;
    overlay_browser_settings.background_color = 0;
    overlay_browser_settings.windowless_frame_rate = browser_settings.windowless_frame_rate;

    std::string overlay_html_path = "app://resources/index.html";
    CefBrowserHost::CreateBrowser(overlay_window_info, overlay_client, overlay_html_path, overlay_browser_settings, nullptr, nullptr);

    // State tracking
    using Clock = std::chrono::steady_clock;

    // Main browser: load saved server immediately, or wait for overlay IPC
    std::string saved_url = Settings::instance().serverUrl();
    if (saved_url.empty()) {
        // No saved server - create with blank, wait for overlay loadServer IPC
        LOG_INFO(LOG_MAIN, "Waiting for overlay to provide server URL");
        CefBrowserHost::CreateBrowser(window_info, client, "about:blank", browser_settings, nullptr, nullptr);
    } else {
        // Have saved server - start loading immediately, begin overlay fade
        overlay_state = OverlayState::WAITING;
        overlay_fade_start = Clock::now();
        LOG_INFO(LOG_MAIN, "Loading saved server: %s", saved_url.c_str());
        CefBrowserHost::CreateBrowser(window_info, client, saved_url, browser_settings, nullptr, nullptr);
    }
    // Input routing stack - use BrowserStack for input layers
    MenuLayer menu_layer(&menu);
    InputStack input_stack;
    input_stack.push(browsers.getInputLayer("overlay"));  // Start with overlay

    // Track which browser layer is active (for WindowStateNotifier)
    BrowserLayer* active_browser = browsers.getInputLayer("overlay");

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
    // Windows: OpenGL with shared WGL context + FBO
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
#endif

#ifdef __APPLE__
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
                mediaSessionThread.setPosition(static_cast<int64_t>(ev.value * 1000.0));
                break;
            case MpvEvent::Type::Duration:
                client->updateDuration(ev.value);
                break;
            case MpvEvent::Type::Playing:
                // Restore video state - loadfile replacement fires END_FILE(STOP)
                // for the old file which triggers Canceled, resetting has_video.
                // FILE_LOADED (Playing event) follows, so re-enable video here.
                if (!has_video) {
                    has_video = true;
                    LOG_INFO(LOG_MAIN, "Video restored after file transition");
#ifdef __APPLE__
                    videoRenderer.setVisible(true);
#else
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
            case MpvEvent::Type::Seeked:
                client->updatePosition(ev.value);
                mediaSessionThread.setPosition(static_cast<int64_t>(ev.value * 1000.0));
                mediaSessionThread.setRate(current_playback_rate);
                mediaSessionThread.emitSeeked(static_cast<int64_t>(ev.value * 1000.0));
                break;
            case MpvEvent::Type::Buffering:
                mediaSessionThread.setPosition(static_cast<int64_t>(ev.value * 1000.0));
                mediaSessionThread.setRate(ev.flag ? 0.0 : current_playback_rate);
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
        bool at_resize_edge = cursor_at_resize_edge.load(std::memory_order_relaxed);
        if (needs_render || has_video || has_pending || has_pending_cmds || !paint_size_matched || at_resize_edge) {
            have_event = SDL_PollEvent(&event);
        } else {
#ifdef __APPLE__
            // Pump CEF before waiting - this processes any pending tasks and
            // may schedule more work (which will wake us via OnScheduleMessagePumpWork)
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
                // Wait using NSApplication's event loop - properly integrates
                // Cocoa events, CFRunLoop sources, and Mojo IPC
                waitForMacEvent();
                have_event = SDL_PollEvent(&event);
            }
#else
            // Idle: block until SDL event (input, window, or CEF wake callback)
            have_event = SDL_WaitEvent(&event);
#endif
        }

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
                client->exitFullscreen();
                if (fullscreen_source == FullscreenSource::WM) {
                    fullscreen_source = FullscreenSource::NONE;
                }
                break;

            case SDL_EVENT_WINDOW_RESIZED: {
                current_width = event.window.data1;
                current_height = event.window.data2;

                // Get physical dimensions for compositor resize
                int physical_w, physical_h;
                SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);

                // Only invalidate paint match when size actually changed,
                // otherwise a duplicate configure event resets the flag after
                // CEF already painted at the correct size (causing stale black border)
                if (physical_w != static_cast<int>(main_ptr->compositor->width()) ||
                    physical_h != static_cast<int>(main_ptr->compositor->height())) {
                    paint_size_matched = false;
                }

                // Resize all browsers and compositors via BrowserStack
                browsers.resizeAll(current_width, current_height, physical_w, physical_h);

#ifdef __APPLE__
                videoRenderer.resize(physical_w, physical_h);
#elif defined(_WIN32)
                // Resize WGL context
                wgl.resize(current_width, current_height);
                videoController.requestResize(current_width, current_height);
#else
                // Resize EGL context
                egl.resize(physical_w, physical_h);

                // Resize video layer on render thread (no-op for X11/OpenGL)
                videoController.requestResize(physical_w, physical_h);
                videoRenderer.setDestinationSize(current_width, current_height);

                // Track resize time for paint matching
                last_resize_time = Clock::now();
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
        // macOS: Always pump CEF - scheduling controls actual work frequency
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
                        LOG_INFO(LOG_MAIN, "Video loaded, has_video=true");
#ifdef __APPLE__
                        videoRenderer.setVisible(true);
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
                    // Rate was encoded as rate * 1000000
                    double rate = static_cast<double>(cmd.intArg) / 1000000.0;
                    current_playback_rate = rate;
                    mediaSessionThread.setRate(rate);
                } else if (cmd.cmd == "media_seeked") {
                    // JS detected a seek - emit Seeked signal to media session
                    int64_t pos_us = static_cast<int64_t>(cmd.intArg) * 1000;
                    mediaSessionThread.emitSeeked(pos_us);
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
            }
        }

        // Menu overlay blending
        menu.clearRedraw();

        // Render video to subsurface/layer
#ifdef __APPLE__
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
#elif defined(_WIN32)
        // Windows: Threaded OpenGL rendering with FBO compositing
        glViewport(0, 0, current_width, current_height);
        frameContext.beginFrame(clear_color, videoController.getClearAlpha());
        videoController.render(current_width, current_height);

        // Composite video texture (from threaded FBO)
        videoRenderer.composite(current_width, current_height);

        // Flush and composite all browsers (back-to-front order)
        browsers.renderAll(current_width, current_height);

        frameContext.endFrame();
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
#ifdef __APPLE__
    SDL_RemoveEventWatch(liveResizeCallback, &live_resize_ctx);
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
#else
    // Windows/Linux: wait for async browser close before cleanup
    browsers.closeAllBrowsers();
    while (!browsers.allBrowsersClosed()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    browsers.cleanupCompositors();
    videoRenderer.cleanup();
    VideoStack::cleanupStatics();
#ifdef _WIN32
    wgl.cleanup();
#else
    egl.cleanup();
#endif
    cefThread.shutdown();
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
