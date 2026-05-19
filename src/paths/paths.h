#pragma once

#include <string>

#include "paths/jfn_paths.h"

// Per-user filesystem locations. All directory getters create the directory
// (and parents) if missing. Thin C++ wrapper over the Rust jfn-paths FFI.
namespace paths {

namespace detail {
inline std::string take(char* (*fn)()) {
    char* p = fn();
    if (!p) return {};
    std::string out(p);
    jfn_paths_free(p);
    return out;
}
}  // namespace detail

inline std::string getConfigDir() { return detail::take(jfn_paths_config_dir); }
inline std::string getCacheDir()  { return detail::take(jfn_paths_cache_dir); }
inline std::string getLogDir()    { return detail::take(jfn_paths_log_dir); }
inline std::string getLogPath()   { return detail::take(jfn_paths_log_path); }
inline std::string getMpvHome()   { return detail::take(jfn_paths_mpv_home); }

inline void openMpvHome() { jfn_paths_open_mpv_home(); }

}  // namespace paths
