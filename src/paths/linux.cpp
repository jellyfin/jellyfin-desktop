#include "paths/paths.h"


namespace paths {

// XDG Base Directory: config under XDG_CONFIG_HOME, cache under XDG_CACHE_HOME,
// logs under XDG_STATE_HOME. Each falls back to the conventional $HOME subdir.
static std::string xdgOrHome(const char* xdg_var, const char* home_subdir) {
    std::string home = envOr("HOME", "/tmp");
    return envOr(xdg_var, home + home_subdir);
}

std::string getConfigDir() {
    std::string dir = xdgOrHome("XDG_CONFIG_HOME", "/.config") + "/" + kAppDirName;
    ensureDir(dir);
    return dir;
}

std::string getCacheDir() {
    std::string dir = xdgOrHome("XDG_CACHE_HOME", "/.cache") + "/" + kAppDirName;
    ensureDir(dir);
    return dir;
}

std::string getLogDir() {
    std::string dir = xdgOrHome("XDG_STATE_HOME", "/.local/state") + "/" + kAppDirName;
    ensureDir(dir);
    return dir;
}

void openMpvHome() {
    std::string command = "xdg-open '" + getMpvHome() + "'";
    system(command.c_str());
}

}  // namespace paths
