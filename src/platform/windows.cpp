#ifdef _WIN32
// platform_windows.cpp — Windows platform layer.
//
// All Windows-specific platform vtable functions (compositor, HWND
// lifecycle, WndProc hook, fullscreen helpers, scale/geometry lookups,
// theme color, idle inhibit, clipboard, URL launch) now live in the
// Rust `jfn-windows` crate (`src/windows/src/`). This file keeps only
// the C-linkage helper that posts SetThreadExecutionState onto TID_UI
// via CefPostTask — CefTask is C++-only so the bouncer stays here.

#include "include/cef_task.h"

#include <windows.h>

#include <functional>
#include <utility>

namespace {
class FnTask : public CefTask {
public:
    explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override { if (fn_) fn_(); }
private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(FnTask);
};
}

extern "C" void jfn_win_post_execution_state(uint32_t flags) {
    CefPostTask(TID_UI, new FnTask([flags]() {
        SetThreadExecutionState(static_cast<EXECUTION_STATE>(flags));
    }));
}

#endif // _WIN32
