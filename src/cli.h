#pragma once

#include <optional>
#include <string>

namespace cli {

struct Args {
    std::string hwdec;
    std::string audio_passthrough;
    bool audio_exclusive = false;
    std::string audio_channels;
    bool disable_gpu_compositing = false;
    std::string ozone_platform;
    std::string platform_override;
    int remote_debugging_port = 0;
    std::string log_level;
    // unset = use platform default; set (even to empty) = explicit override
    std::optional<std::string> log_file;
};

struct Result {
    enum class Kind { Continue, Help, Version, Error };
    Kind kind = Kind::Continue;
    std::string unknown_arg;  // populated for Error
};

// Pre-populate `args` with settings-derived defaults before calling; CLI
// flags override matching fields. Settings/CLI separation lives at the call
// site, not in this module.
Result parse(int argc, char* argv[], Args& args);

void print_help();
void print_version();

} // namespace cli
