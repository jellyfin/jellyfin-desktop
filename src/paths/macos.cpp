#include "paths/paths.h"


namespace paths {

// Config stays XDG-style (~/.config) to match existing installs on macOS.
// Cache and logs use native macOS conventions (Console.app picks up Library/Logs).

std::string getConfigDir() {
    std::string home = envOr("HOME", "/tmp");
    std::string dir = envOr("XDG_CONFIG_HOME", home + "/.config") + "/" + kAppDirName;
    ensureDir(dir);
    return dir;
}

std::string getCacheDir() {
    std::string dir = envOr("HOME", "/tmp") + "/Library/Caches/" + kAppDirName;
    ensureDir(dir);
    return dir;
}

std::string getLogDir() {
    std::string dir = envOr("HOME", "/tmp") + "/Library/Logs/" + kAppDirName;
    ensureDir(dir);
    return dir;
}

void openMpvHome() {
    std::string command = "open '" + getMpvHome() + "'";
    system(command.c_str());
}

}  // namespace paths
