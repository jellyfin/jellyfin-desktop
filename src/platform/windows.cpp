#ifdef _WIN32
// platform_windows.cpp — Windows platform layer.
// D3D11 + DirectComposition composites CEF shared textures (main + overlay)
// onto mpv's HWND. A transparent child HWND captures input for CEF.

#include "platform/platform.h"
#include "common.h"
#include "input/input_windows.h"
#include "logging.h"
#include "mpv/event.h"

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
#include <vector>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

// =====================================================================
// Windows state (file-static)
// =====================================================================

// Per-CefLayer surface: its own composition swap chain + DComp visual.
// Stacking is managed by win_restack rebuilding the child-list under
// dcomp_root in the order supplied by Browsers.
struct PlatformSurface {
    IDXGISwapChain1* swap_chain = nullptr;
    IDCompositionVisual* visual = nullptr;
    IDCompositionEffectGroup* effect = nullptr;  // per-surface fade effect
    int sw = 0, sh = 0;       // swap chain backing-buffer size
    bool visible = true;      // detach content when false
    bool in_tree = false;     // whether visual is currently a child of dcomp_root
    std::atomic<bool> fading{false};

    // Per-surface popup. popup_visual is a child of visual, so popup is
    // automatically composited above this surface's main content.
    IDCompositionVisual* popup_visual = nullptr;
    IDXGISwapChain1* popup_swap_chain = nullptr;
    int popup_sw = 0, popup_sh = 0;
    bool popup_visible = false;
};

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

    // All live PlatformSurfaces and the current stack order (bottom -> top).
    // `stack` is rebuilt by win_restack; `live` mirrors alloc/free.
    std::vector<PlatformSurface*> live;
    std::vector<PlatformSurface*> stack;
    // Bottom-most surface; receives fullscreen-transition frame-drop and
    // content detach. Tracked as stack.front() after restack, or the first
    // allocated surface as a pre-restack fallback.
    PlatformSurface* main_surface = nullptr;

    // Window state
    float cached_scale = 1.0f;
    int mpv_pw = 0, mpv_ph = 0;  // mpv's current physical size

    // Fullscreen transition
    int expected_w = 0, expected_h = 0;
    int transition_pw = 0, transition_ph = 0;
    int pending_lw = 0, pending_lh = 0;
    bool transitioning = false;
    bool was_fullscreen = false;
    bool was_minimized = false;
    bool restore_maximized_on_unfullscreen = false;

    // Input thread (body lives in input::windows::run_input_thread)
    std::thread input_thread;
};

static WinState g_win;

static void win_begin_transition_locked();
static void win_end_transition_locked();

static bool win_is_fullscreen_style(LONG_PTR style) {
    return (style & WS_CAPTION) == 0 && (style & WS_THICKFRAME) == 0;
}

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

    // Visual tree: root holds per-surface visuals (added by win_alloc_surface
    // / reordered by win_restack). Each surface owns its own popup visual
    // as a child of its main visual.
    g_win.dcomp_device->CreateVisual(&g_win.dcomp_root);
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
// Generic per-surface ops
// =====================================================================

// Shared accelerated-present helper: copies the CEF shared texture into the
// surface's swap chain, sizing 1:1 to the CEF buffer (never stretch).
// Caller must NOT hold surface_mtx; this function takes it.
static void present_to_surface_locked(PlatformSurface* s, ID3D11Texture2D* src,
                                      int w, int h) {
    ensure_swap_chain(s->swap_chain, s->sw, s->sh, s->visual, w, h);
    if (!s->swap_chain) return;

    ID3D11Texture2D* bb = nullptr;
    HRESULT hr = s->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (FAILED(hr) || !bb) {
        LOG_ERROR(LOG_PLATFORM, "swap_chain->GetBuffer failed: 0x{:08x}", hr);
        return;
    }
    g_win.d3d_context->CopyResource(bb, src);
    bb->Release();
    s->swap_chain->Present(0, 0);
    g_win.dcomp_device->Commit();
}

static void win_surface_present(PlatformSurface* s, const CefAcceleratedPaintInfo& info) {
    if (!s) return;
    HANDLE handle = info.shared_texture_handle;
    if (!handle) return;

    ID3D11Texture2D* src = nullptr;
    HRESULT hr = g_win.d3d_device->OpenSharedResource1(handle,
        __uuidof(ID3D11Texture2D), (void**)&src);
    if (FAILED(hr) || !src) {
        LOG_ERROR(LOG_PLATFORM, "OpenSharedResource1 failed: 0x{:08x}", hr);
        return;
    }

    D3D11_TEXTURE2D_DESC td;
    src->GetDesc(&td);
    int w = static_cast<int>(td.Width);
    int h = static_cast<int>(td.Height);

    std::lock_guard<std::mutex> lock(g_win.surface_mtx);

    // Transition logic applies only to the bottom-most ("main") surface:
    // mpv resizes the HWND on fullscreen toggle, so we drop CEF frames that
    // still match the pre-transition size until CEF catches up.
    bool is_main = (s == g_win.main_surface);
    if (is_main && g_win.transitioning) {
        if (g_win.expected_w <= 0 || (w == g_win.transition_pw && h == g_win.transition_ph)) {
            src->Release();
            return;
        }
        // New frame matches expected size -- end transition
        win_end_transition_locked();
    }

    // Drop oversized buffers (main only — overlay/about can legitimately be
    // larger than the window during resize churn historically; preserve old
    // semantics where only the main path enforced this).
    if (is_main && g_win.mpv_pw > 0 && (w > g_win.mpv_pw + 2 || h > g_win.mpv_ph + 2)) {
        src->Release();
        return;
    }

    if (!s->visible) { src->Release(); return; }

    present_to_surface_locked(s, src, w, h);
    src->Release();
}

// Software fallback: Windows is shared-textures-only in practice.
// Kept as a no-op to match prior overlay/about behavior; the main path
// historically also no-op'd here.
static void win_surface_present_software(PlatformSurface*,
                                         const CefRenderHandler::RectList&,
                                         const void*, int, int) {
}

static void win_surface_resize(PlatformSurface* s, int /*lw*/, int /*lh*/, int pw, int ph) {
    if (!s || pw <= 0 || ph <= 0) return;
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    // CEF presents at its own buffer size and ensure_swap_chain rebinds at
    // present time, so we only adjust the swap chain if it already exists
    // (matches prior overlay/about resize semantics). This avoids forcing
    // a stale physical size between a window resize and the next CEF paint.
    if (!s->swap_chain) return;
    ensure_swap_chain(s->swap_chain, s->sw, s->sh, s->visual, pw, ph);
    g_win.dcomp_device->Commit();
}

static void win_surface_set_visible(PlatformSurface* s, bool visible) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    if (s->visible == visible) return;
    s->visible = visible;
    if (!s->visual) return;

    if (!visible) {
        // Detach content and drop the swap chain so we don't display a
        // stale frame when the surface is shown again at a different size.
        s->visual->SetContent(nullptr);
        if (s->swap_chain) {
            s->swap_chain->Release();
            s->swap_chain = nullptr;
            s->sw = 0;
            s->sh = 0;
        }
    } else {
        // Content will be re-bound on next ensure_swap_chain via
        // surface_present.
    }
    g_win.dcomp_device->Commit();
}

// Animate the surface's effect-group opacity from 1.0 -> 0.0 over fade_sec,
// then hide. Runs on a detached thread — finite UI animation, no polling
// (frame-paced sleep matches the display refresh).
static void win_fade_surface(PlatformSurface* s, float fade_sec,
                             std::function<void()> on_fade_start,
                             std::function<void()> on_complete) {
    if (!s || !s->visual || !s->effect) {
        if (on_fade_start) on_fade_start();
        if (s) win_surface_set_visible(s, false);
        if (on_complete) on_complete();
        return;
    }

    double fps = mpv::display_hz();
    if (fps <= 0) {
        if (on_fade_start) on_fade_start();
        win_surface_set_visible(s, false);
        if (on_complete) on_complete();
        return;
    }

    s->fading.store(true);
    std::thread([s, fade_sec, fps,
                 on_fade_start = std::move(on_fade_start),
                 on_complete = std::move(on_complete)]() {
        if (on_fade_start) on_fade_start();

        int total_frames = static_cast<int>(fade_sec * fps);
        if (total_frames < 1) total_frames = 1;
        auto frame_duration = std::chrono::microseconds(static_cast<int64_t>(1e6 / fps));

        for (int i = 1; i <= total_frames; i++) {
            float t = static_cast<float>(i) / total_frames;
            float opacity = 1.0f - t;
            {
                std::lock_guard<std::mutex> lock(g_win.surface_mtx);
                if (!s->visible || !s->visual || !s->effect) break;
                s->effect->SetOpacity(opacity);
                g_win.dcomp_device->Commit();
            }
            std::this_thread::sleep_for(frame_duration);
        }

        win_surface_set_visible(s, false);

        // Reset opacity for next show.
        {
            std::lock_guard<std::mutex> lock(g_win.surface_mtx);
            if (s->visual && s->effect) {
                s->effect->SetOpacity(1.0f);
                g_win.dcomp_device->Commit();
            }
        }
        s->fading.store(false);
        if (on_complete) on_complete();
    }).detach();
}

// =====================================================================
// Surface lifecycle + stacking
// =====================================================================

static PlatformSurface* win_alloc_surface() {
    auto* s = new PlatformSurface;

    std::lock_guard<std::mutex> lock(g_win.surface_mtx);

    HRESULT hr = g_win.dcomp_device->CreateVisual(&s->visual);
    if (FAILED(hr) || !s->visual) {
        LOG_ERROR(LOG_PLATFORM, "CreateVisual failed: 0x{:08x}", hr);
        delete s;
        return nullptr;
    }
    hr = g_win.dcomp_device->CreateEffectGroup(&s->effect);
    if (FAILED(hr) || !s->effect) {
        LOG_ERROR(LOG_PLATFORM, "CreateEffectGroup failed: 0x{:08x}", hr);
        s->visual->Release();
        delete s;
        return nullptr;
    }
    s->visual->SetEffect(s->effect);

    // Per-surface popup visual nested under the main visual so popup
    // composites above its surface automatically; restack only reorders
    // main visuals.
    hr = g_win.dcomp_device->CreateVisual(&s->popup_visual);
    if (FAILED(hr) || !s->popup_visual) {
        LOG_ERROR(LOG_PLATFORM, "CreateVisual(popup) failed: 0x{:08x}", hr);
    } else {
        s->visual->AddVisual(s->popup_visual, TRUE, nullptr);
    }

    // Add to the tree at the top; restack() will rebuild order at the
    // next stacking change.
    hr = g_win.dcomp_root->AddVisual(s->visual, TRUE, nullptr);
    if (FAILED(hr))
        LOG_ERROR(LOG_PLATFORM, "AddVisual failed: 0x{:08x}", hr);
    else
        s->in_tree = true;

    g_win.live.push_back(s);
    if (!g_win.main_surface) g_win.main_surface = s;

    // Defer first size until surface_resize / first present.
    g_win.dcomp_device->Commit();
    return s;
}

static void win_free_surface(PlatformSurface* s) {
    if (!s) return;
    // Wait for any in-flight fade thread to finish touching this surface.
    // It runs frame-paced and is bounded; no polling needed beyond a yield.
    while (s->fading.load()) std::this_thread::yield();

    std::lock_guard<std::mutex> lock(g_win.surface_mtx);

    auto it = std::find(g_win.live.begin(), g_win.live.end(), s);
    if (it != g_win.live.end()) g_win.live.erase(it);
    auto sit = std::find(g_win.stack.begin(), g_win.stack.end(), s);
    if (sit != g_win.stack.end()) g_win.stack.erase(sit);
    if (g_win.main_surface == s)
        g_win.main_surface = g_win.stack.empty() ? (g_win.live.empty() ? nullptr : g_win.live.front())
                                                 : g_win.stack.front();

    if (s->popup_visual) {
        if (s->visual) s->visual->RemoveVisual(s->popup_visual);
        s->popup_visual->SetContent(nullptr);
        s->popup_visual->Release();
        s->popup_visual = nullptr;
    }
    if (s->popup_swap_chain) { s->popup_swap_chain->Release(); s->popup_swap_chain = nullptr; }
    if (s->visual) {
        if (s->in_tree) g_win.dcomp_root->RemoveVisual(s->visual);
        s->visual->SetContent(nullptr);
        s->visual->Release();
        s->visual = nullptr;
    }
    if (s->effect) { s->effect->Release(); s->effect = nullptr; }
    if (s->swap_chain) { s->swap_chain->Release(); s->swap_chain = nullptr; }

    g_win.dcomp_device->Commit();
    delete s;
}

// Rebuild the child-list under dcomp_root in `ordered` order
// (bottom -> top). Each surface's popup visual is nested under its
// main visual so it stays above its surface automatically.
static void win_restack(PlatformSurface* const* ordered, size_t n) {
    if (!g_win.dcomp_root) return;
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);

    // Remove every live surface visual from the tree first so we can
    // rebuild order deterministically.
    for (auto* s : g_win.live) {
        if (s->visual && s->in_tree) {
            g_win.dcomp_root->RemoveVisual(s->visual);
            s->in_tree = false;
        }
    }

    // Re-add in given order: each is placed above the previous (so
    // index 0 ends up bottom-most).
    IDCompositionVisual* prev = nullptr;
    g_win.stack.clear();
    for (size_t i = 0; i < n; i++) {
        PlatformSurface* s = ordered[i];
        if (!s || !s->visual) continue;
        HRESULT hr = prev
            ? g_win.dcomp_root->AddVisual(s->visual, TRUE, prev)
            : g_win.dcomp_root->AddVisual(s->visual, FALSE, nullptr);
        if (FAILED(hr)) {
            LOG_ERROR(LOG_PLATFORM, "restack AddVisual failed: 0x{:08x}", hr);
            continue;
        }
        s->in_tree = true;
        g_win.stack.push_back(s);
        prev = s->visual;
    }
    if (!g_win.stack.empty())
        g_win.main_surface = g_win.stack.front();

    g_win.dcomp_device->Commit();
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

static void win_begin_transition_locked() {
    g_win.transitioning = true;
    g_win.transition_pw = g_win.mpv_pw;
    g_win.transition_ph = g_win.mpv_ph;
    g_win.pending_lw = 0;
    g_win.pending_lh = 0;

    // Detach the bottom-most ("main") surface's content to avoid stale
    // frames while the window is resizing.
    PlatformSurface* s = g_win.main_surface;
    if (s && s->visual) {
        s->visual->SetContent(nullptr);
        if (s->swap_chain) {
            s->swap_chain->Release();
            s->swap_chain = nullptr;
            s->sw = 0;
            s->sh = 0;
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
    if (mpv::fullscreen() == fullscreen) {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        if (g_win.transitioning && fullscreen == g_win.was_fullscreen)
            win_end_transition_locked();
        return;
    }

    if (fullscreen) {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        g_win.restore_maximized_on_unfullscreen = IsZoomed(g_win.mpv_hwnd) != 0;
    }

    bool should_restore_maximized = false;
    if (!fullscreen) {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        should_restore_maximized = g_win.restore_maximized_on_unfullscreen;
        g_win.restore_maximized_on_unfullscreen = false;
    }

    bool is_minimized_now = IsMinimized(g_win.mpv_hwnd) != 0;
    if (!is_minimized_now) {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        win_begin_transition_locked();
    }

    if (fullscreen)
        g_mpv.SetWindowMinimized(false);

    g_mpv.SetFullscreen(fullscreen);

    if (!fullscreen && should_restore_maximized)
        g_mpv.SetWindowMaximized(true);
}

static void win_toggle_fullscreen() {
    if (!g_mpv.IsValid()) return;
    bool target_fullscreen = !mpv::fullscreen();

    if (target_fullscreen) {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        g_win.restore_maximized_on_unfullscreen = IsZoomed(g_win.mpv_hwnd) != 0;
    }

    bool should_restore_maximized = false;
    if (!target_fullscreen) {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        should_restore_maximized = g_win.restore_maximized_on_unfullscreen;
        g_win.restore_maximized_on_unfullscreen = false;
    }

    // Only start a transition if the window is not minimized.
    bool is_minimized_now = IsMinimized(g_win.mpv_hwnd) != 0;
    if (!is_minimized_now) {
        std::lock_guard<std::mutex> lock(g_win.surface_mtx);
        win_begin_transition_locked();
    }

    if (target_fullscreen)
        g_mpv.SetWindowMinimized(false);

    g_mpv.ToggleFullscreen();

    if (!target_fullscreen && should_restore_maximized)
        g_mpv.SetWindowMaximized(true);
}

// =====================================================================
// Scale + content size
// =====================================================================

static float win_get_scale() {
    double scale = mpv::display_scale();
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

// =====================================================================
// Input thread: transparent child HWND -> CEF events
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

static void win_set_idle_inhibit(IdleInhibitLevel level) {
    CefPostTask(TID_UI, new FnTask([level]() {
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
    }));
}

// Monitor mpv's HWND for size/fullscreen changes.
static HHOOK g_wndproc_hook = nullptr;

static LRESULT CALLBACK mpv_wndproc_hook(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode >= 0) {
        auto* msg = reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (msg->hwnd == g_win.mpv_hwnd) {
            if (msg->message == WM_SIZE) {
                if (msg->wParam == SIZE_MINIMIZED) {
                    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
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

                    // Detect fullscreen via style bits mpv uses on Windows.
                    LONG_PTR style = GetWindowLongPtr(g_win.mpv_hwnd, GWL_STYLE);
                    bool fs = win_is_fullscreen_style(style);

                    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
                    bool recovering_from_minimize = g_win.was_minimized;
                    if (recovering_from_minimize) {
                        g_win.was_minimized = false;
                        g_win.was_fullscreen = fs;
                        if (g_win.transitioning)
                            win_end_transition_locked();
                    } else if (fs != g_win.was_fullscreen) {
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
        g_win.was_fullscreen = win_is_fullscreen_style(style);
    }

    // Install hook to monitor mpv's HWND for size/fullscreen/close
    DWORD mpv_tid = GetWindowThreadProcessId(g_win.mpv_hwnd, nullptr);
    g_wndproc_hook = SetWindowsHookEx(WH_CALLWNDPROCRET, mpv_wndproc_hook,
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

    // Release any stragglers — Browsers should normally free its surfaces
    // before cleanup, but be defensive.
    for (auto* s : g_win.live) {
        if (s->popup_visual) {
            if (s->visual) s->visual->RemoveVisual(s->popup_visual);
            s->popup_visual->SetContent(nullptr);
            s->popup_visual->Release();
        }
        if (s->popup_swap_chain) s->popup_swap_chain->Release();
        if (s->visual) {
            if (s->in_tree && g_win.dcomp_root) g_win.dcomp_root->RemoveVisual(s->visual);
            s->visual->SetContent(nullptr);
            s->visual->Release();
        }
        if (s->effect) s->effect->Release();
        if (s->swap_chain) s->swap_chain->Release();
        delete s;
    }
    g_win.live.clear();
    g_win.stack.clear();
    g_win.main_surface = nullptr;

    // Release DComp
    if (g_win.dcomp_root)            { g_win.dcomp_root->Release();            g_win.dcomp_root            = nullptr; }
    if (g_win.dcomp_target)          { g_win.dcomp_target->Release();          g_win.dcomp_target          = nullptr; }
    if (g_win.dcomp_device)          { g_win.dcomp_device->Release();          g_win.dcomp_device          = nullptr; }

    // Release D3D11
    if (g_win.dxgi_factory) { g_win.dxgi_factory->Release(); g_win.dxgi_factory = nullptr; }
    if (g_win.d3d_context) { g_win.d3d_context->Release(); g_win.d3d_context = nullptr; }
    if (g_win.d3d_device) { g_win.d3d_device->Release(); g_win.d3d_device = nullptr; }

    g_win.mpv_hwnd = nullptr;
}

static void win_pump() {
    // Input is handled by the dedicated input thread's message loop
}

static void win_set_theme_color(const Color&) {
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

static void win_popup_show(PlatformSurface* s, const Platform::PopupRequest& req) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    s->popup_visible = true;
    if (!s->popup_visual) return;
    float scale = win_get_scale();
    s->popup_visual->SetOffsetX(static_cast<float>(req.x) * scale);
    s->popup_visual->SetOffsetY(static_cast<float>(req.y) * scale);
}

static void win_popup_hide(PlatformSurface* s) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    s->popup_visible = false;
    if (!s->popup_visual) return;

    s->popup_visual->SetContent(nullptr);
    if (s->popup_swap_chain) {
        s->popup_swap_chain->Release();
        s->popup_swap_chain = nullptr;
        s->popup_sw = 0;
        s->popup_sh = 0;
    }
    g_win.dcomp_device->Commit();
}

static void win_popup_present(PlatformSurface* s, const CefAcceleratedPaintInfo& info,
                              int /*lw*/, int /*lh*/) {
    if (!s) return;
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
    if (!s->popup_visible || !s->popup_visual) { src->Release(); return; }

    ensure_swap_chain(s->popup_swap_chain, s->popup_sw, s->popup_sh,
                      s->popup_visual, w, h);
    if (!s->popup_swap_chain) { src->Release(); return; }

    ID3D11Texture2D* bb = nullptr;
    s->popup_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_win.d3d_context->CopyResource(bb, src);
    bb->Release();
    src->Release();

    s->popup_swap_chain->Present(0, 0);
    g_win.dcomp_device->Commit();
}

static void win_popup_present_software(PlatformSurface* s, const void* buffer,
                                       int pw, int ph,
                                       int /*lw*/, int /*lh*/) {
    if (!s || !buffer || pw <= 0 || ph <= 0) return;

    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    if (!s->popup_visible || !s->popup_visual || !g_win.d3d_device) return;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width             = static_cast<UINT>(pw);
    desc.Height            = static_cast<UINT>(ph);
    desc.MipLevels         = 1;
    desc.ArraySize         = 1;
    desc.Format            = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count  = 1;
    desc.Usage             = D3D11_USAGE_DEFAULT;
    desc.BindFlags         = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem     = buffer;
    init.SysMemPitch = static_cast<UINT>(pw) * 4;

    ID3D11Texture2D* src = nullptr;
    if (FAILED(g_win.d3d_device->CreateTexture2D(&desc, &init, &src))) return;

    ensure_swap_chain(s->popup_swap_chain, s->popup_sw, s->popup_sh,
                      s->popup_visual, pw, ph);
    if (!s->popup_swap_chain) { src->Release(); return; }

    ID3D11Texture2D* bb = nullptr;
    s->popup_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_win.d3d_context->CopyResource(bb, src);
    bb->Release();
    src->Release();

    s->popup_swap_chain->Present(0, 0);
    g_win.dcomp_device->Commit();
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
        .post_window_cleanup = nullptr,
        .alloc_surface = win_alloc_surface,
        .free_surface = win_free_surface,
        .surface_present = win_surface_present,
        .surface_present_software = win_surface_present_software,
        .surface_resize = win_surface_resize,
        .surface_set_visible = win_surface_set_visible,
        .restack = win_restack,
        .fade_surface = win_fade_surface,
        .popup_show             = win_popup_show,
        .popup_hide             = win_popup_hide,
        .popup_present          = win_popup_present,
        .popup_present_software = win_popup_present_software,
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
        .set_theme_color = win_set_theme_color,
        .clipboard_read_text_async = win_clipboard_read_text_async,
        .open_external_url = win_open_external_url,
    };
}

#endif // _WIN32
