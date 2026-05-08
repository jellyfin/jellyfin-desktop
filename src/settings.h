#pragma once

#include <string>
#include <mutex>

class Settings {
public:
    static Settings& instance();

    bool load();
    bool save();
    void saveAsync();  // Fire-and-forget async save

    const std::string& serverUrl() const { return server_url_; }
    void setServerUrl(const std::string& url) { server_url_ = url; }

    struct WindowGeometry {
        // Defaults are in logical units. Scaled by the display DPI at
        // restore time so the window has the same apparent size on any
        // display, regardless of scale factor.
        static constexpr int kDefaultLogicalWidth = 1280;
        static constexpr int kDefaultLogicalHeight = 720;
        static constexpr int kDefaultPhysicalWidth = 1280;
        static constexpr int kDefaultPhysicalHeight = 720;
        static constexpr float kDefaultScale = 1.0f;

        int x = -1;          // -1 = not set (use default centering)
        int y = -1;
        int width = 0;           // pixel dims at save time; 0 = not set
        int height = 0;
        int logical_width = 0;   // logical dims at save time; 0 = not set
        int logical_height = 0;
        float scale = 0.f;       // display scale at save time; 0 = not set
        bool maximized = false;
    };

    const WindowGeometry& windowGeometry() const { return window_geometry_; }
    void setWindowGeometry(const WindowGeometry& geom) { window_geometry_ = geom; }

    // CLI-equivalent settings (persisted, overridden by CLI flags)
    const std::string& hwdec() const { return hwdec_; }
    void setHwdec(const std::string& v) { hwdec_ = v; }

    const std::string& audioPassthrough() const { return audio_passthrough_; }
    void setAudioPassthrough(const std::string& v) { audio_passthrough_ = v; }

    bool audioExclusive() const { return audio_exclusive_; }
    void setAudioExclusive(bool v) { audio_exclusive_ = v; }

    const std::string& audioChannels() const { return audio_channels_; }
    void setAudioChannels(const std::string& v) { audio_channels_ = v; }

    bool disableGpuCompositing() const { return disable_gpu_compositing_; }
    void setDisableGpuCompositing(bool v) { disable_gpu_compositing_ = v; }

    bool titlebarThemeColor() const { return titlebar_theme_color_; }
    void setTitlebarThemeColor(bool v) { titlebar_theme_color_ = v; }

    bool transparentTitlebar() const { return transparent_titlebar_; }
    void setTransparentTitlebar(bool v) { transparent_titlebar_ = v; }

    const std::string& logLevel() const { return log_level_; }
    void setLogLevel(const std::string& v) { log_level_ = v; }

    bool forceTranscoding() const { return force_transcoding_; }
    void setForceTranscoding(bool v) { force_transcoding_ = v; }

    // Device name override. Empty = use platform default (hostname).
    const std::string& deviceName() const { return device_name_; }
    void setDeviceName(const std::string& v);

    // System hostname, truncated to the server's 64-char DeviceName limit.
    // Used when no explicit override is set.
    static std::string platformDeviceName();

    // Stored override if non-empty, otherwise platformDeviceName().
    std::string effectiveDeviceName() const;

    // JSON string of CLI-equivalent settings (for injection into JS)
    std::string cliSettingsJson() const;

    Settings() = default;

private:
    std::string getConfigPath();

    std::string server_url_;
    WindowGeometry window_geometry_;

    // CLI-equivalent settings
    std::string hwdec_;
    std::string audio_passthrough_;
    bool audio_exclusive_ = false;
    std::string audio_channels_;
    bool disable_gpu_compositing_ = false;
    bool titlebar_theme_color_ = true;
    bool transparent_titlebar_ = true;
    std::string log_level_;
    bool force_transcoding_ = false;
    std::string device_name_;

    std::mutex save_mutex_;  // Prevent concurrent saves
};
