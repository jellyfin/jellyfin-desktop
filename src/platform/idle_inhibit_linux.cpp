#include "idle_inhibit_linux.h"

#include <systemd/sd-bus.h>
#include <unistd.h>

namespace idle_inhibit {
namespace {

sd_bus* g_bus = nullptr;
int g_inhibit_fd = -1;

void release() {
    if (g_inhibit_fd >= 0) {
        close(g_inhibit_fd);
        g_inhibit_fd = -1;
    }
}

}  // namespace

void init() {}

void set(IdleInhibitLevel level) {
    release();
    if (level == IdleInhibitLevel::None) return;

    if (!g_bus) {
        if (sd_bus_open_system(&g_bus) < 0) {
            g_bus = nullptr;
            return;
        }
    }

    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(
        g_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Inhibit",
        &error, &reply,
        "ssss",
        level == IdleInhibitLevel::Display ? "idle:sleep" : "sleep",
        "Jellyfin Desktop",
        "Media playback",
        "block");
    if (r >= 0 && reply) {
        int fd = -1;
        if (sd_bus_message_read(reply, "h", &fd) >= 0 && fd >= 0)
            g_inhibit_fd = dup(fd);
        sd_bus_message_unref(reply);
    }
    sd_bus_error_free(&error);
}

void cleanup() {
    release();
    if (g_bus) { sd_bus_unref(g_bus); g_bus = nullptr; }
}

}  // namespace idle_inhibit
