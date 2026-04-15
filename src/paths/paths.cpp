#include "paths/paths.h"


#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace paths {

void ensureDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
}

std::string envOr(const char* var, std::string_view fallback) {
    const char* v = std::getenv(var);
    return (v && v[0]) ? std::string(v) : std::string(fallback);
}

std::string getLogPath() {
    return getLogDir() + "/" + kLogFileName;
}

std::string getMpvHome() {
    std::string dir = getConfigDir() + "/mpv";
    ensureDir(dir);
    return dir;
}

}  // namespace paths
