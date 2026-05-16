#include "cli.h"

#include "version.h"
#include "logging.h"
#include "mpv/options.h"

#include <mpv/client.h>
#include <include/cef_version.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

namespace {

const char* match_value(int argc, char* argv[], int* i, const char* name) {
    size_t nlen = strlen(name);
    const char* a = argv[*i];
    if (strcmp(a, name) == 0 && *i + 1 < argc) {
        ++*i;
        return argv[*i];
    }
    if (strncmp(a, name, nlen) == 0 && a[nlen] == '=')
        return a + nlen + 1;
    return nullptr;
}

} // namespace

Result parse(int argc, char* argv[], Args& args) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
            return {Result::Kind::Help, {}};
        if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0)
            return {Result::Kind::Version, {}};
        if (strcmp(a, "--audio-exclusive") == 0) {
            args.audio_exclusive = true;
            continue;
        }
        if (strcmp(a, "--disable-gpu-compositing") == 0) {
            args.disable_gpu_compositing = true;
            continue;
        }
        if (const char* v = match_value(argc, argv, &i, "--log-level")) {
            args.log_level = v;
            continue;
        }
        if (const char* v = match_value(argc, argv, &i, "--log-file")) {
            args.log_file = v;
            continue;
        }
        if (const char* v = match_value(argc, argv, &i, "--hwdec")) {
            args.hwdec = v;
            continue;
        }
        if (const char* v = match_value(argc, argv, &i, "--audio-passthrough")) {
            args.audio_passthrough = v;
            continue;
        }
        if (const char* v = match_value(argc, argv, &i, "--audio-channels")) {
            args.audio_channels = v;
            continue;
        }
        if (const char* v = match_value(argc, argv, &i, "--remote-debug-port")) {
            args.remote_debugging_port = atoi(v);
            continue;
        }
        if (const char* v = match_value(argc, argv, &i, "--ozone-platform")) {
            args.ozone_platform = v;
            continue;
        }
        if (const char* v = match_value(argc, argv, &i, "--platform")) {
            args.platform_override = v;
            continue;
        }
        return {Result::Kind::Error, a};
    }
    return {Result::Kind::Continue, {}};
}

} // namespace cli
