#include "paths/paths.h"


namespace paths {

// %LOCALAPPDATA% is the non-roamed user-scoped location (preferred for
// cache/logs). %APPDATA% is the roaming fallback; used for config.

std::string getConfigDir() {
    std::string dir = envOr("APPDATA", "C:") + "/" + kAppDirName;
    ensureDir(dir);
    return dir;
}

std::string getCacheDir() {
    std::string base = envOr("LOCALAPPDATA", envOr("APPDATA", "C:"));
    std::string dir = base + "/" + kAppDirName;
    ensureDir(dir);
    return dir;
}

std::string getLogDir() {
    std::string base = envOr("LOCALAPPDATA", envOr("APPDATA", "C:"));
    std::string dir = base + "/" + kAppDirName + "/Logs";
    ensureDir(dir);
    return dir;
}

}  // namespace paths
