#ifdef _WIN32
// platform_windows.cpp — Windows platform layer.
// D3D11 + DirectComposition composites CEF shared textures (main + overlay)
// onto mpv's HWND. A transparent child HWND captures input for CEF.

#include "platform/platform.h"
#include "common.h"
#include "cef/cef_client.h"
#include "browser/browsers.h"
#include "browser/web_browser.h"
#include "browser/overlay_browser.h"
#include "input/input_windows.h"
#include "logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <mutex>
#include <thread>
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

// =====================================================================
// Windows state (file-static)
// =====================================================================

struct WinState {
    std::mutex surface_mtx;  // protects swap chain ops during transitions

    HWND mpv_hwnd = nullptr;

    // D3D11
    ID3D11Device1* d3d_device = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;
    IDXGIFactory2* dxgi_factory = nullptr;

    // DirectComposition
    IDCompositionDevice* dcomp_device = nullptr;
    IDCompositionTarget* dcomp_target = nullptr;
    IDCompositionVisual* dcomp_root = nullptr;
    IDCompositionVisual* dcomp_main_visual = nullptr;
    IDCompositionVisual* dcomp_overlay_visual = nullptr;
    IDCompositionEffectGroup* dcomp_overlay_effect = nullptr;

    // Main browser swap chain
    IDXGISwapChain1* main_swap_chain = nullptr;
    int main_sw = 0, main_sh = 0;

    // Overlay browser swap chain
    IDXGISwapChain1* overlay_swap_chain = nullptr;
    int overlay_sw = 0, overlay_sh = 0;
    bool overlay_visible = false;

    // Window state
    float cached_scale = 1.0f;
    int mpv_pw = 0, mpv_ph = 0;  // mpv's current physical size

    // Fullscreen transition
    int expected_w = 0, expected_h = 0;
    int transition_pw = 0, transition_ph = 0;
    int pending_lw = 0, pending_lh = 0;
    bool transitioning = false;
    bool was_fullscreen = false;

    // Input thread (body lives in input::windows::run_input_thread)
    std::thread input_thread;
};

static WinState g_win;

static void win_begin_transition_locked();
static void win_end_transition_locked();

// =====================================================================
// D3D11 / DXGI / DComp initialization
// =====================================================================

static bool init_d3d() {
    // Create D3D11 device
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* base_device = nullptr;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 2, D3D11_SDK_VERSION, &base_device, nullptr, &g_win.d3d_context);
    if (FAILED(hr) || !base_device) {
        LOG_ERROR(LOG_PLATFORM, "D3D11CreateDevice failed: 0x{:08x}", hr);
        return false;
    }
    hr = base_device->QueryInterface(__uuidof(ID3D11Device1), (void**)&g_win.d3d_device);
    base_device->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "QueryInterface for ID3D11Device1 failed: 0x{:08x}", hr);
        return false;
    }

    // Get DXGI factory
    IDXGIDevice* dxgi_device = nullptr;
    g_win.d3d_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    IDXGIAdapter* adapter = nullptr;
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&g_win.dxgi_factory);
    adapter->Release();
    dxgi_device->Release();

    return true;
}

static bool init_dcomp() {
    HRESULT hr = DCompositionCreateDevice(nullptr, __uuidof(IDCompositionDevice),
        (void**)&g_win.dcomp_device);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "DCompositionCreateDevice failed: 0x{:08x}", hr);
        return false;
    }

    hr = g_win.dcomp_device->CreateTargetForHwnd(g_win.mpv_hwnd, FALSE, &g_win.dcomp_target);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "CreateTargetForHwnd failed: 0x{:08x}", hr);
        return false;
    }

    // Visual tree: root -> main (bottom), overlay (top)
    g_win.dcomp_device->CreateVisual(&g_win.dcomp_root);
    g_win.dcomp_device->CreateVisual(&g_win.dcomp_main_visual);
    g_win.dcomp_device->CreateVisual(&g_win.dcomp_overlay_visual);
    g_win.dcomp_device->CreateEffectGroup(&g_win.dcomp_overlay_effect);
    g_win.dcomp_overlay_visual->SetEffect(g_win.dcomp_overlay_effect);

    g_win.dcomp_root->AddVisual(g_win.dcomp_main_visual, TRUE, nullptr);
    g_win.dcomp_root->AddVisual(g_win.dcomp_overlay_visual, TRUE, g_win.dcomp_main_visual);
    g_win.dcomp_target->SetRoot(g_win.dcomp_root);
    g_win.dcomp_device->Commit();

    return true;
}

static IDXGISwapChain1* create_swap_chain(int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    IDXGISwapChain1* sc = nullptr;
    HRESULT hr = g_win.dxgi_factory->CreateSwapChainForComposition(
        g_win.d3d_device, &desc, nullptr, &sc);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "CreateSwapChainForComposition failed: 0x{:08x}", hr);
        return nullptr;
    }
    return sc;
}

static void ensure_swap_chain(IDXGISwapChain1*& sc, int& sw, int& sh,
                              IDCompositionVisual* visual, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (sc && sw == w && sh == h) return;

    if (sc) {
        // Try resize first
        HRESULT hr = sc->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (SUCCEEDED(hr)) {
            sw = w; sh = h;
            return;
        }
        // Resize failed, recreate
        visual->SetContent(nullptr);
        sc->Release();
        sc = nullptr;
    }

    sc = create_swap_chain(w, h);
    if (sc) {
        visual->SetContent(sc);
        sw = w; sh = h;
    }
}

// =====================================================================
// Present CEF shared texture -- main browser
// =====================================================================

static void win_present(const CefAcceleratedPaintInfo& info) {
    HANDLE handle = info.shared_texture_handle;
    if (!handle) return;

    // Open shared texture to query dimensions
    ID3D11Texture2D* src = nullptr;
    HRESULT hr = g_win.d3d_device->OpenSharedResource1(handle,
        __uuidof(ID3D11Texture2D), (void**)&src);
    if (FAILED(hr) || !src) return;

    D3D11_TEXTURE2D_DESC td;
    src->GetDesc(&td);
    int w = static_cast<int>(td.Width);
    int h = static_cast<int>(td.Height);

    std::lock_guard<std::mutex> lock(g_win.surface_mtx);

    // Drop frames during transition (same logic as Wayland)
    if (g_win.transitioning) {
        if (g_win.expected_w <= 0 || (w == g_win.transition_pw && h == g_win.transition_ph)) {
            src->Release();
            return;
        }
        // New frame matches expected size -- end transition
        win_end_transition_locked();
    }

    // Drop oversized buffers
    if (g_win.mpv_pw > 0 && (w > g_win.mpv_pw + 2 || h > g_win.mpv_ph + 2)) {
        src->Release();
        return;
    }

    // 1:1 pixel mapping: swap chain matches CEF buffer size (never stretch)
    ensure_swap_chain(g_win.main_swap_chain, g_win.main_sw, g_win.main_sh,
                      g_win.dcomp_main_visual, w, h);
    if (!g_win.main_swap_chain) { src->Release(); return; }

    ID3D11Texture2D* bb = nullptr;
    g_win.main_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_win.d3d_context->CopyResource(bb, src);
    bb->Release();
    src->Release();

    g_win.main_swap_chain->Present(0, 0);
    g_win.dcomp_device->Commit();
}

static void win_present_software(const CefRenderHandler::RectList&, const void*, int, int) {
    // Software fallback not implemented for Windows
}

// =====================================================================
// Present CEF shared texture -- overlay browser
// =====================================================================

static void win_overlay_present(const CefAcceleratedPaintInfo& info) {
    HANDLE handle = info.shared_texture_handle;
    if (!handle) return;

    ID3D11Texture2D* src = nullptr;
    HRESULT hr = g_win.d3d_device->OpenSharedResource1(handle,
        __uuidof(ID3D11Texture2D), (void**)&src);
    if (FAILED(hr) || !src) return;

    D3D11_TEXTURE2D_DESC td;
    src->GetDesc(&td);
    int w = static_cast<int>(td.Width);
    int h = static_cast<int>(td.Height);

    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    if (!g_win.overlay_visible) { src->Release(); return; }

    ensure_swap_chain(g_win.overlay_swap_chain, g_win.overlay_sw, g_win.overlay_sh,
                      g_win.dcomp_overlay_visual, w, h);
    if (!g_win.overlay_swap_chain) { src->Release(); return; }

    ID3D11Texture2D* bb = nullptr;
    g_win.overlay_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_win.d3d_context->CopyResource(bb, src);
    bb->Release();
    src->Release();

    g_win.overlay_swap_chain->Present(0, 0);
    g_win.dcomp_device->Commit();
}

static void win_overlay_present_software(const CefRenderHandler::RectList&, const void*, int, int) {}

static void win_overlay_resize(int, int, int pw, int ph) {
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    if (!g_win.overlay_swap_chain) return;
    ensure_swap_chain(g_win.overlay_swap_chain, g_win.overlay_sw, g_win.overlay_sh,
                      g_win.dcomp_overlay_visual, pw, ph);
    g_win.dcomp_device->Commit();
}

// =====================================================================
// Overlay visibility + fade
// =====================================================================

static void win_set_overlay_visible(bool visible) {
    {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        g_win.overlay_visible = visible;
        if (!visible && g_win.dcomp_overlay_visual) {
            g_win.dcomp_overlay_visual->SetContent(nullptr);
            if (g_win.overlay_swap_chain) {
                g_win.overlay_swap_chain->Release();
                g_win.overlay_swap_chain = nullptr;
                g_win.overlay_sw = 0;
                g_win.overlay_sh = 0;
            }
            g_win.dcomp_device->Commit();
        }
    }

    // Route keyboard focus to the newly-active browser. Without this, CEF
    // thinks the just-activated browser has no window focus, so text inputs
    // don't show a caret and focus rings don't render. Matches the "active
    // tab" semantics: only one browser at a time holds focus.
    auto main = g_web_browser ? g_web_browser->browser() : nullptr;
    auto ovl  = g_overlay_browser ? g_overlay_browser->browser() : nullptr;
    if (visible) {
        if (main) main->GetHost()->SetFocus(false);
        if (ovl)  ovl->GetHost()->SetFocus(true);
    } else {
        if (ovl)  ovl->GetHost()->SetFocus(false);
        if (main) main->GetHost()->SetFocus(true);
    }
}

// Animate overlay opacity from 1.0 to 0.0 over fade_sec, then hide.
// Runs on a detached thread -- finite UI animation.
static void win_fade_overlay(float fade_sec,
                             std::function<void()> on_fade_start,
                             std::function<void()> on_complete) {
    if (!g_win.dcomp_overlay_visual) {
        win_set_overlay_visible(false);
        if (on_fade_start) on_fade_start();
        if (on_complete) on_complete();
        return;
    }

    std::thread([fade_sec,
                 on_fade_start = std::move(on_fade_start),
                 on_complete = std::move(on_complete)]() {
        if (on_fade_start) on_fade_start();

        int fps = g_display_hz.load(std::memory_order_relaxed);
        int total_frames = static_cast<int>(fade_sec * fps);
        if (total_frames < 1) total_frames = 1;
        auto frame_duration = std::chrono::microseconds(1000000 / fps);

        for (int i = 1; i <= total_frames; i++) {
            float t = static_cast<float>(i) / total_frames;
            float opacity = 1.0f - t;

            {
                std::lock_guard<std::mutex> lock(g_win.surface_mtx);
                if (!g_win.overlay_visible || !g_win.dcomp_overlay_visual) break;
                g_win.dcomp_overlay_effect->SetOpacity(opacity);
                g_win.dcomp_device->Commit();
            }
            std::this_thread::sleep_for(frame_duration);
        }

        win_set_overlay_visible(false);

        // Reset opacity for next show
        {
            std::lock_guard<std::mutex> lock(g_win.surface_mtx);
            if (g_win.dcomp_overlay_visual) {
                g_win.dcomp_overlay_effect->SetOpacity(1.0f);
                g_win.dcomp_device->Commit();
            }
        }
        if (on_complete) on_complete();
    }).detach();
}

// =====================================================================
// Resize + fullscreen transitions
// =====================================================================

static void update_surface_size_locked(int lw, int lh, int pw, int ph) {
    if (g_win.transitioning) {
        g_win.pending_lw = lw;
        g_win.pending_lh = lh;
    }
    // For DComp, the swap chain sizes to match CEF's buffer, not the window.
    // We just track mpv's physical size for oversized-buffer rejection.
    g_win.mpv_pw = pw;
    g_win.mpv_ph = ph;
}

static void win_resize(int lw, int lh, int pw, int ph) {
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    update_surface_size_locked(lw, lh, pw, ph);
}

static void win_begin_transition_locked() {
    g_win.transitioning = true;
    g_win.transition_pw = g_win.mpv_pw;
    g_win.transition_ph = g_win.mpv_ph;
    g_win.pending_lw = 0;
    g_win.pending_lh = 0;

    // Detach main visual content to avoid stale frames
    if (g_win.dcomp_main_visual) {
        g_win.dcomp_main_visual->SetContent(nullptr);
        if (g_win.main_swap_chain) {
            g_win.main_swap_chain->Release();
            g_win.main_swap_chain = nullptr;
            g_win.main_sw = 0;
            g_win.main_sh = 0;
        }
        g_win.dcomp_device->Commit();
    }
}

static void win_end_transition_locked() {
    g_win.transitioning = false;
    g_win.expected_w = 0;
    g_win.expected_h = 0;
    g_win.pending_lw = 0;
    g_win.pending_lh = 0;
}

static void win_begin_transition() {
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    win_begin_transition_locked();
}

static void win_end_transition() {
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    win_end_transition_locked();
}

static bool win_in_transition() {
    return g_win.transitioning;
}

static void win_set_expected_size(int w, int h) {
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    if (g_win.transitioning && w == g_win.transition_pw && h == g_win.transition_ph)
        return;
    g_win.expected_w = w;
    g_win.expected_h = h;
}

// =====================================================================
// Fullscreen
// =====================================================================

static void win_set_fullscreen(bool fullscreen) {
    if (!g_mpv.IsValid()) return;
    bool current = false;
    if (g_mpv.GetFullscreen(current) >= 0) {
        if (current == fullscreen) return;
    }
    {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        win_begin_transition_locked();
    }
    g_mpv.SetFullscreen(fullscreen);
}

static void win_toggle_fullscreen() {
    {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        win_begin_transition_locked();
    }
    if (g_mpv.IsValid()) {
        g_mpv.ToggleFullscreen();
    }
}

// =====================================================================
// Scale + content size
// =====================================================================

static float win_get_scale() {
    if (g_mpv.IsValid()) {
        double scale = 0;
        if (g_mpv.GetDisplayScale(scale) >= 0 && scale > 0) {
            g_win.cached_scale = static_cast<float>(scale);
            return g_win.cached_scale;
        }
    }
    if (g_win.cached_scale > 0) return g_win.cached_scale;
    // Pre-mpv (e.g. default-geometry sizing at startup): ask the OS directly.
    UINT dpi = GetDpiForSystem();
    if (dpi > 0) return static_cast<float>(dpi) / 96.0f;
    return 1.0f;
}

// =====================================================================
// Input thread: transparent child HWND -> CEF events
// =====================================================================

static void win_set_idle_inhibit(IdleInhibitLevel level) {
    UINT flags = ES_CONTINUOUS;
    switch (level) {
    case IdleInhibitLevel::Display:
        flags |= ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED;
        break;
    case IdleInhibitLevel::System:
        flags |= ES_SYSTEM_REQUIRED;
        break;
    case IdleInhibitLevel::None:
        // ES_CONTINUOUS alone releases the inhibit
        break;
    }
    SetThreadExecutionState(flags);
}

// Monitor mpv's HWND for size/fullscreen changes.
static HHOOK g_wndproc_hook = nullptr;

static LRESULT CALLBACK mpv_wndproc_hook(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode >= 0) {
        auto* msg = reinterpret_cast<CWPSTRUCT*>(lp);
        if (msg->hwnd == g_win.mpv_hwnd) {
            if (msg->message == WM_SIZE && msg->wParam != SIZE_MINIMIZED) {
                int pw = LOWORD(msg->lParam);
                int ph = HIWORD(msg->lParam);
                if (pw > 0 && ph > 0) {
                    input::windows::resize_to_parent(pw, ph);

                    float scale = g_win.cached_scale > 0 ? g_win.cached_scale : 1.0f;
                    int lw = static_cast<int>(pw / scale);
                    int lh = static_cast<int>(ph / scale);

                    // Detect fullscreen change via window style
                    LONG_PTR style = GetWindowLongPtr(g_win.mpv_hwnd, GWL_STYLE);
                    bool fs = !(style & WS_OVERLAPPEDWINDOW);

                    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
                    if (fs != g_win.was_fullscreen) {
                        if (!g_win.transitioning)
                            win_begin_transition_locked();
                        else
                            win_end_transition_locked();
                        g_win.was_fullscreen = fs;
                    } else if (g_win.transitioning) {
                        win_end_transition_locked();
                    }
                    update_surface_size_locked(lw, lh, pw, ph);
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

static void win_early_init() {
    // Nothing needed on Windows before mpv starts
}

static bool win_init(mpv_handle* mpv) {
    // Get HWND from mpv
    int64_t wid = 0;
    if (g_mpv.GetWindowId(wid) < 0 || !wid) {
        LOG_ERROR(LOG_PLATFORM, "Failed to get window-id from mpv");
        return false;
    }
    g_win.mpv_hwnd = reinterpret_cast<HWND>(wid);

    // Get initial scale
    win_get_scale();

    // Enable DWM transparency so DComp visuals with premultiplied alpha work
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_win.mpv_hwnd, &margins);

    if (!init_d3d()) return false;
    if (!init_dcomp()) return false;

    // Seed was_fullscreen before installing the hook so the first WM_SIZE
    // doesn't start a spurious transition if already fullscreen.
    {
        LONG_PTR style = GetWindowLongPtr(g_win.mpv_hwnd, GWL_STYLE);
        g_win.was_fullscreen = !(style & WS_OVERLAPPEDWINDOW);
    }

    // Install hook to monitor mpv's HWND for size/fullscreen/close
    DWORD mpv_tid = GetWindowThreadProcessId(g_win.mpv_hwnd, nullptr);
    g_wndproc_hook = SetWindowsHookEx(WH_CALLWNDPROC, mpv_wndproc_hook,
        nullptr, mpv_tid);

    // Start input thread (body lives in input::windows::run_input_thread).
    // The thread owns its own child HWND, cursor state, and WndProc; it
    // runs a Windows message loop until we post WM_QUIT in cleanup.
    HWND mpv_hwnd = g_win.mpv_hwnd;
    g_win.input_thread = std::thread([mpv_hwnd]() {
        input::windows::run_input_thread(mpv_hwnd);
    });

    LOG_INFO(LOG_PLATFORM, "Windows DirectComposition compositor initialized");
    return true;
}

static void win_cleanup() {
    // Signal input thread to quit
    input::windows::stop_input_thread();
    if (g_win.input_thread.joinable())
        g_win.input_thread.join();
    if (g_wndproc_hook) { UnhookWindowsHookEx(g_wndproc_hook); g_wndproc_hook = nullptr; }

    // Release swap chains
    if (g_win.main_swap_chain) { g_win.main_swap_chain->Release(); g_win.main_swap_chain = nullptr; }
    if (g_win.overlay_swap_chain) { g_win.overlay_swap_chain->Release(); g_win.overlay_swap_chain = nullptr; }

    // Release DComp
    if (g_win.dcomp_overlay_effect) { g_win.dcomp_overlay_effect->Release(); g_win.dcomp_overlay_effect = nullptr; }
    if (g_win.dcomp_overlay_visual) { g_win.dcomp_overlay_visual->Release(); g_win.dcomp_overlay_visual = nullptr; }
    if (g_win.dcomp_main_visual) { g_win.dcomp_main_visual->Release(); g_win.dcomp_main_visual = nullptr; }
    if (g_win.dcomp_root) { g_win.dcomp_root->Release(); g_win.dcomp_root = nullptr; }
    if (g_win.dcomp_target) { g_win.dcomp_target->Release(); g_win.dcomp_target = nullptr; }
    if (g_win.dcomp_device) { g_win.dcomp_device->Release(); g_win.dcomp_device = nullptr; }

    // Release D3D11
    if (g_win.dxgi_factory) { g_win.dxgi_factory->Release(); g_win.dxgi_factory = nullptr; }
    if (g_win.d3d_context) { g_win.d3d_context->Release(); g_win.d3d_context = nullptr; }
    if (g_win.d3d_device) { g_win.d3d_device->Release(); g_win.d3d_device = nullptr; }

    g_win.mpv_hwnd = nullptr;
}

static void win_pump() {
    // Input is handled by the dedicated input thread's message loop
}

static void win_set_titlebar_color(uint8_t, uint8_t, uint8_t) {
    // No-op on Windows (DWM handles titlebar appearance)
}

// =====================================================================
// Clipboard (Win32 CF_UNICODETEXT) — read only; writes go through CEF's
// own frame->Copy() path which works correctly on Windows.
// =====================================================================

static void win_clipboard_read_text_async(std::function<void(std::string)> on_done) {
    if (!on_done) return;
    // Win32 clipboard is synchronous; fire the callback inline.
    std::string result;
    if (OpenClipboard(nullptr)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            auto* wbuf = static_cast<const wchar_t*>(GlobalLock(h));
            if (wbuf) {
                int bytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
                if (bytes > 1) {  // includes terminator
                    result.resize(bytes - 1);
                    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, result.data(), bytes, nullptr, nullptr);
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    on_done(std::move(result));
}

static void win_open_external_url(const std::string& url) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url.data(), (int)url.size(), nullptr, 0);
    if (wlen <= 0) return;
    std::wstring wurl(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.data(), (int)url.size(), wurl.data(), wlen);
    HINSTANCE r = ShellExecuteW(nullptr, L"open", wurl.c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32)
        LOG_ERROR(LOG_PLATFORM, "ShellExecuteW failed ({}): {}", (INT_PTR)r, url);
}

// Query window position relative to the monitor's working area (excludes
// taskbar), in physical pixels. Matches mpv's --geometry +X+Y coordinate
// system on Windows (vo_calc_window_geometry uses the working area).
static bool win_query_window_position(int* x, int* y) {
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
// unset axis (mpv's own centering misbehaves when we override --geometry's
// wh but leave xy unset).
static void win_clamp_window_geometry(int* w, int* h, int* x, int* y) {
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

Platform make_windows_platform() {
    return Platform{
        .display = DisplayBackend::Windows,
        .early_init = win_early_init,
        .init = win_init,
        .cleanup = win_cleanup,
        .present = win_present,
        .present_software = win_present_software,
        .resize = win_resize,
        .overlay_present = win_overlay_present,
        .overlay_present_software = win_overlay_present_software,
        .overlay_resize = win_overlay_resize,
        .set_overlay_visible = win_set_overlay_visible,
        .popup_show = [](int, int, int, int) {},
        .popup_hide = []() {},
        .popup_present = [](const CefAcceleratedPaintInfo&, int, int) {},
        .popup_present_software = [](const void*, int, int, int, int) {},
        .try_native_popup_menu = [](int, int, int, int,
                                    const std::vector<std::string>&, int,
                                    std::function<void(int)>) { return false; },
        .fade_overlay = win_fade_overlay,
        .set_fullscreen = win_set_fullscreen,
        .toggle_fullscreen = win_toggle_fullscreen,
        .begin_transition = win_begin_transition,
        .end_transition = win_end_transition,
        .in_transition = win_in_transition,
        .set_expected_size = win_set_expected_size,
        .get_scale = win_get_scale,
        .query_window_position = win_query_window_position,
        .clamp_window_geometry = win_clamp_window_geometry,
        .pump = win_pump,
        .set_cursor = input::windows::set_cursor,
        .set_idle_inhibit = win_set_idle_inhibit,
        .set_titlebar_color = win_set_titlebar_color,
        .clipboard_read_text_async = win_clipboard_read_text_async,
        .open_external_url = win_open_external_url,
    };
}

#endif // _WIN32
