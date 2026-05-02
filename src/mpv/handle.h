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
    //
    // Prefer mpv::* accessors (src/mpv/event.h) when a property is already
    // observed — they read an atomic seeded by the event handler. Sync
    // reads only belong here for init-time, one-shot values mpv doesn't
    // expose as observable properties (window-id, wayland-display/surface,
    // callback pointers).
    // =====================================================================

    int GetPropertyInt(const std::string& name, int64_t& out) {
        return mpv_get_property(handle_, name.c_str(), MPV_FORMAT_INT64, &out);
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
    void SetAudioTrack(int64_t id)       { SetPropertyStringAsync("aid", TrackToMpvStr(id)); }
    void SetSubtitleTrack(int64_t id)    { SetPropertyStringAsync("sid", TrackToMpvStr(id)); }
    void SetAudioDelay(double secs)      { SetPropertyDoubleAsync("audio-delay", secs); }
    void SetStartPosition(double secs)   { SetPropertyDoubleAsync("start", secs); }
    void SubAdd(const std::string& url)   { CommandAsync({"sub-add", url, "select"}); }
    void AudioAdd(const std::string& url) { CommandAsync({"audio-add", url, "select"}); }
    // Public sentinels: 0 = disable, 1+ = specific track id. mpv auto track
    // selection is completely disabled (track-auto-selection=no in
    // SetDefaults) as it conflicts with the fact that jellyfin-web is
    // ultimately responsible for track selection.
    static constexpr int64_t kTrackDisable =  0;

    struct LoadOptions {
        double startSecs = 0;
        int64_t videoTrack = 1;                // we always want the (single) video track
        int64_t audioTrack = kTrackDisable;
        int64_t subTrack   = kTrackDisable;
        std::string externalAudioUrl;          // empty = none
        std::string externalSubUrl;            // empty = none
    };

    void LoadFile(const std::string& path, const LoadOptions& opts) {
        // Track selection is owned by Jellyfin. With track-auto-selection=no,
        // mpv silently drops aid=/vid=/sid= passed in loadfile options
        // (loadfile.c:1850-1858 skips select_default_track entirely). We
        // therefore load the file *paused* with no selectors, stash the
        // intended ids, and apply them via property writes after FILE_LOADED.
        // The async writes + final pause=false are FIFO-ordered on mpv's core
        // thread, so playback only begins after track-switch reinits land.
        pendingVid_ = opts.videoTrack;
        pendingAid_ = opts.audioTrack;
        pendingSid_ = opts.subTrack;
        pendingExternalAudioUrl_ = opts.externalAudioUrl;
        pendingExternalSubUrl_ = opts.externalSubUrl;
        pendingValid_ = true;

        std::string optsStr = "start=" + std::to_string(opts.startSecs)
                            + ",pause=yes";
        CommandAsync({"loadfile", path, "replace", "-1", optsStr});
    }

    // Called from the FILE_LOADED event handler. Queues the pending
    // vid/aid/sid property writes, then audio-add / sub-add for any
    // external streams (their `select` flag picks the new track), then
    // pause=false. mpv processes these in submission order on its core
    // thread, so the unpause runs after the external files' demuxers are
    // opened and their tracks selected — same gating as internal tracks.
    void ApplyPendingTrackSelectionAndPlay() {
        if (!pendingValid_) return;
        SetPropertyStringAsync("vid", TrackToMpvStr(pendingVid_));
        SetPropertyStringAsync("aid", TrackToMpvStr(pendingAid_));
        SetPropertyStringAsync("sid", TrackToMpvStr(pendingSid_));
        if (!pendingExternalAudioUrl_.empty())
            CommandAsync({"audio-add", pendingExternalAudioUrl_, "select"});
        if (!pendingExternalSubUrl_.empty())
            CommandAsync({"sub-add", pendingExternalSubUrl_, "select"});
        SetPropertyFlagAsync("pause", false);
        pendingValid_ = false;
        pendingExternalAudioUrl_.clear();
        pendingExternalSubUrl_.clear();
    }

private:
    // Translate our public sentinel to mpv's TRACKCHOICE string form. Must
    // be sent as a string: mpv's choice_set converts MPV_FORMAT_INT64 via
    // snprintf+parse_choice, and parse_choice falls through to numeric
    // range parsing on a non-name match, where M_RANGE(0, 8190) rejects
    // -2 ("no") as out-of-range.
    static std::string TrackToMpvStr(int64_t id) {
        return id == kTrackDisable ? "no" : std::to_string(id);
    }

    int64_t pendingVid_ = 1;
    int64_t pendingAid_ = kTrackDisable;
    int64_t pendingSid_ = kTrackDisable;
    std::string pendingExternalAudioUrl_;
    std::string pendingExternalSubUrl_;
    bool pendingValid_ = false;

public:

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
    void SetWindowMinimized(bool v)      { SetPropertyFlagAsync("window-minimized", v); }
    void SetWindowMaximized(bool v)      { SetPropertyFlagAsync("window-maximized", v); }
    void SetBackgroundColor(const std::string& color) { SetPropertyStringAsync("background-color", color); }
    void SetForceWindowPosition(bool v)  { SetPropertyFlagAsync("force-window-position", v); }
    void SetGeometry(const std::string& geom) { SetPropertyStringAsync("geometry", geom); }

    int GetWindowId(int64_t& out)        { return GetPropertyInt("window-id", out); }

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

        // Track selection is owned by Jellyfin. Disable mpv's heuristic so
        // unspecified tracks stay disabled instead of being auto-picked
        // by language/default-flag/codec scoring.
        SetOptionString("track-auto-selection", "no");

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
