#include "open_url_linux.h"

#include "../logging.h"

#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <thread>

namespace open_url_linux {

void open(const std::string& url) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,  "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    const char* argv[] = {"xdg-open", url.c_str(), nullptr};
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, "xdg-open", &actions, nullptr,
                          const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0) {
        LOG_ERROR(LOG_PLATFORM, "posix_spawnp(xdg-open) failed: {}", strerror(rc));
        return;
    }
    // xdg-open exits quickly after daemonizing the real handler; reap it.
    std::thread([pid] { int st = 0; waitpid(pid, &st, 0); }).detach();
}

}
