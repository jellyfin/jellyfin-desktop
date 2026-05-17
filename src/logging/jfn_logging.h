#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LogCategory and LogLevel values must match the C++ enums in src/logging.h.
//   Category: 0=Main 1=mpv 2=CEF 3=Media 4=Platform 5=JS 6=Resource
//   Level:    0=Trace 1=Debug 2=Info 3=Warn 4=Error

// Initialize the logging subsystem. `path` is the file log path, or NULL/""
// to disable file logging. `min_level` is the minimum level emitted.
// Idempotent on a second call.
void jfn_log_init(const char* path, uint8_t min_level);

// Flush + drop sinks. Restores stderr if capture was active.
void jfn_log_shutdown(void);

// Returns true if a message at `level` in `category` would be emitted. Used
// by the LOG_* macros to skip formatting when filtered out.
bool jfn_log_enabled(uint8_t category, uint8_t level);

// Emit one log record. `msg` is `len` bytes of UTF-8 (no trailing newline).
void jfn_log(uint8_t category, uint8_t level, const char* msg, size_t len);

// Returns the active file log path (heap-allocated; caller frees with
// jfn_log_free_string). Empty string when file logging is disabled.
char* jfn_log_active_path(void);

void jfn_log_free_string(char* s);

#ifdef __cplusplus
}
#endif
