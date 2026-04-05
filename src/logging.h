#pragma once

// Standalone logging — no external dependencies.
// Uses fprintf to stderr with optional file output.

#include <cstdio>
#include <cstring>

// Log categories (values don't matter -- just used as tags)
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
    LOG_JS_MAIN    = 13,
    LOG_JS_OVERLAY = 14,
    LOG_VIDEO      = 15,
};

inline const char* getCategoryTag(int category) {
    switch (category) {
        case LOG_MAIN:       return "[Main]";
        case LOG_MPV:        return "[mpv]";
        case LOG_CEF:        return "[CEF]";
        case LOG_GL:         return "[GL]";
        case LOG_MEDIA:      return "[Media]";
        case LOG_OVERLAY:    return "[Overlay]";
        case LOG_MENU:       return "[Menu]";
        case LOG_UI:         return "[UI]";
        case LOG_WINDOW:     return "[Window]";
        case LOG_PLATFORM:   return "[Platform]";
        case LOG_COMPOSITOR: return "[Compositor]";
        case LOG_RESOURCE:   return "[Resource]";
        case LOG_TEST:       return "[Test]";
        case LOG_JS_MAIN:    return "[JS:Main]";
        case LOG_JS_OVERLAY: return "[JS:Overlay]";
        case LOG_VIDEO:      return "[Video]";
        default:             return "";
    }
}

// Log file handle (nullptr = stderr only)
inline FILE* g_log_file = nullptr;

#define LOG_ERROR(cat, fmt, ...)   do { fprintf(stderr, "%s ERROR: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "%s ERROR: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); fflush(g_log_file); } } while(0)
#define LOG_WARN(cat, fmt, ...)    do { fprintf(stderr, "%s WARN: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "%s WARN: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); fflush(g_log_file); } } while(0)
#define LOG_INFO(cat, fmt, ...)    do { fprintf(stderr, "%s INFO: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "%s INFO: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); fflush(g_log_file); } } while(0)
#define LOG_DEBUG(cat, fmt, ...)   do { fprintf(stderr, "%s DEBUG: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "%s DEBUG: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); fflush(g_log_file); } } while(0)
#define LOG_VERBOSE(cat, fmt, ...) do { fprintf(stderr, "%s VERBOSE: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); if (g_log_file) { fprintf(g_log_file, "%s VERBOSE: " fmt "\n", getCategoryTag(cat), ##__VA_ARGS__); fflush(g_log_file); } } while(0)

inline int parseLogLevel(const char* level) {
    if (strcmp(level, "verbose") == 0) return 0;
    if (strcmp(level, "debug") == 0)   return 1;
    if (strcmp(level, "info") == 0)    return 2;
    if (strcmp(level, "warn") == 0)    return 3;
    if (strcmp(level, "error") == 0)   return 4;
    return -1;
}

inline void initLogging(int /*level*/ = 1) {
    // Future: filter by level. For now, log everything.
}
