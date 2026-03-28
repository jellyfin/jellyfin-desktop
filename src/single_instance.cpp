#include "single_instance.h"
#include "logging.h"
#include <thread>
#include <atomic>
#include <string>
#include <cstring>
#include <functional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstdlib>
#endif

namespace {

std::atomic<bool> g_listener_running{false};
std::thread g_listener_thread;

#ifdef _WIN32

HANDLE g_shutdown_event = NULL;
constexpr const char* PIPE_NAME = "\\\\.\\pipe\\jellyfin-desktop";

#else

int g_listen_fd = -1;
int g_wake_pipe[2] = {-1, -1};  // [0]=read, [1]=write

std::string getSocketPath() {
#ifdef __APPLE__
    const char* tmpdir = std::getenv("TMPDIR");
    if (tmpdir && tmpdir[0]) {
        std::string dir(tmpdir);
        if (dir.back() != '/') dir += '/';
        return dir + "jellyfin-desktop.sock";
    }
    return "/tmp/jellyfin-desktop.sock";
#else
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0])
        return std::string(runtime_dir) + "/jellyfin-desktop.sock";
    return "/tmp/jellyfin-desktop-" + std::to_string(getuid()) + ".sock";
#endif
}

#endif

} // namespace

#ifdef _WIN32

bool trySignalExisting() {
    HANDLE pipe = CreateFileA(
        PIPE_NAME,
        GENERIC_WRITE,
        0, NULL,
        OPEN_EXISTING,
        0, NULL);

    if (pipe == INVALID_HANDLE_VALUE)
        return false;

    const char msg[] = "raise\n";
    DWORD written;
    (void)WriteFile(pipe, msg, sizeof(msg) - 1, &written, NULL);
    CloseHandle(pipe);
    LOG_INFO(LOG_MAIN, "Signaled existing instance to raise window");
    return true;
}

void listenerThread(std::function<void(const std::string&)> onRaise) {
    while (g_listener_running) {
        HANDLE pipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 0, 256, 0, NULL);

        if (pipe == INVALID_HANDLE_VALUE)
            break;

        // Wait for connection or shutdown
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        ConnectNamedPipe(pipe, &overlapped);

        HANDLE handles[2] = {overlapped.hEvent, g_shutdown_event};
        DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        CloseHandle(overlapped.hEvent);

        if (result == WAIT_OBJECT_0 + 1) {
            // Shutdown
            CancelIo(pipe);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        // Read message
        char buf[256];
        DWORD bytesRead;
        if (ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            if (std::strstr(buf, "raise")) {
                LOG_INFO(LOG_MAIN, "Received raise signal from another instance");
                onRaise(std::string{});
            }
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void startListener(std::function<void(const std::string&)> onRaise) {
    g_shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_shutdown_event)
        return;

    g_listener_running = true;
    g_listener_thread = std::thread(listenerThread, std::move(onRaise));
}

void stopListener() {
    if (!g_listener_running)
        return;

    g_listener_running = false;

    if (g_shutdown_event) {
        SetEvent(g_shutdown_event);
    }

    // Connect to unblock the waiting pipe
    HANDLE pipe = CreateFileA(PIPE_NAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);

    if (g_listener_thread.joinable())
        g_listener_thread.join();

    if (g_shutdown_event) {
        CloseHandle(g_shutdown_event);
        g_shutdown_event = NULL;
    }
}

#else // Unix (Linux + macOS)

bool trySignalExisting() {
    std::string path = getSocketPath();
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        LOG_ERROR(LOG_MAIN, "Socket path too long (%zu bytes): %s", path.size(), path.c_str());
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        if (errno == ECONNREFUSED || errno == ENOENT) {
            // Stale socket — remove it so we can bind
            unlink(path.c_str());
        }
        return false;
    }

    // Forward XDG_ACTIVATION_TOKEN so the existing instance can use it
    // to activate its window via xdg-activation-v1 protocol
    std::string msg = "raise";
    const char* token = std::getenv("XDG_ACTIVATION_TOKEN");
    if (token && token[0]) {
        msg += ' ';
        msg += token;
    }
    msg += "\n";

    // Best-effort write; we're about to exit anyway
    ssize_t n = write(fd, msg.c_str(), msg.size());
    (void)n;
    close(fd);
    LOG_INFO(LOG_MAIN, "Signaled existing instance to raise window");
    return true;
}

void listenerThread(std::function<void(const std::string&)> onRaise) {
    struct pollfd pfds[2] = {
        {g_listen_fd, POLLIN, 0},
        {g_wake_pipe[0], POLLIN, 0}
    };

    while (g_listener_running) {
        int ret = poll(pfds, 2, -1);
        if (ret < 0)
            break;

        // Shutdown signal
        if (pfds[1].revents & POLLIN)
            break;

        if (pfds[0].revents & POLLIN) {
            int client = accept(g_listen_fd, nullptr, nullptr);
            if (client < 0)
                continue;

            char buf[256];
            ssize_t n = read(client, buf, sizeof(buf) - 1);
            close(client);

            if (n > 0) {
                buf[n] = '\0';
                if (std::strstr(buf, "raise")) {
                    // Parse optional activation token after "raise "
                    std::string token;
                    const char* space = std::strchr(buf, ' ');
                    if (space) {
                        token = space + 1;
                        // Strip trailing newline
                        while (!token.empty() && (token.back() == '\n' || token.back() == '\r'))
                            token.pop_back();
                    }
                    LOG_INFO(LOG_MAIN, "Received raise signal from another instance (token=%s)",
                             token.empty() ? "none" : "present");
                    onRaise(token);
                }
            }
        }
    }
}

void startListener(std::function<void(const std::string&)> onRaise) {
    std::string path = getSocketPath();
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        LOG_ERROR(LOG_MAIN, "Socket path too long (%zu bytes): %s", path.size(), path.c_str());
        return;
    }

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0)
        return;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(g_listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_WARN(LOG_MAIN, "Single-instance listener: bind failed (%s)", std::strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return;
    }

    if (listen(g_listen_fd, 2) < 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
        unlink(path.c_str());
        return;
    }

    if (pipe(g_wake_pipe) < 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
        unlink(path.c_str());
        return;
    }

    g_listener_running = true;
    g_listener_thread = std::thread(listenerThread, std::move(onRaise));
}

void stopListener() {
    if (!g_listener_running)
        return;

    g_listener_running = false;

    // Wake listener thread
    if (g_wake_pipe[1] >= 0) {
        ssize_t n = write(g_wake_pipe[1], "x", 1);
        (void)n;
    }

    if (g_listener_thread.joinable())
        g_listener_thread.join();

    // Clean up socket file
    std::string path = getSocketPath();
    unlink(path.c_str());

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_wake_pipe[0] >= 0) {
        close(g_wake_pipe[0]);
        g_wake_pipe[0] = -1;
    }
    if (g_wake_pipe[1] >= 0) {
        close(g_wake_pipe[1]);
        g_wake_pipe[1] = -1;
    }
}

#endif
