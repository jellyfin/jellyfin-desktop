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
        int x = -1;          // -1 = not set (use default centering)
        int y = -1;
        int width = 0;       // 0 = not set (use default 1280x720)
        int height = 0;
        bool maximized = false;
    };

    const WindowGeometry& windowGeometry() const { return window_geometry_; }
    void setWindowGeometry(const WindowGeometry& geom) { window_geometry_ = geom; }

private:
    Settings() = default;
    std::string getConfigPath();

    std::string server_url_;
    WindowGeometry window_geometry_;
    std::mutex save_mutex_;  // Prevent concurrent saves
};
