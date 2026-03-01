#include "settings.h"
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
    config_dir += "\\jellyfin-desktop-cef";
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
    config_dir += "/jellyfin-desktop-cef";
#endif

    MKDIR(config_dir.c_str());

    return config_dir + "/settings.json";
}

bool Settings::load() {
    std::ifstream file(getConfigPath());
    if (!file.is_open()) {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Simple JSON parsing helpers
    auto parseString = [&](const char* key) -> std::string {
        size_t pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return {};
        pos = content.find(':', pos);
        if (pos == std::string::npos) return {};
        pos = content.find('"', pos);
        if (pos == std::string::npos) return {};
        size_t end = content.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return content.substr(pos + 1, end - pos - 1);
    };

    auto parseInt = [&](const char* key, int fallback) -> int {
        size_t pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return fallback;
        pos = content.find(':', pos);
        if (pos == std::string::npos) return fallback;
        // Skip whitespace after colon
        pos++;
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t'))
            pos++;
        try { return std::stoi(content.substr(pos)); }
        catch (...) { return fallback; }
    };

    auto parseBool = [&](const char* key, bool fallback) -> bool {
        size_t pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return fallback;
        pos = content.find(':', pos);
        if (pos == std::string::npos) return fallback;
        size_t end = content.find_first_of(",}\n", pos);
        if (end == std::string::npos) end = content.size();
        return content.substr(pos + 1, end - pos - 1).find("true") != std::string::npos;
    };

    server_url_ = parseString("serverUrl");

    window_geometry_.width = parseInt("windowWidth", 0);
    window_geometry_.height = parseInt("windowHeight", 0);
    window_geometry_.x = parseInt("windowX", -1);
    window_geometry_.y = parseInt("windowY", -1);
    window_geometry_.maximized = parseBool("windowMaximized", false);

    return true;
}

bool Settings::save() {
    std::ofstream file(getConfigPath());
    if (!file.is_open()) {
        return false;
    }

    writeJson(file, server_url_, window_geometry_);

    return true;
}

void Settings::saveAsync() {
    // Capture current state and save in background
    std::string url = server_url_;
    std::string path = getConfigPath();
    WindowGeometry geom = window_geometry_;

    std::thread([this, url, path, geom]() {
        std::lock_guard<std::mutex> lock(save_mutex_);
        std::ofstream file(path);
        if (file.is_open()) {
            writeJson(file, url, geom);
        }
    }).detach();
}

void Settings::writeJson(std::ofstream& file, const std::string& url,
                          const WindowGeometry& geom) {
    file << "{\n";
    file << "  \"serverUrl\": \"" << url << "\"";
    if (geom.width > 0 && geom.height > 0) {
        file << ",\n  \"windowWidth\": " << geom.width;
        file << ",\n  \"windowHeight\": " << geom.height;
    }
    if (geom.x >= 0 && geom.y >= 0) {
        file << ",\n  \"windowX\": " << geom.x;
        file << ",\n  \"windowY\": " << geom.y;
    }
    file << ",\n  \"windowMaximized\": " << (geom.maximized ? "true" : "false");
    file << "\n}\n";
}
