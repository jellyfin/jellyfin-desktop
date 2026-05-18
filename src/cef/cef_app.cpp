#include "cef_app.h"

// Process bootstrap, App handlers, and the app:// scheme resource handler
// live in the jfn-cef Rust crate. This translation unit is a thin C-ABI
// shim that preserves the CefRuntime:: API used by main.cpp.

extern "C" {

int  jfn_cef_start(int argc, char* argv[]);
void jfn_cef_set_log_severity(int severity);
void jfn_cef_set_remote_debugging_port(int port);
void jfn_cef_set_disable_gpu_compositing(bool disable);
#ifdef __linux__
void jfn_cef_set_ozone_platform(const char* platform_utf8);
#endif
bool jfn_cef_initialize();
void jfn_cef_shutdown();

}  // extern "C"

namespace CefRuntime {

int Start(int argc, char* argv[]) {
    return jfn_cef_start(argc, argv);
}

void SetLogSeverity(cef_log_severity_t severity) {
    jfn_cef_set_log_severity(static_cast<int>(severity));
}

void SetRemoteDebuggingPort(int port) {
    jfn_cef_set_remote_debugging_port(port);
}

void SetDisableGpuCompositing(bool disable) {
    jfn_cef_set_disable_gpu_compositing(disable);
}

#ifdef __linux__
void SetOzonePlatform(const std::string& platform) {
    jfn_cef_set_ozone_platform(platform.c_str());
}
#endif

bool Initialize() {
    return jfn_cef_initialize();
}

void Shutdown() {
    jfn_cef_shutdown();
}

}  // namespace CefRuntime
