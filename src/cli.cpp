#include "cli.h"

#include "version.h"
#include "logging.h"
#include "mpv/options.h"
#include "cli/cli.h"

#include <mpv/client.h>
#include <include/cef_version.h>

#include <cstdio>

namespace cli {

void print_help() {
    printf("Usage: jellyfin-desktop [options]\n"
           "\nOptions:\n"
           "  -h, --help                Show this help\n"
           "  -v, --version             Show version\n"
           "  --log-level <level>       trace|debug|info|warn|error (default: %s)\n"
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
           kDefaultLogLevelName, kHwdecDefault);
}

void print_version() {
    printf("jellyfin-desktop %s\n\nCEF %s\n\n", APP_VERSION_FULL, CEF_VERSION);
    mpv_handle* h = mpv_create();
    if (h && mpv_initialize(h) >= 0) {
        for (const char* prop : {"mpv-version", "ffmpeg-version"}) {
            char* v = mpv_get_property_string(h, prop);
            if (v) {
                printf("%s %s\n", prop, v);
                mpv_free(v);
            }
        }
    }
    if (h) mpv_terminate_destroy(h);
}

Result parse(int argc, char* argv[], Args& args) {
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
