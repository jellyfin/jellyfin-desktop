#pragma once

#include "logging/jfn_logging.h"
#include "include/internal/cef_types.h"

#include <cstdint>
#include <string>

#include <fmt/format.h>

enum LogCategory : uint8_t {
    LOG_MAIN     = 0,
    LOG_MPV      = 1,
    LOG_CEF      = 2,
    LOG_MEDIA    = 3,
    LOG_PLATFORM = 4,
    LOG_JS       = 5,
    LOG_RESOURCE = 6,
    LOG_CATEGORY_COUNT,
};

enum class LogLevel : uint8_t {
    Trace   = 0,
    Debug   = 1,
    Info    = 2,
    Warn    = 3,
    Error   = 4,
    Default = 255,
};

inline constexpr LogLevel kDefaultLogLevel = LogLevel::Info;
inline constexpr const char* kDefaultLogFilter = "info";

// Most-verbose level still surfaced by the active filter for `cat`.
// Used to derive integration-level subscription thresholds (mpv log
// subscription, CEF log severity) from the unified filter directive.
inline LogLevel effectiveLogLevel(LogCategory cat) {
    if (jfn_log_enabled(cat, static_cast<uint8_t>(LogLevel::Trace))) return LogLevel::Trace;
    if (jfn_log_enabled(cat, static_cast<uint8_t>(LogLevel::Debug))) return LogLevel::Debug;
    if (jfn_log_enabled(cat, static_cast<uint8_t>(LogLevel::Info)))  return LogLevel::Info;
    if (jfn_log_enabled(cat, static_cast<uint8_t>(LogLevel::Warn)))  return LogLevel::Warn;
    return LogLevel::Error;
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

namespace logging_internal {

inline void emit(LogCategory cat, LogLevel level, fmt::string_view fmt_str,
                 fmt::format_args args) {
    if (!jfn_log_enabled(static_cast<uint8_t>(cat),
                         static_cast<uint8_t>(level))) return;
    fmt::memory_buffer buf;
    fmt::vformat_to(std::back_inserter(buf), fmt_str, args);
    jfn_log(static_cast<uint8_t>(cat), static_cast<uint8_t>(level),
            buf.data(), buf.size());
}

template <typename... Args>
inline void emit_args(LogCategory cat, LogLevel level, fmt::format_string<Args...> fmt_str,
                      Args&&... args) {
    emit(cat, level, fmt_str, fmt::make_format_args(args...));
}

}  // namespace logging_internal

#define LOG_ERROR(cat, ...) ::logging_internal::emit_args((cat), LogLevel::Error, __VA_ARGS__)
#define LOG_WARN(cat, ...)  ::logging_internal::emit_args((cat), LogLevel::Warn,  __VA_ARGS__)
#define LOG_INFO(cat, ...)  ::logging_internal::emit_args((cat), LogLevel::Info,  __VA_ARGS__)
#define LOG_DEBUG(cat, ...) ::logging_internal::emit_args((cat), LogLevel::Debug, __VA_ARGS__)
#define LOG_TRACE(cat, ...) ::logging_internal::emit_args((cat), LogLevel::Trace, __VA_ARGS__)

class LoggingScope {
public:
    LoggingScope(const char* path, const char* filter) {
        jfn_log_init(path, (filter && *filter) ? filter : kDefaultLogFilter);
    }
    ~LoggingScope() { jfn_log_shutdown(); }
};

inline std::string activeLogPath() {
    char* p = jfn_log_active_path();
    if (!p) return {};
    std::string s(p);
    jfn_log_free_string(p);
    return s;
}
