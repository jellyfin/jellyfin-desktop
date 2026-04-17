#pragma once

#include <mpv/client.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>

#include "logging.h"
#include "platform/display_backend.h"

/**
 * Typed wrapper for mpv_handle. Encapsulates the mpv instance so it doesn't
 * need to be passed around. All methods are typesafe and handle format
 * conversions internally.
 */
class MpvHandle {
private:
    mpv_handle* handle_;

public:
    explicit MpvHandle(mpv_handle* h = nullptr) : handle_(h) {}

    // =====================================================================
    // Creation and initialization
    // =====================================================================

    static MpvHandle Create(DisplayBackend display) {
        MpvHandle mpv(mpv_create());
        if (mpv.IsValid())
            mpv.SetDefaults(display);
        return mpv;
    }

    int Initialize() {
        return mpv_initialize(handle_);
    }

    void TerminateDestroy() {
        if (handle_) {
            mpv_terminate_destroy(handle_);
            handle_ = nullptr;
        }
    }

    // =====================================================================
    // Options (must be set before Initialize)
    // =====================================================================

    void SetOptionString(const std::string& name, const std::string& value) {
        mpv_set_option_string(handle_, name.c_str(), value.c_str());
    }

    void SetOptionInt(const std::string& name, int64_t value) {
        mpv_set_option(handle_, name.c_str(), MPV_FORMAT_INT64, &value);
    }

    void SetOptionFlag(const std::string& name, bool value) {
        int flag = value ? 1 : 0;
        mpv_set_option(handle_, name.c_str(), MPV_FORMAT_FLAG, &flag);
    }

    // Typed option setters (must be called before Initialize)
    void SetHwdec(const std::string& mode)          { SetOptionString("hwdec", mode); }
    void SetAudioSpdif(const std::string& codecs)    { SetOptionString("audio-spdif", codecs); }
    void SetAudioExclusive(bool v)                   { SetOptionFlag("audio-exclusive", v); }
    void SetAudioChannels(const std::string& layout)  { SetOptionString("audio-channels", layout); }

    // =====================================================================
    // Property access (synchronous - safe in main thread)
    // =====================================================================

    int GetPropertyString(const std::string& name, std::string& out) {
        char* s = nullptr;
        int err = mpv_get_property(handle_, name.c_str(), MPV_FORMAT_OSD_STRING, &s);
        if (err >= 0 && s) {
            out = s;
            mpv_free(s);
        }
        return err;
    }

    int GetPropertyInt(const std::string& name, int64_t& out) {
        return mpv_get_property(handle_, name.c_str(), MPV_FORMAT_INT64, &out);
    }

    int GetPropertyDouble(const std::string& name, double& out) {
        return mpv_get_property(handle_, name.c_str(), MPV_FORMAT_DOUBLE, &out);
    }

    int GetPropertyFlag(const std::string& name, bool& out) {
        int flag = 0;
        int err = mpv_get_property(handle_, name.c_str(), MPV_FORMAT_FLAG, &flag);
        out = (flag != 0);
        return err;
    }

    int GetPropertyNode(const std::string& name, mpv_node& out) {
        return mpv_get_property(handle_, name.c_str(), MPV_FORMAT_NODE, &out);
    }

    // =====================================================================
    // Player API (dedicated typed methods)
    // =====================================================================

    // Transport control
    void Play()                          { SetPropertyFlagAsync("pause", false); }
    void Pause()                         { SetPropertyFlagAsync("pause", true); }
    void TogglePause()                   { CyclePauseAsync(); }
    void Stop()                          { StopAsync(); }
    void SeekAbsolute(double seconds)    { SeekAsync(seconds); }

    // Media properties
    void SetVolume(double vol)           { SetPropertyDoubleAsync("volume", vol); }
    void SetMuted(bool muted)            { SetPropertyFlagAsync("mute", muted); }
    void SetSpeed(double rate)           { SetPropertyDoubleAsync("speed", rate); }
    void SetAudioTrack(int64_t id)       { SetPropertyIntAsync("aid", id); }
    void SetSubtitleTrack(int64_t id)    { SetPropertyIntAsync("sid", id); }
    void SetAudioDelay(double secs)      { SetPropertyDoubleAsync("audio-delay", secs); }
    void SetStartPosition(double secs)   { SetPropertyDoubleAsync("start", secs); }    void SubAdd(const std::string& url)   { CommandAsync({"sub-add", url, "select"}); }
    // mpv track selection: -1 = auto, 0 = disable, 1+ = specific track
    static constexpr int64_t kTrackAuto    = -1;
    static constexpr int64_t kTrackDisable =  0;

    struct LoadOptions {
        double startSecs = 0;
        int64_t audioTrack = kTrackAuto;
        int64_t subTrack = kTrackAuto;
    };

    void LoadFile(const std::string& path, const LoadOptions& opts) {
        std::string optsStr = "start=" + std::to_string(opts.startSecs)
                            + ",pause=no";
        if (opts.audioTrack != kTrackAuto)
            optsStr += ",aid=" + std::to_string(opts.audioTrack);
        if (opts.subTrack != kTrackAuto)
            optsStr += ",sid=" + std::to_string(opts.subTrack);
        CommandAsync({"loadfile", path, "replace", "-1", optsStr});
    }

    inline static const std::unordered_map<std::string, std::pair<bool, double>> kAspectModes = {
        {"auto",  {true,  0.0}},
        {"cover", {true,  1.0}},
        {"fill",  {false, 0.0}},
    };

    void SetAspectMode(const std::string& mode) {
        auto it = kAspectModes.find(mode);
        if (it == kAspectModes.end()) {
            LOG_WARN(LOG_MPV, "SetAspectMode: unknown mode {}", mode.c_str());
            return;
        }
        SetPropertyFlagAsync("keepaspect", it->second.first);
        SetPropertyDoubleAsync("panscan", it->second.second);
    }

    // Window/display state
    void SetFullscreen(bool fs)          { SetPropertyFlagAsync("fullscreen", fs); }
    void ToggleFullscreen()              { CycleFullscreenAsync(); }
    void SetBackgroundColor(const std::string& color) { SetPropertyStringAsync("background-color", color); }
    void SetForceWindowPosition(bool v)  { SetPropertyFlagAsync("force-window-position", v); }

    int GetFullscreen(bool& out)         { return GetPropertyFlag("fullscreen", out); }
    int GetOsdWidth(int64_t& out)        { return GetPropertyInt("osd-width", out); }
    int GetOsdHeight(int64_t& out)       { return GetPropertyInt("osd-height", out); }
    int GetWindowId(int64_t& out)        { return GetPropertyInt("window-id", out); }
    int GetWindowMaximized(bool& out)    { return GetPropertyFlag("window-maximized", out); }
    int GetDisplayScale(double& out)     { return GetPropertyDouble("display-hidpi-scale", out); }

    // Wayland platform pointers
    int GetWaylandDisplay(intptr_t& out) {
        int64_t val = 0;
        int err = GetPropertyInt("wayland-display", val);
        out = static_cast<intptr_t>(val);
        return err;
    }
    int GetWaylandSurface(intptr_t& out) {
        int64_t val = 0;
        int err = GetPropertyInt("wayland-surface", val);
        out = static_cast<intptr_t>(val);
        return err;
    }
    int GetWaylandConfigureCbPtr(intptr_t& out) {
        int64_t val = 0;
        int err = GetPropertyInt("wayland-configure-cb-ptr", val);
        out = static_cast<intptr_t>(val);
        return err;
    }
    int GetWaylandCloseCbPtr(intptr_t& out) {
        int64_t val = 0;
        int err = GetPropertyInt("wayland-close-cb-ptr", val);
        out = static_cast<intptr_t>(val);
        return err;
    }

private:
    // =====================================================================
    // Default options (called by Create)
    // =====================================================================

    void SetDefaults(DisplayBackend display) {
#ifdef __APPLE__
        setenv("MPVBUNDLE", "true", 1);
#endif

        // Disable OSD/OSC — CEF overlay handles all UI
        SetOptionString("osd-level", "0");
        SetOptionString("osc", "no");
        SetOptionString("display-tags", "");

        // Disable all mpv input — we own input and route through CEF
        SetOptionString("input-default-bindings", "no");
        SetOptionString("input-vo-keyboard", "no");
        SetOptionString("input-vo-cursor", "no");
        SetOptionString("input-cursor", "no");
        // X11's WM_DELETE_WINDOW routes through mpv's input system as
        // CLOSE_WIN — input-keyboard=no drops it, breaking the close button.
#if defined(_WIN32) || defined(__APPLE__)
        SetOptionString("input-keyboard", "no");
#else
        if (display == DisplayBackend::Wayland)
            SetOptionString("input-keyboard", "no");
#endif

        // Window behavior
        SetOptionString("stop-screensaver", "no");
        SetOptionString("keepaspect-window", "no");
        SetOptionString("auto-window-resize", "no");
        SetOptionString("border", "yes");
        SetOptionString("title", "Jellyfin Desktop");
        SetOptionString("wayland-app-id", "org.jellyfin.JellyfinDesktop");
#ifdef _WIN32
        // Tell mpv to load window icon from our exe resources
        _putenv_s("MPV_WINDOW_ICON", "IDI_ICON1");
#endif

        // Keep window open when idle (no media loaded).
        // force-window=yes (not "immediate") avoids a macOS deadlock:
        // "immediate" calls handle_force_window during mpv_initialize, which
        // triggers DispatchQueue.main.sync while main is blocked in init.
        SetOptionString("force-window", "yes");
        SetOptionString("idle", "yes");
    }

    // =====================================================================
    // Property modification (asynchronous - safe from any thread)
    // =====================================================================

    void SetPropertyStringAsync(const std::string& name, const std::string& value) {
        const char* v = value.c_str();
        mpv_set_property_async(handle_, 0, name.c_str(), MPV_FORMAT_STRING, &v);
    }

    void SetPropertyIntAsync(const std::string& name, int64_t value) {
        mpv_set_property_async(handle_, 0, name.c_str(), MPV_FORMAT_INT64,
                               const_cast<int64_t*>(&value));
    }

    void SetPropertyDoubleAsync(const std::string& name, double value) {
        mpv_set_property_async(handle_, 0, name.c_str(), MPV_FORMAT_DOUBLE,
                               const_cast<double*>(&value));
    }

    void SetPropertyFlagAsync(const std::string& name, bool value) {
        int flag = value ? 1 : 0;
        mpv_set_property_async(handle_, 0, name.c_str(), MPV_FORMAT_FLAG,
                               const_cast<int*>(&flag));
    }

    // =====================================================================
    // Commands (internal)
    // =====================================================================

    // Helper to build mpv command array from string vector
    static const char** BuildCommandArray(const std::vector<std::string>& args) {
        if (args.empty()) return nullptr;
        const char** c = new const char*[args.size() + 1];
        for (size_t i = 0; i < args.size(); i++) {
            c[i] = args[i].c_str();
        }
        c[args.size()] = nullptr;
        return c;
    }

    void CommandAsync(const std::vector<std::string>& args) {
        const char** c = BuildCommandArray(args);
        if (!c) return;
        mpv_command_async(handle_, 0, c);
        delete[] c;
    }

    void Command(const std::vector<std::string>& args) {
        const char** c = BuildCommandArray(args);
        if (!c) return;
        mpv_command(handle_, c);
        delete[] c;
    }

    // Common commands as convenience methods
    void LoadFileAsync(const std::string& path) {
        CommandAsync({"loadfile", path});
    }

    void SeekAsync(double seconds) {
        CommandAsync({"seek", std::to_string(seconds), "absolute"});
    }

    void StopAsync() {
        CommandAsync({"stop"});
    }

    void CycleFullscreenAsync() {
        CommandAsync({"cycle", "fullscreen"});
    }

    void CyclePauseAsync() {
        CommandAsync({"cycle", "pause"});
    }

public:

    // =====================================================================
    // Property observation
    // =====================================================================

    void ObserveProperty(uint64_t reply_userdata, const std::string& name,
                         mpv_format format) {
        mpv_observe_property(handle_, reply_userdata, name.c_str(), format);
    }

    // Convenience: observe properties with standard formats
    void ObservePropertyInt(uint64_t id, const std::string& name) {
        ObserveProperty(id, name, MPV_FORMAT_INT64);
    }

    void ObservePropertyDouble(uint64_t id, const std::string& name) {
        ObserveProperty(id, name, MPV_FORMAT_DOUBLE);
    }

    void ObservePropertyFlag(uint64_t id, const std::string& name) {
        ObserveProperty(id, name, MPV_FORMAT_FLAG);
    }

    void ObservePropertyNode(uint64_t id, const std::string& name) {
        ObserveProperty(id, name, MPV_FORMAT_NODE);
    }

    // =====================================================================
    // Events
    // =====================================================================

    void SetWakeupCallback(void (*cb)(void*), void* data) {
        mpv_set_wakeup_callback(handle_, cb, data);
    }

    void RequestLogMessages(const char* level) {
        mpv_request_log_messages(handle_, level);
    }

    mpv_event* WaitEvent(double timeout) {
        return mpv_wait_event(handle_, timeout);
    }

    void Wakeup() {
        mpv_wakeup(handle_);
    }

    // =====================================================================
    // Accessors
    // =====================================================================

    mpv_handle* Get() const { return handle_; }

    void Set(mpv_handle* h) {
        handle_ = h;
    }

    operator mpv_handle*() const { return handle_; }

    bool IsValid() const { return handle_ != nullptr; }

    explicit operator bool() const { return IsValid(); }
};
