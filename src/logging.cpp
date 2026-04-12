#include "logging.h"
#include <thread>
#include <atomic>
#include <string>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#define pipe(fds) _pipe(fds, 4096, _O_BINARY)
#define read _read
#define write _write
#define dup _dup
#define dup2 _dup2
#define close _close
#define STDERR_FILENO 2
#else
#include <unistd.h>
#include <poll.h>
#endif

// Global for log callback to use original stderr
int g_original_stderr_fd = -1;

static void writeLogLine(const char* tag, const char* level, const char* message) {
    // Format timestamp as HH:MM:SS.mmm so gaps in the log are easy to spot.
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif
    char ts[16];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));

    // Write to stderr with short timestamp
    if (g_original_stderr_fd >= 0) {
#ifdef _WIN32
        char buf[4096];
        int n = snprintf(buf, sizeof(buf), "%s %s %s: %s\n", ts, tag, level, message);
        if (n > 0) _write(g_original_stderr_fd, buf, static_cast<unsigned>(n));
#else
        dprintf(g_original_stderr_fd, "%s %s %s: %s\n", ts, tag, level, message);
#endif
    } else {
        fprintf(stderr, "%s %s %s: %s\n", ts, tag, level, message);
    }

    // Write to log file with full ISO timestamp
    if (g_log_file) {
        char time_buf[32];
        std::snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                      static_cast<int>(ms.count()));
        fprintf(g_log_file, "%s %-7s %s %s\n", time_buf, level, tag, message);
        fflush(g_log_file);
    }
}

void logWrite(int category, const char* level, const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Strip trailing newlines — external sources (mpv, CEF) often include them
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';

    writeLogLine(getCategoryTag(category), level, buf);
}

namespace {

std::atomic<bool> g_stderr_capture_running{false};
std::thread g_stderr_thread;
int g_pipe_read = -1;
int g_pipe_write = -1;

#ifdef _WIN32
HANDLE g_shutdown_event = NULL;
#else
int g_signal_pipe[2] = {-1, -1};  // [0]=read, [1]=write
#endif

void stderrCaptureThread() {
    char buf[4096];
    std::string partial_line;

#ifdef _WIN32
    HANDLE pipe_handle = (HANDLE)_get_osfhandle(g_pipe_read);
    HANDLE handles[2] = {pipe_handle, g_shutdown_event};

    while (g_stderr_capture_running) {
        DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 + 1) break;  // shutdown event
        if (result != WAIT_OBJECT_0) break;      // error

        int n = read(g_pipe_read, buf, sizeof(buf) - 1);
        if (n <= 0) break;

        buf[n] = '\0';
        partial_line += buf;

        size_t pos;
        while ((pos = partial_line.find('\n')) != std::string::npos) {
            std::string line = partial_line.substr(0, pos);
            partial_line = partial_line.substr(pos + 1);
            if (line.empty()) continue;
            logWrite(LOG_CEF, "DEBUG", "%s", line.c_str());
        }
    }
#else
    struct pollfd pfds[2] = {
        {g_pipe_read, POLLIN, 0},
        {g_signal_pipe[0], POLLIN, 0}
    };

    while (g_stderr_capture_running) {
        int ret = poll(pfds, 2, -1);
        if (ret < 0) break;

        if (pfds[1].revents & POLLIN) break;  // shutdown signal

        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(g_pipe_read, buf, sizeof(buf) - 1);
            if (n <= 0) break;

            buf[n] = '\0';
            partial_line += buf;

            size_t pos;
            while ((pos = partial_line.find('\n')) != std::string::npos) {
                std::string line = partial_line.substr(0, pos);
                partial_line = partial_line.substr(pos + 1);
                if (line.empty()) continue;
                logWrite(LOG_CEF, "DEBUG", "%s", line.c_str());
            }
        }
    }
#endif
}

} // namespace

void initStderrCapture() {
    // Save original stderr
    g_original_stderr_fd = dup(STDERR_FILENO);
    if (g_original_stderr_fd < 0) return;

    // Create pipe for stderr capture
    int fds[2];
    if (pipe(fds) < 0) {
        close(g_original_stderr_fd);
        g_original_stderr_fd = -1;
        return;
    }
    g_pipe_read = fds[0];
    g_pipe_write = fds[1];

#ifdef _WIN32
    // Create shutdown event
    g_shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_shutdown_event) {
        close(g_pipe_read);
        close(g_pipe_write);
        close(g_original_stderr_fd);
        g_original_stderr_fd = -1;
        return;
    }
#else
    // Create signal pipe for shutdown
    if (pipe(g_signal_pipe) < 0) {
        close(g_pipe_read);
        close(g_pipe_write);
        close(g_original_stderr_fd);
        g_original_stderr_fd = -1;
        return;
    }
#endif

    // Redirect stderr to pipe
    if (dup2(g_pipe_write, STDERR_FILENO) < 0) {
        close(g_pipe_read);
        close(g_pipe_write);
        close(g_original_stderr_fd);
        g_original_stderr_fd = -1;
#ifdef _WIN32
        CloseHandle(g_shutdown_event);
        g_shutdown_event = NULL;
#else
        close(g_signal_pipe[0]);
        close(g_signal_pipe[1]);
        g_signal_pipe[0] = g_signal_pipe[1] = -1;
#endif
        return;
    }

    // Start capture thread
    g_stderr_capture_running = true;
    g_stderr_thread = std::thread(stderrCaptureThread);
}

void shutdownStderrCapture() {
    if (!g_stderr_capture_running) return;

    g_stderr_capture_running = false;

#ifdef _WIN32
    // Signal thread to wake up
    if (g_shutdown_event) {
        SetEvent(g_shutdown_event);
    }
    // Close pipe write end to unblock read() if it's waiting
    if (g_pipe_write >= 0) {
        close(g_pipe_write);
        g_pipe_write = -1;
    }
#else
    // Write to signal pipe to wake thread
    if (g_signal_pipe[1] >= 0) {
        write(g_signal_pipe[1], "x", 1);
    }
#endif

    // Wait for thread to finish
    if (g_stderr_thread.joinable()) {
        g_stderr_thread.join();
    }

    // Restore stderr
    if (g_original_stderr_fd >= 0) {
        dup2(g_original_stderr_fd, STDERR_FILENO);
        close(g_original_stderr_fd);
        g_original_stderr_fd = -1;
    }

    // Cleanup pipes
    if (g_pipe_read >= 0) {
        close(g_pipe_read);
        g_pipe_read = -1;
    }
    if (g_pipe_write >= 0) {
        close(g_pipe_write);
        g_pipe_write = -1;
    }

#ifdef _WIN32
    if (g_shutdown_event) {
        CloseHandle(g_shutdown_event);
        g_shutdown_event = NULL;
    }
#else
    if (g_signal_pipe[0] >= 0) {
        close(g_signal_pipe[0]);
        g_signal_pipe[0] = -1;
    }
    if (g_signal_pipe[1] >= 0) {
        close(g_signal_pipe[1]);
        g_signal_pipe[1] = -1;
    }
#endif
}

void shutdownLogging() {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
}
