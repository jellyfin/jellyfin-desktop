#include "paths/paths.h"

#include "paths/jfn_paths.h"

namespace paths {

namespace {

std::string take(char* (*fn)()) {
    char* p = fn();
    if (!p) return {};
    std::string out(p);
    jfn_paths_free(p);
    return out;
}

}  // namespace

std::string getConfigDir() { return take(jfn_paths_config_dir); }
std::string getCacheDir()  { return take(jfn_paths_cache_dir); }
std::string getLogDir()    { return take(jfn_paths_log_dir); }
std::string getLogPath()   { return take(jfn_paths_log_path); }
std::string getMpvHome()   { return take(jfn_paths_mpv_home); }

void openMpvHome() { jfn_paths_open_mpv_home(); }

}  // namespace paths
