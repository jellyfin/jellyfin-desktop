#ifndef LOGGING_H
#define LOGGING_H

// Linux rewrite: no SDL dependency
#if defined(__linux__) && !defined(JELLYFIN_USE_SDL_LOGGING)
#include "logging_linux.h"
#else

#include <SDL3/SDL_log.h>
#include <cstring>
#include <string>

// Custom log categories (SDL_LOG_CATEGORY_CUSTOM = 19)
enum LogCategory {
    LOG_MAIN       = SDL_LOG_CATEGORY_APPLICATION,
    LOG_MPV        = SDL_LOG_CATEGORY_CUSTOM,
    LOG_CEF        = SDL_LOG_CATEGORY_CUSTOM + 1,
    LOG_GL         = SDL_LOG_CATEGORY_CUSTOM + 2,
    LOG_MEDIA      = SDL_LOG_CATEGORY_CUSTOM + 3,
    LOG_OVERLAY    = SDL_LOG_CATEGORY_CUSTOM + 4,
    LOG_MENU       = SDL_LOG_CATEGORY_CUSTOM + 5,
    LOG_UI         = SDL_LOG_CATEGORY_CUSTOM + 6,
    LOG_WINDOW     = SDL_LOG_CATEGORY_CUSTOM + 7,
    LOG_PLATFORM   = SDL_LOG_CATEGORY_CUSTOM + 8,
    LOG_COMPOSITOR = SDL_LOG_CATEGORY_CUSTOM + 9,
    LOG_RESOURCE   = SDL_LOG_CATEGORY_CUSTOM + 10,
    LOG_TEST       = SDL_LOG_CATEGORY_CUSTOM + 11,
    LOG_JS_MAIN    = SDL_LOG_CATEGORY_CUSTOM + 12,
    LOG_JS_OVERLAY = SDL_LOG_CATEGORY_CUSTOM + 13,
    LOG_VIDEO      = SDL_LOG_CATEGORY_CUSTOM + 14,
};

// Last custom category (for iteration)
constexpr int LOG_CATEGORY_LAST = LOG_VIDEO;

// Convenience macros - printf-style
#define LOG_ERROR(cat, ...)   SDL_LogError(cat, __VA_ARGS__)
#define LOG_WARN(cat, ...)    SDL_LogWarn(cat, __VA_ARGS__)
#define LOG_INFO(cat, ...)    SDL_LogInfo(cat, __VA_ARGS__)
#define LOG_DEBUG(cat, ...)   SDL_LogDebug(cat, __VA_ARGS__)
#define LOG_VERBOSE(cat, ...) SDL_LogVerbose(cat, __VA_ARGS__)

// Category tag lookup
inline const char* getCategoryTag(int category) {
    switch (category) {
        case LOG_MAIN:       return "[Main] ";
        case LOG_MPV:        return "[mpv] ";
        case LOG_CEF:        return "[CEF] ";
        case LOG_GL:         return "[GL] ";
        case LOG_MEDIA:      return "[Media] ";
        case LOG_OVERLAY:    return "[Overlay] ";
        case LOG_MENU:       return "[Menu] ";
        case LOG_UI:         return "[UI] ";
        case LOG_WINDOW:     return "[Window] ";
        case LOG_PLATFORM:   return "[Platform] ";
        case LOG_COMPOSITOR: return "[Compositor] ";
        case LOG_RESOURCE:   return "[Resource] ";
        case LOG_TEST:       return "[Test] ";
        case LOG_JS_MAIN:    return "[JS:Main] ";
        case LOG_JS_OVERLAY: return "[JS:Overlay] ";
        case LOG_VIDEO:      return "[Video] ";
        default:             return "";
    }
}

// Original stderr fd (set by initStderrCapture, used by log callback)
extern int g_original_stderr_fd;

// Log file handle (nullptr = stderr only, set before initLogging())
extern FILE* g_log_file;

// Get log level string from SDL priority
inline const char* getLogLevelStr(SDL_LogPriority priority) {
    switch (priority) {
        case SDL_LOG_PRIORITY_VERBOSE: return "VERBOSE";
        case SDL_LOG_PRIORITY_DEBUG:   return "DEBUG";
        case SDL_LOG_PRIORITY_INFO:    return "INFO";
        case SDL_LOG_PRIORITY_WARN:    return "WARN";
        case SDL_LOG_PRIORITY_ERROR:   return "ERROR";
        case SDL_LOG_PRIORITY_CRITICAL:return "CRITICAL";
        default:                       return "?";
    }
}

// Write a log line to file (with timestamp+level) and stderr (without)
void writeLogLine(const char* tag, const char* message, const char* level = nullptr);

// Custom log callback that prepends category tags
inline void SDLCALL logCallback(void* /*userdata*/, int category,
                                 SDL_LogPriority priority, const char* message) {
    const char* tag = getCategoryTag(category);
    const char* level = getLogLevelStr(priority);

    // Replace newlines with spaces
    std::string sanitized(message);
    for (char& c : sanitized) {
        if (c == '\n' || c == '\r') c = ' ';
    }

    writeLogLine(tag, sanitized.c_str(), level);
}

// Stderr capture for CEF/Chromium logs (call before CefInitialize)
void initStderrCapture();
void shutdownStderrCapture();

// Close log file if open
void shutdownLogging();

// Parse log level string to SDL priority, returns -1 on invalid
inline int parseLogLevel(const char* level) {
    if (strcmp(level, "verbose") == 0) return SDL_LOG_PRIORITY_VERBOSE;
    if (strcmp(level, "debug") == 0)   return SDL_LOG_PRIORITY_DEBUG;
    if (strcmp(level, "info") == 0)    return SDL_LOG_PRIORITY_INFO;
    if (strcmp(level, "warn") == 0)    return SDL_LOG_PRIORITY_WARN;
    if (strcmp(level, "error") == 0)   return SDL_LOG_PRIORITY_ERROR;
    return -1;
}

// Initialize logging (call once from main after SDL_Init)
inline void initLogging(SDL_LogPriority priority = SDL_LOG_PRIORITY_INFO) {
    // Set priority for all categories
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, priority);
    for (int i = SDL_LOG_CATEGORY_CUSTOM; i <= LOG_CATEGORY_LAST; i++) {
        SDL_SetLogPriority(i, priority);
    }

    // Install custom callback for tagged output
    SDL_SetLogOutputFunction(logCallback, nullptr);
}

#endif // !__linux__
#endif // LOGGING_H
