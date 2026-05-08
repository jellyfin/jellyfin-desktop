#pragma once

#include <cstring>
#include <string>

#include "quill/Logger.h"
#include "quill/LogMacros.h"
#include "include/internal/cef_types.h"

enum LogCategory {
    LOG_MAIN,
    LOG_MPV,
    LOG_CEF,
    LOG_MEDIA,
    LOG_PLATFORM,
    LOG_JS,
    LOG_RESOURCE,
    LOG_CATEGORY_COUNT,
};

extern quill::Logger* g_loggers[LOG_CATEGORY_COUNT];

#define LOG_ERROR(cat, ...)   QUILL_LOG_ERROR(g_loggers[cat],   __VA_ARGS__)
#define LOG_WARN(cat, ...)    QUILL_LOG_WARNING(g_loggers[cat], __VA_ARGS__)
#define LOG_INFO(cat, ...)    QUILL_LOG_INFO(g_loggers[cat],    __VA_ARGS__)
#define LOG_DEBUG(cat, ...)   QUILL_LOG_DEBUG(g_loggers[cat],   __VA_ARGS__)
#define LOG_TRACE(cat, ...)   QUILL_LOG_TRACE_L1(g_loggers[cat], __VA_ARGS__)

enum class LogLevel {
    Default,
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

inline constexpr LogLevel kDefaultLogLevel = LogLevel::Info;
inline constexpr const char* kDefaultLogLevelName = "info";

// Returns LogLevel::Default for unknown strings.
inline LogLevel parseLogLevel(const char* level) {
    if (strcmp(level, "trace") == 0) return LogLevel::Trace;
    if (strcmp(level, "debug") == 0) return LogLevel::Debug;
    if (strcmp(level, "info")  == 0) return LogLevel::Info;
    if (strcmp(level, "warn")  == 0) return LogLevel::Warn;
    if (strcmp(level, "error") == 0) return LogLevel::Error;
    return LogLevel::Default;
}

inline cef_log_severity_t toCefSeverity(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
        case LogLevel::Debug: return LOGSEVERITY_VERBOSE;
        case LogLevel::Info:  return LOGSEVERITY_INFO;
        case LogLevel::Warn:  return LOGSEVERITY_WARNING;
        case LogLevel::Error: return LOGSEVERITY_ERROR;
        case LogLevel::Default: break;
    }
    return LOGSEVERITY_DEFAULT;
}

// Install the log file at `path` (rotated on startup + at 10 MB, 3 backups)
// and redirect this process's stderr through the logger so writes from
// CEF/Chromium (and subprocesses that inherit our stderr) land in the log.
// Empty/null path disables file logging; stderr is still captured.
// Call once before spawning any subprocess.
void initLogging(const char* path, LogLevel min_level);

// Flush and stop the backend, drain the stderr capture, close files.
void shutdownLogging();

// Path of the active log file, or empty string when file logging is disabled.
const std::string& activeLogPath();
