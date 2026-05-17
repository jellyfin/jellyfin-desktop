#pragma once

#include "config/config.h"
#include "mpv/options.h"
#include "paths/paths.h"

#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
std::string macosComputerName();  // defined in src/platform/macos.mm
#endif

// Header-only thin wrapper over the Rust settings singleton in
// src/config/src/lib.rs. State lives in Rust; this class just adapts the
// extern "C" surface to the std::string-flavored call sites.
class Settings {
public:
    static Settings& instance() {
        static Settings s;
        static const bool initialized = []() {
            jfn_settings_init((paths::getConfigDir() + "/settings.json").c_str());
            return true;
        }();
        (void)initialized;
        return s;
    }

    struct WindowGeometry {
        // Defaults are in logical units. Scaled by the display DPI at
        // restore time so the window has the same apparent size on any
        // display, regardless of scale factor.
        static constexpr int   kDefaultLogicalWidth  = 1600;
        static constexpr int   kDefaultLogicalHeight = 900;
        static constexpr int   kDefaultPhysicalWidth  = kDefaultLogicalWidth;
        static constexpr int   kDefaultPhysicalHeight = kDefaultLogicalHeight;
        static constexpr float kDefaultScale          = 1.0f;

        int   x = -1;
        int   y = -1;
        int   width = 0;
        int   height = 0;
        int   logical_width = 0;
        int   logical_height = 0;
        float scale = 0.f;
        bool  maximized = false;
    };

    bool load() { return jfn_settings_load(); }
    bool save() { return jfn_settings_save(); }
    void saveAsync() { jfn_settings_save_async(); }

    std::string serverUrl() const { return takeString(jfn_settings_get_server_url()); }
    void setServerUrl(const std::string& v) { jfn_settings_set_server_url(v.c_str()); }

    std::string hwdec() const { return takeString(jfn_settings_get_hwdec()); }
    void setHwdec(const std::string& v) { jfn_settings_set_hwdec(v.c_str()); }

    std::string audioPassthrough() const { return takeString(jfn_settings_get_audio_passthrough()); }
    void setAudioPassthrough(const std::string& v) { jfn_settings_set_audio_passthrough(v.c_str()); }

    bool audioExclusive() const { return jfn_settings_get_audio_exclusive(); }
    void setAudioExclusive(bool v) { jfn_settings_set_audio_exclusive(v); }

    std::string audioChannels() const { return takeString(jfn_settings_get_audio_channels()); }
    void setAudioChannels(const std::string& v) { jfn_settings_set_audio_channels(v.c_str()); }

    bool disableGpuCompositing() const { return jfn_settings_get_disable_gpu_compositing(); }
    void setDisableGpuCompositing(bool v) { jfn_settings_set_disable_gpu_compositing(v); }

    bool titlebarThemeColor() const { return jfn_settings_get_titlebar_theme_color(); }
    void setTitlebarThemeColor(bool v) { jfn_settings_set_titlebar_theme_color(v); }

    bool transparentTitlebar() const { return jfn_settings_get_transparent_titlebar(); }
    void setTransparentTitlebar(bool v) { jfn_settings_set_transparent_titlebar(v); }

    std::string logLevel() const { return takeString(jfn_settings_get_log_level()); }
    void setLogLevel(const std::string& v) { jfn_settings_set_log_level(v.c_str()); }

    bool forceTranscoding() const { return jfn_settings_get_force_transcoding(); }
    void setForceTranscoding(bool v) { jfn_settings_set_force_transcoding(v); }

    std::string deviceName() const { return takeString(jfn_settings_get_device_name()); }
    void setDeviceName(const std::string& v) {
        jfn_settings_set_device_name(v.c_str(), platformDeviceName().c_str());
    }

    static std::string platformDeviceName() {
#ifdef _WIN32
        wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD len = sizeof(buf) / sizeof(buf[0]);
        GetComputerNameW(buf, &len);
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buf, len,
                                           nullptr, 0, nullptr, nullptr);
        std::string out(utf8_len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, len, out.data(), utf8_len,
                            nullptr, nullptr);
#elif defined(__APPLE__)
        std::string out = macosComputerName();
#else
        char buf[256] = {};
        gethostname(buf, sizeof(buf) - 1);
        std::string out(buf);
#endif
        if (out.size() > kDeviceNameMax) out.resize(kDeviceNameMax);
        return out;
    }

    std::string effectiveDeviceName() const {
        std::string dn = deviceName();
        return dn.empty() ? platformDeviceName() : dn;
    }

    WindowGeometry windowGeometry() const {
        JfnWindowGeometry g{};
        jfn_settings_get_window_geometry(&g);
        WindowGeometry out;
        out.x = g.x;
        out.y = g.y;
        out.width = g.width;
        out.height = g.height;
        out.logical_width = g.logical_width;
        out.logical_height = g.logical_height;
        out.scale = g.scale;
        out.maximized = g.maximized;
        return out;
    }
    void setWindowGeometry(const WindowGeometry& g) {
        JfnWindowGeometry j{};
        j.x = g.x;
        j.y = g.y;
        j.width = g.width;
        j.height = g.height;
        j.logical_width = g.logical_width;
        j.logical_height = g.logical_height;
        j.scale = g.scale;
        j.maximized = g.maximized;
        jfn_settings_set_window_geometry(&j);
    }

    std::string cliSettingsJson() const {
        auto opts = hwdecOptions();
        std::vector<const char*> ptrs;
        ptrs.reserve(opts.size());
        for (const auto& o : opts) ptrs.push_back(o.c_str());
        char* p = jfn_settings_cli_json(
            platformDeviceName().c_str(),
            ptrs.empty() ? nullptr : ptrs.data(),
            ptrs.size());
        return takeString(p);
    }

private:
    Settings() = default;

    static constexpr size_t kDeviceNameMax = 64;

    static std::string takeString(char* p) {
        if (!p) return {};
        std::string s(p);
        jfn_settings_free_string(p);
        return s;
    }
};
