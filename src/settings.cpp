#include "settings.h"
#include "config/config.h"
#include "mpv/options.h"
#include "paths/paths.h"
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// Server's Devices.DeviceName column is varchar(64); clamp here to match.
static constexpr size_t kDeviceNameMax = 64;

Settings& Settings::instance() {
    static Settings instance;
    return instance;
}

std::string Settings::getConfigPath() {
    return paths::getConfigDir() + "/settings.json";
}

namespace {

// Build a JfnConfigData populated from the C++ Settings. String fields point
// into the Settings instance, so the data struct is only valid for the
// duration of one synchronous call into the Rust crate. Pass strings
// through a temporary buffer when the Rust call may run after this returns
// (see saveAsync).
JfnConfigData toData(const Settings& s) {
    JfnConfigData d{};
    jfn_config_init_defaults(&d);
    // Caller-owned strings: cast away const to fit the C struct definition;
    // the Rust side only reads them.
    d.server_url = const_cast<char*>(s.serverUrl().c_str());
    d.hwdec = const_cast<char*>(s.hwdec().c_str());
    d.audio_passthrough = const_cast<char*>(s.audioPassthrough().c_str());
    d.audio_channels = const_cast<char*>(s.audioChannels().c_str());
    d.log_level = const_cast<char*>(s.logLevel().c_str());
    d.device_name = const_cast<char*>(s.deviceName().c_str());

    const auto& g = s.windowGeometry();
    d.window_x = g.x;
    d.window_y = g.y;
    d.window_width = g.width;
    d.window_height = g.height;
    d.window_logical_width = g.logical_width;
    d.window_logical_height = g.logical_height;
    d.window_scale = g.scale;
    d.window_maximized = g.maximized;

    d.audio_exclusive = s.audioExclusive();
    d.disable_gpu_compositing = s.disableGpuCompositing();
    d.titlebar_theme_color = s.titlebarThemeColor();
    d.transparent_titlebar = s.transparentTitlebar();
    d.force_transcoding = s.forceTranscoding();
    // init_defaults zeroed the pointers; replace with our borrowed buffers
    // after, so the above pointer assignments stick.
    return d;
}

std::string takeString(char* p) {
    if (!p) return {};
    std::string s(p);
    return s;
}

}  // namespace

bool Settings::load() {
    JfnConfigData d{};
    jfn_config_init_defaults(&d);
    bool ok = jfn_config_load(getConfigPath().c_str(), &d);
    if (!ok) {
        jfn_config_free_data(&d);
        return false;
    }

    if (d.server_url) server_url_ = d.server_url;
    if (d.hwdec) hwdec_ = d.hwdec;
    if (d.audio_passthrough) audio_passthrough_ = d.audio_passthrough;
    if (d.audio_channels) audio_channels_ = d.audio_channels;
    if (d.log_level) log_level_ = d.log_level;
    if (d.device_name) {
        device_name_ = d.device_name;
        if (device_name_.size() > kDeviceNameMax) device_name_.resize(kDeviceNameMax);
    }

    window_geometry_.x = d.window_x;
    window_geometry_.y = d.window_y;
    window_geometry_.width = d.window_width;
    window_geometry_.height = d.window_height;
    window_geometry_.logical_width = d.window_logical_width;
    window_geometry_.logical_height = d.window_logical_height;
    window_geometry_.scale = d.window_scale;
    window_geometry_.maximized = d.window_maximized;

    audio_exclusive_ = d.audio_exclusive;
    disable_gpu_compositing_ = d.disable_gpu_compositing;
    titlebar_theme_color_ = d.titlebar_theme_color;
    transparent_titlebar_ = d.transparent_titlebar;
    force_transcoding_ = d.force_transcoding;

    jfn_config_free_data(&d);
    return true;
}

bool Settings::save() {
    JfnConfigData d = toData(*this);
    return jfn_config_save(getConfigPath().c_str(), &d, kHwdecDefault);
}

void Settings::saveAsync() {
    // Snapshot data into owned strings so the worker thread doesn't race
    // with mutations on the main thread.
    std::string path = getConfigPath();
    struct Snapshot {
        std::string server_url, hwdec, audio_passthrough, audio_channels, log_level, device_name;
        Settings::WindowGeometry geom;
        bool audio_exclusive, disable_gpu_compositing, titlebar_theme_color,
             transparent_titlebar, force_transcoding;
    };
    Snapshot snap{
        server_url_, hwdec_, audio_passthrough_, audio_channels_, log_level_, device_name_,
        window_geometry_,
        audio_exclusive_, disable_gpu_compositing_, titlebar_theme_color_,
        transparent_titlebar_, force_transcoding_,
    };

    std::thread([this, path = std::move(path), snap = std::move(snap)]() {
        std::lock_guard<std::mutex> lock(save_mutex_);
        JfnConfigData d{};
        jfn_config_init_defaults(&d);
        d.server_url = const_cast<char*>(snap.server_url.c_str());
        d.hwdec = const_cast<char*>(snap.hwdec.c_str());
        d.audio_passthrough = const_cast<char*>(snap.audio_passthrough.c_str());
        d.audio_channels = const_cast<char*>(snap.audio_channels.c_str());
        d.log_level = const_cast<char*>(snap.log_level.c_str());
        d.device_name = const_cast<char*>(snap.device_name.c_str());
        d.window_x = snap.geom.x;
        d.window_y = snap.geom.y;
        d.window_width = snap.geom.width;
        d.window_height = snap.geom.height;
        d.window_logical_width = snap.geom.logical_width;
        d.window_logical_height = snap.geom.logical_height;
        d.window_scale = snap.geom.scale;
        d.window_maximized = snap.geom.maximized;
        d.audio_exclusive = snap.audio_exclusive;
        d.disable_gpu_compositing = snap.disable_gpu_compositing;
        d.titlebar_theme_color = snap.titlebar_theme_color;
        d.transparent_titlebar = snap.transparent_titlebar;
        d.force_transcoding = snap.force_transcoding;
        jfn_config_save(path.c_str(), &d, kHwdecDefault);
    }).detach();
}

std::string Settings::cliSettingsJson() const {
    JfnConfigData d = toData(*this);
    std::string platform_default = platformDeviceName();

    auto opts = hwdecOptions();
    std::vector<const char*> opt_ptrs;
    opt_ptrs.reserve(opts.size());
    for (const auto& o : opts) opt_ptrs.push_back(o.c_str());

    char* json = jfn_config_cli_json(&d, platform_default.c_str(),
                                     opt_ptrs.empty() ? nullptr : opt_ptrs.data(),
                                     opt_ptrs.size());
    std::string result = takeString(json);
    jfn_config_free_string(json);
    return result;
}

#ifdef __APPLE__
std::string macosComputerName();  // src/platform/macos.mm
#endif

std::string Settings::platformDeviceName() {
#ifdef _WIN32
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD len = sizeof(buf) / sizeof(buf[0]);
    GetComputerNameW(buf, &len);
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buf, len, nullptr, 0, nullptr, nullptr);
    std::string out(utf8_len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, len, out.data(), utf8_len, nullptr, nullptr);
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

void Settings::setDeviceName(const std::string& v) {
    // Server's auth header parser preserves whitespace verbatim, so " foo "
    // would round-trip into the Devices table.
    std::string trimmed;
    trimmed.reserve(v.size());
    bool in_space = true;
    for (char c : v) {
        bool ws = c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
        if (ws) {
            if (!in_space) trimmed.push_back(' ');
            in_space = true;
        } else {
            trimmed.push_back(c);
            in_space = false;
        }
    }
    if (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    if (trimmed.size() > kDeviceNameMax) trimmed.resize(kDeviceNameMax);
    // Don't persist the platform default — leave the override empty so
    // hostname changes propagate automatically on the next launch.
    if (trimmed == platformDeviceName()) trimmed.clear();
    device_name_ = std::move(trimmed);
}

std::string Settings::effectiveDeviceName() const {
    return device_name_.empty() ? platformDeviceName() : device_name_;
}
