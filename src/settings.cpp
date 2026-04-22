#include "settings.h"
#include "cjson/cJSON.h"
#include "mpv/options.h"
#include "paths/paths.h"
#include <fstream>
#include <sstream>
#include <thread>

Settings& Settings::instance() {
    static Settings instance;
    return instance;
}

std::string Settings::getConfigPath() {
    return paths::getConfigDir() + "/settings.json";
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

static double jsonDouble(const cJSON* root, const char* key, double fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) return item->valuedouble;
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
    window_geometry_.logical_width = jsonInt(root, "windowLogicalWidth", 0);
    window_geometry_.logical_height = jsonInt(root, "windowLogicalHeight", 0);
    window_geometry_.scale = static_cast<float>(jsonDouble(root, "windowScale", 0.0));
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
    if (geom.logical_width > 0 && geom.logical_height > 0) {
        cJSON_AddNumberToObject(root, "windowLogicalWidth", geom.logical_width);
        cJSON_AddNumberToObject(root, "windowLogicalHeight", geom.logical_height);
    }
    if (geom.scale > 0.f)
        cJSON_AddNumberToObject(root, "windowScale", geom.scale);
    if (geom.x >= 0 && geom.y >= 0) {
        cJSON_AddNumberToObject(root, "windowX", geom.x);
        cJSON_AddNumberToObject(root, "windowY", geom.y);
    }
    cJSON_AddBoolToObject(root, "windowMaximized", geom.maximized);

    if (!s.hwdec().empty() && s.hwdec() != kHwdecDefault) cJSON_AddStringToObject(root, "hwdec", s.hwdec().c_str());
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

void Settings::ensureSaveWorker() {
    if (worker_started_) return;
    worker_started_ = true;
    save_thread_ = std::thread(&Settings::saveWorkerLoop, this);
}

void Settings::saveWorkerLoop() {
    const std::string path = getConfigPath();
    std::unique_lock<std::mutex> lk(save_mutex_);
    while (true) {
        save_cv_.wait(lk, [this] { return pending_ || stop_; });
        if (pending_) {
            std::string data = std::move(pending_data_);
            pending_ = false;
            lk.unlock();
            std::ofstream file(path);
            if (file.is_open()) {
                file << data << '\n';
            }
            lk.lock();
        }
        if (stop_ && !pending_) return;
    }
}

void Settings::saveAsync() {
    std::string data = buildSettingsJson(*this, true);
    {
        std::lock_guard<std::mutex> lk(save_mutex_);
        ensureSaveWorker();
        pending_data_ = std::move(data);
        pending_ = true;
    }
    save_cv_.notify_one();
}

void Settings::shutdownSaveWorker() {
    {
        std::lock_guard<std::mutex> lk(save_mutex_);
        if (!worker_started_) return;
        stop_ = true;
    }
    save_cv_.notify_one();
    if (save_thread_.joinable()) save_thread_.join();
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

    cJSON* opts = cJSON_AddArrayToObject(root, "hwdecOptions");
    for (const auto& o : hwdecOptions())
        cJSON_AddItemToArray(opts, cJSON_CreateString(o.c_str()));

    char* str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
