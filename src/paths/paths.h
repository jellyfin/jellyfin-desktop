#pragma once

#include <string>

// Per-user filesystem locations. All directory getters create the directory
// (and parents) if missing.
namespace paths {

std::string getConfigDir();  // e.g. ~/.config/jellyfin-desktop
std::string getCacheDir();   // e.g. ~/.cache/jellyfin-desktop
std::string getLogDir();     // e.g. ~/.local/state/jellyfin-desktop
std::string getLogPath();    // default log file inside getLogDir()
std::string getMpvHome();    // MPV_HOME, inside getConfigDir()

void openMpvHome();

}  // namespace paths
