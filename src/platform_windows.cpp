#ifdef _WIN32
// platform_windows.cpp — Windows platform layer.
// D3D11 + DirectComposition composites CEF shared textures (main + overlay)
// onto mpv's HWND. A transparent child HWND captures input for CEF.

#include "platform.h"
#include "common.h"
#include "cef/cef_client.h"
#include "logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>

#include <mutex>
#include <thread>
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

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

    // Input stack: always route to top.
    CefRefPtr<CefBrowser> input_stack[2];
    int input_top = -1;

    // Window state
    float cached_scale = 1.0f;
    int mpv_pw = 0, mpv_ph = 0;  // mpv's current physical size

    // Fullscreen transition
    int expected_w = 0, expected_h = 0;
    int transition_pw = 0, transition_ph = 0;
    int pending_lw = 0, pending_lh = 0;
    bool transitioning = false;
    bool was_fullscreen = false;

    // Input thread
    std::thread input_thread;
    DWORD input_thread_id = 0;
    HWND input_hwnd = nullptr;
};

static WinState g_win;

static void input_thread_func();
static void win_begin_transition_locked();
static void win_end_transition_locked();

static CefRefPtr<CefBrowser> active_browser() {
    if (g_win.input_top >= 0)
        return g_win.input_stack[g_win.input_top];
    return nullptr;
}

void platform_push_input(CefRefPtr<CefBrowser> b) {
    if (g_win.input_top < 1) {
        g_win.input_stack[++g_win.input_top] = b;
    }
}

static void pop_input() {
    if (g_win.input_top >= 0) {
        g_win.input_stack[g_win.input_top] = nullptr;
        g_win.input_top--;
    }
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
        LOG_ERROR(LOG_PLATFORM, "D3D11CreateDevice failed: 0x%08lx", hr);
        return false;
    }
    hr = base_device->QueryInterface(__uuidof(ID3D11Device1), (void**)&g_win.d3d_device);
    base_device->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "QueryInterface for ID3D11Device1 failed: 0x%08lx", hr);
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
        LOG_ERROR(LOG_PLATFORM, "DCompositionCreateDevice failed: 0x%08lx", hr);
        return false;
    }

    hr = g_win.dcomp_device->CreateTargetForHwnd(g_win.mpv_hwnd, TRUE, &g_win.dcomp_target);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "CreateTargetForHwnd failed: 0x%08lx", hr);
        return false;
    }

    // Visual tree: root -> main (bottom), overlay (top)
    g_win.dcomp_device->CreateVisual(&g_win.dcomp_root);
    g_win.dcomp_device->CreateVisual(&g_win.dcomp_main_visual);
    g_win.dcomp_device->CreateVisual(&g_win.dcomp_overlay_visual);
    g_win.dcomp_device->CreateEffectGroup(&g_win.dcomp_overlay_effect);
    g_win.dcomp_overlay_visual->SetEffect(g_win.dcomp_overlay_effect);

    g_win.dcomp_root->AddVisual(g_win.dcomp_main_visual, TRUE, nullptr);
    g_win.dcomp_root->AddVisual(g_win.dcomp_overlay_visual, FALSE, g_win.dcomp_main_visual);
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
        LOG_ERROR(LOG_PLATFORM, "CreateSwapChainForComposition failed: 0x%08lx", hr);
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

static void win_present_software(const void*, int, int) {
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

static void win_overlay_present_software(const void*, int, int) {}

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
}

// Animate overlay opacity from 1.0 to 0.0 over duration_sec, then hide.
// Runs on a detached thread -- finite UI animation, not a poll loop.
static void win_fade_overlay(float duration_sec) {
    if (!g_win.dcomp_overlay_visual) {
        pop_input();
        win_set_overlay_visible(false);
        return;
    }

    std::thread([duration_sec]() {
        constexpr int fps = 60;
        int total_frames = static_cast<int>(duration_sec * fps);
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

        pop_input();
        win_set_overlay_visible(false);

        // Reset opacity for next show
        {
            std::lock_guard<std::mutex> lock(g_win.surface_mtx);
            if (g_win.dcomp_overlay_visual) {
                g_win.dcomp_overlay_effect->SetOpacity(1.0f);
                g_win.dcomp_device->Commit();
            }
        }
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
    if (!g_mpv.IsValid()) return 1.0f;
    double scale = 0;
    if (g_mpv.GetDisplayScale(scale) >= 0 && scale > 0) {
        g_win.cached_scale = static_cast<float>(scale);
        return g_win.cached_scale;
    }
    return g_win.cached_scale > 0 ? g_win.cached_scale : 1.0f;
}

static bool win_query_logical_content_size(int* w, int* h) {
    if (!g_win.mpv_hwnd) return false;
    RECT rc;
    if (!GetClientRect(g_win.mpv_hwnd, &rc)) return false;
    // Client rect is in physical pixels on Windows; convert to logical
    float scale = g_win.cached_scale > 0 ? g_win.cached_scale : 1.0f;
    *w = static_cast<int>((rc.right - rc.left) / scale);
    *h = static_cast<int>((rc.bottom - rc.top) / scale);
    return *w > 0 && *h > 0;
}

// =====================================================================
// Input thread: transparent child HWND -> CEF events
// =====================================================================

static bool IsKeyDown(WPARAM wp) {
    return (GetKeyState(static_cast<int>(wp)) & 0x8000) != 0;
}

static uint32_t GetCefMouseModifiers(WPARAM wp) {
    uint32_t m = 0;
    if (wp & MK_CONTROL) m |= EVENTFLAG_CONTROL_DOWN;
    if (wp & MK_SHIFT)   m |= EVENTFLAG_SHIFT_DOWN;
    if (IsKeyDown(VK_MENU)) m |= EVENTFLAG_ALT_DOWN;
    if (wp & MK_LBUTTON) m |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (wp & MK_RBUTTON) m |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    if (wp & MK_MBUTTON) m |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    return m;
}

// From cefclient/tests/shared/browser/util_win.cc
static int GetCefKeyboardModifiers(WPARAM wp, LPARAM lp) {
    int m = 0;
    if (IsKeyDown(VK_SHIFT))   m |= EVENTFLAG_SHIFT_DOWN;
    if (IsKeyDown(VK_CONTROL)) m |= EVENTFLAG_CONTROL_DOWN;
    if (IsKeyDown(VK_MENU))    m |= EVENTFLAG_ALT_DOWN;
    if (::GetKeyState(VK_NUMLOCK) & 1) m |= EVENTFLAG_NUM_LOCK_ON;
    if (::GetKeyState(VK_CAPITAL) & 1) m |= EVENTFLAG_CAPS_LOCK_ON;

    switch (wp) {
    case VK_RETURN:
        if ((lp >> 16) & KF_EXTENDED) m |= EVENTFLAG_IS_KEY_PAD;
        break;
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_UP: case VK_DOWN:
    case VK_LEFT: case VK_RIGHT:
        if (!((lp >> 16) & KF_EXTENDED)) m |= EVENTFLAG_IS_KEY_PAD;
        break;
    case VK_NUMLOCK: case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2:
    case VK_NUMPAD3: case VK_NUMPAD4: case VK_NUMPAD5: case VK_NUMPAD6:
    case VK_NUMPAD7: case VK_NUMPAD8: case VK_NUMPAD9:
    case VK_DIVIDE: case VK_MULTIPLY: case VK_SUBTRACT: case VK_ADD:
    case VK_DECIMAL: case VK_CLEAR:
        m |= EVENTFLAG_IS_KEY_PAD;
        break;
    case VK_SHIFT:
        if (IsKeyDown(VK_LSHIFT)) m |= EVENTFLAG_IS_LEFT;
        else if (IsKeyDown(VK_RSHIFT)) m |= EVENTFLAG_IS_RIGHT;
        break;
    case VK_CONTROL:
        if (IsKeyDown(VK_LCONTROL)) m |= EVENTFLAG_IS_LEFT;
        else if (IsKeyDown(VK_RCONTROL)) m |= EVENTFLAG_IS_RIGHT;
        break;
    case VK_MENU:
        if (IsKeyDown(VK_LMENU)) m |= EVENTFLAG_IS_LEFT;
        else if (IsKeyDown(VK_RMENU)) m |= EVENTFLAG_IS_RIGHT;
        break;
    case VK_LWIN: m |= EVENTFLAG_IS_LEFT; break;
    case VK_RWIN: m |= EVENTFLAG_IS_RIGHT; break;
    }
    return m;
}

static cef_mouse_button_type_t msg_to_button(UINT msg) {
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return MBT_LEFT;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return MBT_RIGHT;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return MBT_MIDDLE;
    default: return MBT_LEFT;
    }
}

static bool is_button_up(UINT msg) {
    return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP;
}

static LRESULT CALLBACK input_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    // --- Mouse ---
    case WM_MOUSEMOVE: {
        auto b = active_browser();
        if (!b) break;
        CefMouseEvent e;
        e.x = GET_X_LPARAM(lp);
        e.y = GET_Y_LPARAM(lp);
        e.modifiers = GetCefMouseModifiers(wp);
        b->GetHost()->SendMouseMoveEvent(e, false);
        return 0;
    }
    case WM_MOUSELEAVE: {
        auto b = active_browser();
        if (!b) break;
        CefMouseEvent e;
        e.x = -1; e.y = -1;
        e.modifiers = GetCefMouseModifiers(wp);
        b->GetHost()->SendMouseMoveEvent(e, true);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: {
        auto b = active_browser();
        if (!b) break;
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
            SetFocus(hwnd);
            b->GetHost()->SetFocus(true);
        }
        CefMouseEvent e;
        e.x = GET_X_LPARAM(lp);
        e.y = GET_Y_LPARAM(lp);
        e.modifiers = GetCefMouseModifiers(wp);
        bool up = is_button_up(msg);
        int click_count = (msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
                           msg == WM_MBUTTONDBLCLK) ? 2 : 1;
        b->GetHost()->SendMouseClickEvent(e, msg_to_button(msg), up, click_count);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        auto b = active_browser();
        if (!b) break;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        CefMouseEvent e;
        e.x = pt.x; e.y = pt.y;
        e.modifiers = GetCefMouseModifiers(wp);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        b->GetHost()->SendMouseWheelEvent(e, 0, delta);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        auto b = active_browser();
        if (!b) break;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        CefMouseEvent e;
        e.x = pt.x; e.y = pt.y;
        e.modifiers = GetCefMouseModifiers(wp);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        b->GetHost()->SendMouseWheelEvent(e, delta, 0);
        return 0;
    }

    // --- Keyboard (matches cefclient/browser/osr_window_win.cc exactly) ---
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        // Hotkeys (only when overlay is not visible)
        if (!g_win.overlay_visible) {
            int vk = static_cast<int>(wp);
            if (vk == 'F' || vk == VK_F11) { win_toggle_fullscreen(); return 0; }
            if (vk == 'Q' || vk == VK_ESCAPE) { initiate_shutdown(); return 0; }
        }
    }
    [[fallthrough]];
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR: {
        auto b = active_browser();
        if (!b) break;
        b->GetHost()->SetFocus(true);

        CefKeyEvent event;
        event.windows_key_code = static_cast<int>(wp);
        event.native_key_code = static_cast<int>(lp);
        event.is_system_key = (msg == WM_SYSCHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP);

        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
            event.type = KEYEVENT_RAWKEYDOWN;
        else if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
            event.type = KEYEVENT_KEYUP;
        else
            event.type = KEYEVENT_CHAR;

        event.modifiers = GetCefKeyboardModifiers(wp, lp);
        b->GetHost()->SendKeyEvent(event);
        return 0;
    }

    // --- Focus ---
    case WM_SETFOCUS: {
        auto b = active_browser();
        if (b) b->GetHost()->SetFocus(true);
        return 0;
    }

    // --- Resize: match parent ---
    case WM_SIZE: {
        // Handled by parent monitoring -- nothing needed here
        return 0;
    }

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
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
                    if (g_win.input_hwnd)
                        SetWindowPos(g_win.input_hwnd, nullptr, 0, 0, pw, ph,
                            SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);

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

static void input_thread_func() {
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = input_wndproc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"JellyfinCefInput";
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Child window covering mpv's client area — captures all mouse and
    // keyboard for CEF. AttachThreadInput lets our cross-thread child
    // receive keyboard focus (mpv's wndproc never sees keyboard messages
    // because focus is on our child, not mpv's HWND).
    RECT rc;
    GetClientRect(g_win.mpv_hwnd, &rc);

    g_win.input_hwnd = CreateWindowExW(
        0,
        L"JellyfinCefInput", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        g_win.mpv_hwnd, nullptr, GetModuleHandle(nullptr), nullptr);

    DWORD mpv_tid = GetWindowThreadProcessId(g_win.mpv_hwnd, nullptr);
    AttachThreadInput(GetCurrentThreadId(), mpv_tid, TRUE);
    SetFocus(g_win.input_hwnd);

    // Install hook to monitor mpv's HWND for size/fullscreen/close
    g_wndproc_hook = SetWindowsHookEx(WH_CALLWNDPROC, mpv_wndproc_hook,
        nullptr, mpv_tid);

    // Message loop
    MSG m;
    while (GetMessage(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    if (g_wndproc_hook) { UnhookWindowsHookEx(g_wndproc_hook); g_wndproc_hook = nullptr; }
    if (g_win.input_hwnd) { DestroyWindow(g_win.input_hwnd); g_win.input_hwnd = nullptr; }
    UnregisterClassW(L"JellyfinCefInput", GetModuleHandle(nullptr));
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

    // Start input thread
    g_win.input_thread = std::thread([]() {
        g_win.input_thread_id = GetCurrentThreadId();
        input_thread_func();
    });

    LOG_INFO(LOG_PLATFORM, "Windows DirectComposition compositor initialized");
    return true;
}

static void win_cleanup() {
    // Signal input thread to quit
    if (g_win.input_thread_id)
        PostThreadMessage(g_win.input_thread_id, WM_QUIT, 0, 0);
    if (g_win.input_thread.joinable())
        g_win.input_thread.join();
    g_win.input_thread_id = 0;

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
// make_windows_platform
// =====================================================================

Platform make_windows_platform() {
    return Platform{
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
        .fade_overlay = win_fade_overlay,
        .set_fullscreen = win_set_fullscreen,
        .toggle_fullscreen = win_toggle_fullscreen,
        .begin_transition = win_begin_transition,
        .end_transition = win_end_transition,
        .in_transition = win_in_transition,
        .set_expected_size = win_set_expected_size,
        .get_scale = win_get_scale,
        .query_logical_content_size = win_query_logical_content_size,
        .pump = win_pump,
        .set_cursor = [](cef_cursor_type_t) {},
        .set_titlebar_color = win_set_titlebar_color,
    };
}

#endif // _WIN32
