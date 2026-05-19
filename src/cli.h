#pragma once

#include <cstdio>
#include <optional>
#include <string>

#include "version.h"
#include "logging.h"
#include "mpv/jfn_mpv_boot.h"
#include "cli/cli.h"

#include <include/cef_version.h>

extern "C" void jfn_mpv_print_version_info(void);

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

inline void print_help() {
    printf("Usage: jellyfin-desktop [options]\n"
           "\nOptions:\n"
           "  -h, --help                Show this help\n"
           "  -v, --version             Show version\n"
           "  --log-level <filter>      e.g. info | debug | debug,mpv=trace,CEF=off (default: %s)\n"
           "  --log-file <path>         Write logs to file ('' to disable)\n"
           "  --hwdec <mode>            Hardware decoding mode (default: %s)\n"
           "  --audio-passthrough <codecs>  e.g. ac3,dts-hd,eac3,truehd\n"
           "  --audio-exclusive         Exclusive audio output\n"
           "  --audio-channels <layout> e.g. stereo, 5.1, 7.1\n"
           "  --remote-debug-port <port> Chrome remote debugging\n"
           "  --disable-gpu-compositing Disable CEF GPU compositing\n"
           "  --ozone-platform <plat>   CEF ozone platform (default: follows --platform)\n"
#ifdef HAVE_X11
           "  --platform <wayland|x11>  Force display backend (Linux only)\n"
#endif
           ,
           kDefaultLogFilter, jfn_mpv_hwdec_default());
}

inline void print_version() {
    printf("jellyfin-desktop %s\n\nCEF %s\n\n", APP_VERSION_FULL, CEF_VERSION);
    fflush(stdout);
    jfn_mpv_print_version_info();
}

// Pre-populate `args` with settings-derived defaults before calling; CLI
// flags override matching fields. Settings/CLI separation lives at the call
// site, not in this module.
inline Result parse(int argc, char* argv[], Args& args) {
#ifdef HAVE_X11
    constexpr bool have_x11 = true;
#else
    constexpr bool have_x11 = false;
#endif
    JfnCliResult* r = jfn_cli_parse(argc, const_cast<const char* const*>(argv), have_x11);
    if (!r) return {Result::Kind::Error, ""};

    Result out;
    switch (r->kind) {
    case JFN_CLI_HELP:
        out = {Result::Kind::Help, {}};
        jfn_cli_result_free(r);
        return out;
    case JFN_CLI_VERSION:
        out = {Result::Kind::Version, {}};
        jfn_cli_result_free(r);
        return out;
    case JFN_CLI_ERROR:
        out = {Result::Kind::Error, r->unknown_arg ? r->unknown_arg : ""};
        jfn_cli_result_free(r);
        return out;
    case JFN_CLI_CONTINUE:
        break;
    }

    if (r->hwdec) args.hwdec = r->hwdec;
    if (r->audio_passthrough) args.audio_passthrough = r->audio_passthrough;
    if (r->audio_channels) args.audio_channels = r->audio_channels;
    if (r->log_level) args.log_level = r->log_level;
    if (r->log_file_set) args.log_file = r->log_file ? r->log_file : "";
    if (r->ozone_platform) args.ozone_platform = r->ozone_platform;
    if (r->platform_override) args.platform_override = r->platform_override;
    if (r->audio_exclusive_set) args.audio_exclusive = true;
    if (r->disable_gpu_compositing_set) args.disable_gpu_compositing = true;
    if (r->remote_debugging_port != -1) args.remote_debugging_port = r->remote_debugging_port;

    jfn_cli_result_free(r);
    return {Result::Kind::Continue, {}};
}

} // namespace cli
