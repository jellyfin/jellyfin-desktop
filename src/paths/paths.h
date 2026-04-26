#pragma once

#include <string>
#include <string_view>

// Per-user filesystem locations. All directory getters create the directory
// (and parents) if missing.
namespace paths {

std::string getConfigDir();  // e.g. ~/.config/jellyfin-desktop
std::string getCacheDir();   // e.g. ~/.cache/jellyfin-desktop
std::string getLogDir();     // e.g. ~/.local/state/jellyfin-desktop
std::string getLogPath();    // default log file inside getLogDir()
std::string getMpvHome();    // MPV_HOME, inside getConfigDir()

// Shared helpers (used by per-platform impls and by paths.cpp).
inline constexpr char kAppDirName[] = "jellyfin-desktop";
inline constexpr char kLogFileName[] = "jellyfin-desktop.log";

void ensureDir(const std::string& path);
void openMpvHome();

// Returns std::getenv(var) if set and non-empty, else fallback.
std::string envOr(const char* var, std::string_view fallback);

}  // namespace paths
