#ifdef _WIN32
// platform_windows.cpp — Windows platform layer.
//
// Compositor (D3D11/DirectComposition per-surface) now lives in the Rust
// jfn-windows crate (src/windows/src/compositor.rs); this file owns only
// the HWND, the WndProc hook, fullscreen helpers, scale lookups, and the
// input thread bootstrap. The Rust crate exposes the compositor via
// jfn_win_init_compositor / jfn_win_cleanup_compositor and a handful of
// transition accessors called from WndProc.

#include "platform/platform.h"
#include "common.h"
#include "input/input_windows.h"
#include "logging.h"
#include "mpv/jfn_mpv_api.h"
#include "playback/jfn_ingest.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <atomic>
#include <thread>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

// =====================================================================
// Windows state (file-static)
//
// All compositor state (D3D11 device, DComp device/target/root, the
// surface registry, transition_pw/ph + expected_w/h + pending_lw/lh,
// mpv_pw/ph) now lives in the Rust jfn-windows crate.
// =====================================================================

struct WinState {
    HWND mpv_hwnd = nullptr;

    float cached_scale = 1.0f;

    // Fullscreen transition bookkeeping that stays C++-side because it's
    // read/written by the WndProc and the fullscreen toggle helpers below.
    bool was_fullscreen = false;
    bool was_minimized = false;
    bool restore_maximized_on_unfullscreen = false;

    // Input thread (body lives in input::windows::run_input_thread)
    std::thread input_thread;
};

static WinState g_win;

// Rust-side compositor entry points (src/windows/src/compositor.rs).
extern "C" bool jfn_win_init_compositor(void* hwnd);
extern "C" void jfn_win_cleanup_compositor();
extern "C" void jfn_win_update_surface_size(int lw, int lh, int pw, int ph);
extern "C" void jfn_win_wndproc_begin_transition_locked();
extern "C" void jfn_win_wndproc_end_transition_locked();
extern "C" bool win_in_transition();

extern "C" bool win_is_fullscreen_style(LONG_PTR style) {
    return (style & WS_CAPTION) == 0 && (style & WS_THICKFRAME) == 0;
}

// =====================================================================
// Fullscreen
// =====================================================================

extern "C" void win_set_fullscreen(bool fullscreen) {
    if (!jfn_mpv_handle_get()) return;
    if (jfn_playback_fullscreen() == fullscreen) {
        if (win_in_transition() && fullscreen == g_win.was_fullscreen)
            jfn_win_wndproc_end_transition_locked();
        return;
    }

    if (fullscreen)
        g_win.restore_maximized_on_unfullscreen = IsZoomed(g_win.mpv_hwnd) != 0;

    bool should_restore_maximized = false;
    if (!fullscreen) {
        should_restore_maximized = g_win.restore_maximized_on_unfullscreen;
        g_win.restore_maximized_on_unfullscreen = false;
    }

    bool is_minimized_now = IsMinimized(g_win.mpv_hwnd) != 0;
    if (!is_minimized_now)
        jfn_win_wndproc_begin_transition_locked();

    if (fullscreen)
        jfn_mpv_set_window_minimized(false);

    jfn_mpv_set_fullscreen(fullscreen);

    if (!fullscreen && should_restore_maximized)
        jfn_mpv_set_window_maximized(true);
}

extern "C" void win_toggle_fullscreen() {
    if (!jfn_mpv_handle_get()) return;
    bool target_fullscreen = !jfn_playback_fullscreen();

    if (target_fullscreen)
        g_win.restore_maximized_on_unfullscreen = IsZoomed(g_win.mpv_hwnd) != 0;

    bool should_restore_maximized = false;
    if (!target_fullscreen) {
        should_restore_maximized = g_win.restore_maximized_on_unfullscreen;
        g_win.restore_maximized_on_unfullscreen = false;
    }

    bool is_minimized_now = IsMinimized(g_win.mpv_hwnd) != 0;
    if (!is_minimized_now)
        jfn_win_wndproc_begin_transition_locked();

    if (target_fullscreen)
        jfn_mpv_set_window_minimized(false);

    jfn_mpv_toggle_fullscreen();

    if (!target_fullscreen && should_restore_maximized)
        jfn_mpv_set_window_maximized(true);
}

// =====================================================================
// Scale + content size
// =====================================================================

extern "C" float win_get_scale() {
    double scale = jfn_playback_display_scale();
    if (scale > 0) {
        g_win.cached_scale = static_cast<float>(scale);
        return g_win.cached_scale;
    }
    if (g_win.cached_scale > 0) return g_win.cached_scale;
    // Pre-mpv (e.g. default-geometry sizing at startup): ask the OS directly.
    UINT dpi = GetDpiForSystem();
    if (dpi > 0) return static_cast<float>(dpi) / 96.0f;
    return 1.0f;
}

// Per-monitor DPI (GetDpiForMonitor) lives in Shcore.dll which isn't
// currently linked; fall back to system DPI and ignore (x, y).
extern "C" float win_get_display_scale(int /*x*/, int /*y*/) {
    UINT dpi = GetDpiForSystem();
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

// =====================================================================
// Idle inhibit posting helper (called from Rust win_set_idle_inhibit).
// =====================================================================

#include "include/cef_task.h"

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

// Narrow accessor exposing g_win.mpv_hwnd to the Rust crate.
extern "C" void* jfn_win_get_hwnd() {
    return reinterpret_cast<void*>(g_win.mpv_hwnd);
}

// =====================================================================
// WndProc hook: monitor mpv's HWND for size/fullscreen/close.
// =====================================================================

static HHOOK g_wndproc_hook = nullptr;

static LRESULT CALLBACK mpv_wndproc_hook(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode >= 0) {
        auto* msg = reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (msg->hwnd == g_win.mpv_hwnd) {
            if (msg->message == WM_SIZE) {
                if (msg->wParam == SIZE_MINIMIZED) {
                    g_win.was_minimized = true;
                    return CallNextHookEx(g_wndproc_hook, nCode, wp, lp);
                }

                int pw = LOWORD(msg->lParam);
                int ph = HIWORD(msg->lParam);
                if (pw > 0 && ph > 0) {
                    input::windows::resize_to_parent(pw, ph);

                    float scale = g_win.cached_scale > 0 ? g_win.cached_scale : 1.0f;
                    int lw = static_cast<int>(pw / scale);
                    int lh = static_cast<int>(ph / scale);

                    LONG_PTR style = GetWindowLongPtr(g_win.mpv_hwnd, GWL_STYLE);
                    bool fs = win_is_fullscreen_style(style);

                    bool recovering_from_minimize = g_win.was_minimized;
                    if (recovering_from_minimize) {
                        g_win.was_minimized = false;
                        g_win.was_fullscreen = fs;
                        if (win_in_transition())
                            jfn_win_wndproc_end_transition_locked();
                    } else if (fs != g_win.was_fullscreen) {
                        if (!win_in_transition())
                            jfn_win_wndproc_begin_transition_locked();
                        else
                            jfn_win_wndproc_end_transition_locked();
                        g_win.was_fullscreen = fs;
                    } else if (win_in_transition()) {
                        jfn_win_wndproc_end_transition_locked();
                    }
                    jfn_win_update_surface_size(lw, lh, pw, ph);
                }
            } else if (msg->message == WM_CLOSE) {
                initiate_shutdown();
            }
        }
    }
    return CallNextHookEx(g_wndproc_hook, nCode, wp, lp);
}

// =====================================================================
// Platform interface
// =====================================================================

extern "C" void win_early_init() {
    // Nothing needed on Windows before mpv starts
}

extern "C" bool win_init(mpv_handle* mpv) {
    int64_t wid = 0;
    if (jfn_mpv_get_property_int("window-id", &wid) < 0 || !wid) {
        LOG_ERROR(LOG_PLATFORM, "Failed to get window-id from mpv");
        return false;
    }
    g_win.mpv_hwnd = reinterpret_cast<HWND>(wid);

    win_get_scale();

    // Enable DWM transparency so DComp visuals with premultiplied alpha work.
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_win.mpv_hwnd, &margins);

    if (!jfn_win_init_compositor(g_win.mpv_hwnd)) return false;

    // Seed was_fullscreen before installing the hook so the first WM_SIZE
    // doesn't start a spurious transition if already fullscreen.
    {
        LONG_PTR style = GetWindowLongPtr(g_win.mpv_hwnd, GWL_STYLE);
        g_win.was_fullscreen = win_is_fullscreen_style(style);
    }

    DWORD mpv_tid = GetWindowThreadProcessId(g_win.mpv_hwnd, nullptr);
    g_wndproc_hook = SetWindowsHookEx(WH_CALLWNDPROCRET, mpv_wndproc_hook,
        nullptr, mpv_tid);

    HWND mpv_hwnd = g_win.mpv_hwnd;
    g_win.input_thread = std::thread([mpv_hwnd]() {
        input::windows::run_input_thread(mpv_hwnd);
    });

    LOG_INFO(LOG_PLATFORM, "Windows DirectComposition compositor initialized");
    return true;
}

extern "C" void win_cleanup() {
    input::windows::stop_input_thread();
    if (g_win.input_thread.joinable())
        g_win.input_thread.join();
    if (g_wndproc_hook) { UnhookWindowsHookEx(g_wndproc_hook); g_wndproc_hook = nullptr; }

    jfn_win_cleanup_compositor();

    g_win.mpv_hwnd = nullptr;
}

// =====================================================================
// Window-position / geometry helpers (untouched).
// =====================================================================

// Query window position relative to the monitor's working area (excludes
// taskbar), in physical pixels. Matches mpv's --geometry +X+Y coordinate
// system on Windows (vo_calc_window_geometry uses the working area).
extern "C" bool win_query_window_position(int* x, int* y) {
    if (!g_win.mpv_hwnd) return false;
    RECT wr;
    if (!GetWindowRect(g_win.mpv_hwnd, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(g_win.mpv_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) return false;
    *x = wr.left - mi.rcWork.left;
    *y = wr.top - mi.rcWork.top;
    return true;
}

// Resolve saved geometry against the primary monitor's working area so the
// window never opens larger than the screen or off-screen, and center any
// unset axis.
extern "C" void win_clamp_window_geometry(int* w, int* h, int* x, int* y) {
    RECT work;
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0)) return;
    int vw = work.right - work.left;
    int vh = work.bottom - work.top;
    if (*w > vw) *w = vw;
    if (*h > vh) *h = vh;
    if (*x < 0) *x = (vw - *w) / 2;
    if (*y < 0) *y = (vh - *h) / 2;
    if (*x + *w > vw) *x = vw - *w;
    if (*y + *h > vh) *y = vh - *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

// =====================================================================
// make_windows_platform
// =====================================================================
//
// Platform vtable composition moved to the Rust jfn-windows crate
// (src/windows/src/lib.rs).

#endif // _WIN32
