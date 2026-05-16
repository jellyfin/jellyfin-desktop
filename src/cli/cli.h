#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JFN_CLI_CONTINUE = 0,
    JFN_CLI_HELP = 1,
    JFN_CLI_VERSION = 2,
    JFN_CLI_ERROR = 3,
} JfnCliResultKind;

// Flat result of parsing argv. String fields are owned by the Rust side and
// freed by jfn_cli_result_free(). NULL = flag absent. `*_set` flags
// disambiguate boolean toggles. `remote_debugging_port` uses sentinel -1
// for "unset". `unknown_arg` is only populated on JFN_CLI_ERROR.
typedef struct {
    JfnCliResultKind kind;
    char* unknown_arg;

    char* hwdec;
    char* audio_passthrough;
    char* audio_channels;
    char* log_level;
    char* log_file;
    char* ozone_platform;
    char* platform_override;

    bool log_file_set;
    bool audio_exclusive_set;
    bool disable_gpu_compositing_set;

    int32_t remote_debugging_port;
} JfnCliResult;

// Parse argv. `have_x11` toggles acceptance of `--platform`; when false, the
// flag is rejected as unknown to match the C++ behavior under `#ifdef
// HAVE_X11`. Returns a heap-allocated result the caller frees with
// jfn_cli_result_free().
JfnCliResult* jfn_cli_parse(int argc, const char* const* argv, bool have_x11);

void jfn_cli_result_free(JfnCliResult* r);

#ifdef __cplusplus
}
#endif
