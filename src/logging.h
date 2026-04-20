#pragma once

#include <cstring>
#include <string>

#include "quill/Logger.h"
#include "quill/LogMacros.h"
#include "include/internal/cef_types.h"

enum LogCategory {
    LOG_MAIN       = 0,
    LOG_MPV        = 1,
    LOG_CEF        = 2,
    LOG_GL         = 3,
    LOG_MEDIA      = 4,
    LOG_OVERLAY    = 5,
    LOG_MENU       = 6,
    LOG_UI         = 7,
    LOG_WINDOW     = 8,
    LOG_PLATFORM   = 9,
    LOG_COMPOSITOR = 10,
    LOG_RESOURCE   = 11,
    LOG_TEST       = 12,
    LOG_JS         = 13,
    LOG_VIDEO      = 14,
    LOG_CATEGORY_COUNT,
};

extern quill::Logger* g_loggers[LOG_CATEGORY_COUNT];

#define LOG_ERROR(cat, ...)   QUILL_LOG_ERROR(g_loggers[cat],   __VA_ARGS__)
#define LOG_WARN(cat, ...)    QUILL_LOG_WARNING(g_loggers[cat], __VA_ARGS__)
#define LOG_INFO(cat, ...)    QUILL_LOG_INFO(g_loggers[cat],    __VA_ARGS__)
#define LOG_DEBUG(cat, ...)   QUILL_LOG_DEBUG(g_loggers[cat],   __VA_ARGS__)
#define LOG_TRACE(cat, ...)   QUILL_LOG_TRACE_L1(g_loggers[cat], __VA_ARGS__)

inline int parseLogLevel(const char* level) {
    if (strcmp(level, "trace") == 0) return 0;
    if (strcmp(level, "debug") == 0)   return 1;
    if (strcmp(level, "info") == 0)    return 2;
    if (strcmp(level, "warn") == 0)    return 3;
    if (strcmp(level, "error") == 0)   return 4;
    return -1;
}

// Map our 0..4 log level (see parseLogLevel) to CEF's severity enum.
inline cef_log_severity_t toCefSeverity(int parsed) {
    switch (parsed) {
        case 0: case 1: return LOGSEVERITY_VERBOSE;
        case 2:         return LOGSEVERITY_INFO;
        case 3:         return LOGSEVERITY_WARNING;
        case 4:         return LOGSEVERITY_ERROR;
        default:        return LOGSEVERITY_DEFAULT;
    }
}

// Install the log file at `path` (rotated on startup + at 10 MB, 3 backups)
// and redirect this process's stderr through the logger so writes from
// CEF/Chromium (and subprocesses that inherit our stderr) land in the log.
// Empty/null path disables file logging; stderr is still captured.
// `min_level` is the value returned by parseLogLevel: 0=verbose .. 4=error;
// pass -1 to keep the default (everything).
// Call once before spawning any subprocess.
void initLogging(const char* path, int min_level);

// Flush and stop the backend, drain the stderr capture, close files.
void shutdownLogging();

// Path of the active log file, or empty string when file logging is disabled.
const std::string& activeLogPath();
