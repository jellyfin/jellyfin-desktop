#include "paths/paths.h"

#include <algorithm>

#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")


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

void openMpvHome() {
    auto path = getMpvHome();   
    std::replace(path.begin(), path.end(), '/', '\\');
    ShellExecuteA(NULL, "explore", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

}  // namespace paths
