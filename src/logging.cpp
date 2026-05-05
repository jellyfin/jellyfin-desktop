#include "logging.h"

#include "log_redact.h"
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/RotatingFileSink.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#define pipe(fds) _pipe(fds, 4096, _O_BINARY)
#define read _read
#define dup _dup
#define dup2 _dup2
#define close _close
#define STDERR_FILENO 2
#else
#include <unistd.h>
#include <poll.h>
#endif

quill::Logger* g_loggers[LOG_CATEGORY_COUNT] = {};

static std::string g_active_log_path;

// Sink mixin that post-processes the rendered log_statement before handing it
// to the real sink. Two transforms:
//   1. Replace embedded newlines (mainly multi-line JS stack traces) with
//      spaces so each log entry stays on one line.
//   2. Redact auth tokens / API keys via log_redact::censor.
// Runs on Quill's backend worker thread so call sites stay free of mangling.
template <class Base>
class SanitizingSink : public Base {
public:
    using Base::Base;
    void write_log(quill::MacroMetadata const* md, uint64_t ts,
                   std::string_view thread_id, std::string_view thread_name,
                   std::string const& process_id, std::string_view logger_name,
                   quill::LogLevel level, std::string_view level_desc,
                   std::string_view level_code,
                   std::vector<std::pair<std::string, std::string>> const* named_args,
                   std::string_view log_message, std::string_view log_statement) override {
        // Last char is the pattern suffix '\n' which Quill added — keep it.
        // Only consider transforms over the body.
        auto body_end = log_statement.size();
        if (body_end > 0 && log_statement[body_end - 1] == '\n') --body_end;
        std::string_view body = log_statement.substr(0, body_end);

        bool has_newline = body.find_first_of("\r\n") != std::string_view::npos;
        bool has_secret = log_redact::containsSecret(body);

        if (!has_newline && !has_secret) {
            Base::write_log(md, ts, thread_id, thread_name, process_id, logger_name,
                            level, level_desc, level_code, named_args,
                            log_message, log_statement);
            return;
        }

        std::string cleaned(log_statement);
        if (has_newline) {
            for (size_t i = 0; i < body_end; ++i) {
                if (cleaned[i] == '\r' || cleaned[i] == '\n') cleaned[i] = ' ';
            }
        }
        if (has_secret) {
            log_redact::censor(cleaned);
        }
        Base::write_log(md, ts, thread_id, thread_name, process_id, logger_name,
                        level, level_desc, level_code, named_args,
                        log_message, std::string_view(cleaned));
    }
};

using ConsoleSinkSanitizing = SanitizingSink<quill::ConsoleSink>;
using RotatingFileSinkSanitizing = SanitizingSink<quill::RotatingFileSink>;

namespace {

constexpr const char* kCategoryNames[LOG_CATEGORY_COUNT] = {
    "Main", "mpv", "CEF", "Media", "Platform", "JS", "Resource",
};

// ---- stderr capture ------------------------------------------------------

std::atomic<bool> g_capture_running{false};
std::thread g_capture_thread;
int g_original_stderr_fd = -1;
int g_pipe_read = -1;
int g_pipe_write = -1;

#ifdef _WIN32
HANDLE g_shutdown_event = NULL;
#else
int g_signal_pipe[2] = {-1, -1};
#endif

void feedLine(std::string& partial, const char* data, size_t n) {
    partial.append(data, n);
    size_t pos;
    while ((pos = partial.find('\n')) != std::string::npos) {
        std::string line = partial.substr(0, pos);
        partial.erase(0, pos + 1);
        if (!line.empty()) LOG_DEBUG(LOG_CEF, "{}", line);
    }
}

void captureThread() {
    char buf[4096];
    std::string partial;

#ifdef _WIN32
    HANDLE pipe_handle = (HANDLE)_get_osfhandle(g_pipe_read);
    HANDLE handles[2] = {pipe_handle, g_shutdown_event};

    while (g_capture_running) {
        DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0) break;
        int n = ::read(g_pipe_read, buf, sizeof(buf));
        if (n <= 0) break;
        feedLine(partial, buf, static_cast<size_t>(n));
    }
#else
    struct pollfd pfds[2] = {
        {g_pipe_read,       POLLIN, 0},
        {g_signal_pipe[0],  POLLIN, 0},
    };
    while (g_capture_running) {
        if (poll(pfds, 2, -1) < 0) break;
        if (pfds[1].revents & POLLIN) break;
        if (pfds[0].revents & POLLIN) {
            ssize_t n = ::read(g_pipe_read, buf, sizeof(buf));
            if (n <= 0) break;
            feedLine(partial, buf, static_cast<size_t>(n));
        }
    }
#endif
}

void initStderrCapture() {
    g_original_stderr_fd = dup(STDERR_FILENO);
    if (g_original_stderr_fd < 0) return;

    int fds[2];
    if (pipe(fds) < 0) {
        close(g_original_stderr_fd);
        g_original_stderr_fd = -1;
        return;
    }
    g_pipe_read = fds[0];
    g_pipe_write = fds[1];

#ifdef _WIN32
    g_shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_shutdown_event) goto fail;
#else
    if (pipe(g_signal_pipe) < 0) goto fail;
#endif

    if (dup2(g_pipe_write, STDERR_FILENO) < 0) goto fail;

    g_capture_running = true;
    g_capture_thread = std::thread(captureThread);
    return;

fail:
    if (g_pipe_read >= 0) { close(g_pipe_read); g_pipe_read = -1; }
    if (g_pipe_write >= 0) { close(g_pipe_write); g_pipe_write = -1; }
#ifdef _WIN32
    if (g_shutdown_event) { CloseHandle(g_shutdown_event); g_shutdown_event = NULL; }
#else
    if (g_signal_pipe[0] >= 0) { close(g_signal_pipe[0]); g_signal_pipe[0] = -1; }
    if (g_signal_pipe[1] >= 0) { close(g_signal_pipe[1]); g_signal_pipe[1] = -1; }
#endif
    if (g_original_stderr_fd >= 0) { close(g_original_stderr_fd); g_original_stderr_fd = -1; }
}

void shutdownStderrCapture() {
    if (!g_capture_running) return;
    g_capture_running = false;

    if (g_original_stderr_fd >= 0) {
        dup2(g_original_stderr_fd, STDERR_FILENO);
        close(g_original_stderr_fd);
        g_original_stderr_fd = -1;
    }

#ifdef _WIN32
    if (g_shutdown_event) SetEvent(g_shutdown_event);
    if (g_pipe_write >= 0) { close(g_pipe_write); g_pipe_write = -1; }
#else
    if (g_signal_pipe[1] >= 0) ::write(g_signal_pipe[1], "x", 1);
#endif

    if (g_capture_thread.joinable()) g_capture_thread.join();

    if (g_pipe_read >= 0)  { close(g_pipe_read);  g_pipe_read  = -1; }
    if (g_pipe_write >= 0) { close(g_pipe_write); g_pipe_write = -1; }
#ifdef _WIN32
    if (g_shutdown_event) { CloseHandle(g_shutdown_event); g_shutdown_event = NULL; }
#else
    if (g_signal_pipe[0] >= 0) { close(g_signal_pipe[0]); g_signal_pipe[0] = -1; }
    if (g_signal_pipe[1] >= 0) { close(g_signal_pipe[1]); g_signal_pipe[1] = -1; }
#endif
}

}  // namespace

static quill::LogLevel toQuillLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return quill::LogLevel::TraceL1;
        case LogLevel::Debug: return quill::LogLevel::Debug;
        case LogLevel::Info:  return quill::LogLevel::Info;
        case LogLevel::Warn:  return quill::LogLevel::Warning;
        case LogLevel::Error: return quill::LogLevel::Error;
        case LogLevel::Default: break;
    }
    return quill::LogLevel::TraceL3;  // allow everything
}

const std::string& activeLogPath() {
    return g_active_log_path;
}

void initLogging(const char* path, LogLevel min_level) {
    g_active_log_path = (path && path[0]) ? path : "";

    quill::Backend::start();

    std::vector<std::shared_ptr<quill::Sink>> sinks;

    // Terminal: just "[Category] message", no timestamp/level.
    quill::ConsoleSinkConfig console_config;
    console_config.set_override_pattern_formatter_options(quill::PatternFormatterOptions{
        "[%(logger)] %(message)", "", quill::Timezone::LocalTime});
    sinks.push_back(quill::Frontend::create_or_get_sink<ConsoleSinkSanitizing>(
        "console_sink", console_config));

    // File: ISO timestamp, padded level, then "[Category] message".
    // Rotates on startup + at 10 MB; keeps 3 previous runs.
    if (path && path[0]) {
        quill::RotatingFileSinkConfig file_config;
        file_config.set_open_mode('w');
        file_config.set_rotation_max_file_size(10 * 1024 * 1024);
        file_config.set_max_backup_files(3);
        file_config.set_rotation_on_creation(true);
        file_config.set_remove_old_files(true);
        file_config.set_override_pattern_formatter_options(quill::PatternFormatterOptions{
            "%(time) %(log_level:<7) [%(logger)] %(message)",
            "%Y-%m-%dT%H:%M:%S",
            quill::Timezone::LocalTime});
        sinks.push_back(quill::Frontend::create_or_get_sink<RotatingFileSinkSanitizing>(path, file_config));
    }

    quill::LogLevel level = toQuillLevel(min_level);
    for (int i = 0; i < LOG_CATEGORY_COUNT; ++i) {
        g_loggers[i] = quill::Frontend::create_or_get_logger(kCategoryNames[i], sinks);
        g_loggers[i]->set_log_level(level);
    }

    initStderrCapture();
}

void shutdownLogging() {
    shutdownStderrCapture();
    for (auto*& logger : g_loggers) {
        if (logger) logger->flush_log();
        logger = nullptr;
    }
    quill::Backend::stop();
}
