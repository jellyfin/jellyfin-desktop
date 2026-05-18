#include "single_instance.h"

#include "logging.h"
#include "single_instance/jfn_single_instance.h"

#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace {

std::mutex g_cb_mutex;
std::function<void(const std::string&)> g_on_raise;

extern "C" void on_raise_thunk(const char* token, void* /*userdata*/) {
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(g_cb_mutex);
        cb = g_on_raise;
    }
    std::string t = token ? std::string(token) : std::string{};
    LOG_INFO(LOG_MAIN, "Received raise signal from another instance (token={})",
             t.empty() ? "none" : "present");
    if (cb) cb(t);
}

}  // namespace

bool trySignalExisting() {
    if (jfn_single_instance_try_signal_existing()) {
        LOG_INFO(LOG_MAIN, "Signaled existing instance to raise window");
        return true;
    }
    return false;
}

void startListener(std::function<void(const std::string&)> onRaise) {
    {
        std::lock_guard<std::mutex> lock(g_cb_mutex);
        g_on_raise = std::move(onRaise);
    }
    if (!jfn_single_instance_start_listener(on_raise_thunk, nullptr)) {
        LOG_WARN(LOG_MAIN, "Single-instance listener failed to start");
        std::lock_guard<std::mutex> lock(g_cb_mutex);
        g_on_raise = {};
    }
}

void stopListener() {
    jfn_single_instance_stop_listener();
    std::lock_guard<std::mutex> lock(g_cb_mutex);
    g_on_raise = {};
}
