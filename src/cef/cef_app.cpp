#include "cef_app.h"
#include "resource_handler.h"
#include "include/cef_scheme.h"

// Process bootstrap and App handlers are implemented in the jfn-cef Rust
// crate. This translation unit is a thin C-ABI shim that preserves the
// CefRuntime:: API used by main.cpp.

extern "C" {

int  jfn_cef_start(int argc, char* argv[]);
void jfn_cef_set_log_severity(int severity);
void jfn_cef_set_remote_debugging_port(int port);
void jfn_cef_set_disable_gpu_compositing(bool disable);
#ifdef __linux__
void jfn_cef_set_ozone_platform(const char* platform_utf8);
#endif
void jfn_cef_set_context_initialized_callback(void (*cb)());
bool jfn_cef_initialize();
void jfn_cef_shutdown();

}  // extern "C"

// Single seam from Rust back to C++ during the transition: the Rust App's
// BrowserProcessHandler::OnContextInitialized invokes this; we register
// the embedded scheme handler factory, which still lives in C++ alongside
// resource_handler.cpp.
static void on_context_initialized_trampoline() {
    CefRegisterSchemeHandlerFactory("app", "", new EmbeddedSchemeHandlerFactory());
}

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
    jfn_cef_set_context_initialized_callback(&on_context_initialized_trampoline);
    return jfn_cef_initialize();
}

void Shutdown() {
    jfn_cef_shutdown();
}

}  // namespace CefRuntime
