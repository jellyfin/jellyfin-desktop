#include "settings.h"
#include "cjson/cJSON.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <thread>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

Settings& Settings::instance() {
    static Settings instance;
    return instance;
}

std::string Settings::getConfigPath() {
    std::string config_dir;

#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0]) {
        config_dir = appdata;
    } else {
        config_dir = "C:\\";
    }
    config_dir += "\\jellyfin-desktop";
#else
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && xdg_config[0]) {
        config_dir = xdg_config;
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            config_dir = std::string(home) + "/.config";
        } else {
            config_dir = "/tmp";
        }
    }
    config_dir += "/jellyfin-desktop";
#endif

    MKDIR(config_dir.c_str());

    return config_dir + "/settings.json";
}

static const char* jsonStr(const cJSON* root, const char* key, const char* fallback = "") {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return fallback;
}

static int jsonInt(const cJSON* root, const char* key, int fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    return fallback;
}

static bool jsonBool(const cJSON* root, const char* key, bool fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return fallback;
}

bool Settings::load() {
    std::ifstream file(getConfigPath());
    if (!file.is_open())
        return false;

    std::stringstream buf;
    buf << file.rdbuf();
    std::string contents = buf.str();

    cJSON* root = cJSON_Parse(contents.c_str());
    if (!root)
        return false;

    server_url_ = jsonStr(root, "serverUrl");

    window_geometry_.width = jsonInt(root, "windowWidth", 0);
    window_geometry_.height = jsonInt(root, "windowHeight", 0);
    window_geometry_.x = jsonInt(root, "windowX", -1);
    window_geometry_.y = jsonInt(root, "windowY", -1);
    window_geometry_.maximized = jsonBool(root, "windowMaximized", false);

    hwdec_ = jsonStr(root, "hwdec");
    audio_passthrough_ = jsonStr(root, "audioPassthrough");
    audio_exclusive_ = jsonBool(root, "audioExclusive", false);
    audio_channels_ = jsonStr(root, "audioChannels");
    disable_gpu_compositing_ = jsonBool(root, "disableGpuCompositing", false);
    titlebar_theme_color_ = jsonBool(root, "titlebarThemeColor", true);
    transparent_titlebar_ = jsonBool(root, "transparentTitlebar", true);
    log_level_ = jsonStr(root, "logLevel");

    cJSON_Delete(root);
    return true;
}

static std::string buildSettingsJson(const Settings& s, bool pretty) {
    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "serverUrl", s.serverUrl().c_str());

    auto& geom = s.windowGeometry();
    if (geom.width > 0 && geom.height > 0) {
        cJSON_AddNumberToObject(root, "windowWidth", geom.width);
        cJSON_AddNumberToObject(root, "windowHeight", geom.height);
    }
    if (geom.x >= 0 && geom.y >= 0) {
        cJSON_AddNumberToObject(root, "windowX", geom.x);
        cJSON_AddNumberToObject(root, "windowY", geom.y);
    }
    cJSON_AddBoolToObject(root, "windowMaximized", geom.maximized);

    if (!s.hwdec().empty()) cJSON_AddStringToObject(root, "hwdec", s.hwdec().c_str());
    if (!s.audioPassthrough().empty()) cJSON_AddStringToObject(root, "audioPassthrough", s.audioPassthrough().c_str());
    if (s.audioExclusive()) cJSON_AddBoolToObject(root, "audioExclusive", true);
    if (!s.audioChannels().empty()) cJSON_AddStringToObject(root, "audioChannels", s.audioChannels().c_str());
    if (s.disableGpuCompositing()) cJSON_AddBoolToObject(root, "disableGpuCompositing", true);
    if (!s.titlebarThemeColor()) cJSON_AddBoolToObject(root, "titlebarThemeColor", false);
    if (!s.transparentTitlebar()) cJSON_AddBoolToObject(root, "transparentTitlebar", false);
    if (!s.logLevel().empty()) cJSON_AddStringToObject(root, "logLevel", s.logLevel().c_str());

    char* str = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

bool Settings::save() {
    std::ofstream file(getConfigPath());
    if (!file.is_open())
        return false;

    file << buildSettingsJson(*this, true) << '\n';
    return true;
}

void Settings::saveAsync() {
    std::string path = getConfigPath();
    std::string data = buildSettingsJson(*this, true);

    std::thread([this, path, data]() {
        std::lock_guard<std::mutex> lock(save_mutex_);
        std::ofstream file(path);
        if (file.is_open()) {
            file << data << '\n';
        }
    }).detach();
}

std::string Settings::cliSettingsJson() const {
    cJSON* root = cJSON_CreateObject();

    if (!hwdec_.empty()) cJSON_AddStringToObject(root, "hwdec", hwdec_.c_str());
    if (!audio_passthrough_.empty()) cJSON_AddStringToObject(root, "audioPassthrough", audio_passthrough_.c_str());
    if (audio_exclusive_) cJSON_AddBoolToObject(root, "audioExclusive", true);
    if (!audio_channels_.empty()) cJSON_AddStringToObject(root, "audioChannels", audio_channels_.c_str());
    if (disable_gpu_compositing_) cJSON_AddBoolToObject(root, "disableGpuCompositing", true);
    if (!titlebar_theme_color_) cJSON_AddBoolToObject(root, "titlebarThemeColor", false);
    if (!transparent_titlebar_) cJSON_AddBoolToObject(root, "transparentTitlebar", false);
    if (!log_level_.empty()) cJSON_AddStringToObject(root, "logLevel", log_level_.c_str());

    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
