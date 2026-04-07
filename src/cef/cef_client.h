#pragma once

#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_context_menu_handler.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

class MenuOverlay;

// Interface for input routing
class InputReceiver {
public:
    virtual ~InputReceiver() = default;
    virtual void sendFocus(bool focused) = 0;
    virtual void sendMouseMove(int x, int y, int modifiers) = 0;
    virtual void sendMouseClick(int x, int y, bool down, int button, int clickCount, int modifiers) = 0;
    virtual void sendMouseWheel(int x, int y, float deltaX, float deltaY, int modifiers) = 0;
    virtual void sendKeyEvent(int key, bool down, int modifiers) = 0;
    virtual void sendChar(int charCode, int modifiers) = 0;
    virtual void sendTouch(int id, float x, float y, float radiusX, float radiusY,
                           float pressure, int type, int modifiers) = 0;
    // Edit commands
    virtual void paste(const char* mimeType, const void* data, size_t len) = 0;
    virtual void copy() = 0;
    virtual void cut() = 0;
    virtual void selectAll() = 0;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual void goBack() = 0;
    virtual void goForward() = 0;
};

// Message callback for player commands from renderer
// metadata is JSON string for "load" command, empty otherwise
using PlayerMessageCallback = std::function<void(const std::string& cmd, const std::string& arg, int intArg, const std::string& metadata)>;

// Cursor change callback (passes CEF cursor type)
using CursorChangeCallback = std::function<void(cef_cursor_type_t type)>;

// Fullscreen mode change callback (web content requested fullscreen)
using FullscreenChangeCallback = std::function<void(bool fullscreen)>;

// Theme color change callback (web content updated <meta name="theme-color">)
using ThemeColorCallback = std::function<void(const std::string& color)>;

// Cursor visibility change callback (web content toggled mouse idle state)
using CursorVisibilityCallback = std::function<void(bool visible)>;

// OSD visibility change callback (video player OSD shown/hidden)
using OsdVisibleCallback = std::function<void(bool visible)>;

// Physical pixel size callback (returns actual framebuffer dimensions)
using PhysicalSizeCallback = std::function<void(int& width, int& height)>;

// Accelerated paint callback (dmabuf on Linux)
// fd: dmabuf file descriptor, stride/offset/size: plane info, modifier: for EGL
using AcceleratedPaintCallback = std::function<void(int fd, uint32_t stride, uint64_t modifier,
                                                     int width, int height)>;

#ifdef __APPLE__
// IOSurface paint callback (macOS accelerated paint)
// surface: IOSurfaceRef, format: pixel format enum
using IOSurfacePaintCallback = std::function<void(void* surface, int format, int width, int height)>;
#endif

// Popup show/hide callback (accelerated paint path)
using PopupShowCallback = std::function<void(bool show)>;

// Popup size callback (CSS logical coordinates, accelerated paint path)
using PopupSizeCallback = std::function<void(int x, int y, int width, int height)>;

#ifdef _WIN32
// Windows shared texture paint callback (D3D11 shared handle via OnAcceleratedPaint)
// handle: NT HANDLE to D3D11 Texture2D, valid only during callback
using WinSharedTexturePaintCallback = std::function<void(void* handle, int type, int width, int height)>;
#endif

class Client : public CefClient, public CefRenderHandler, public CefLifeSpanHandler, public CefDisplayHandler, public CefLoadHandler, public CefContextMenuHandler, public InputReceiver {
public:
    using PaintCallback = std::function<void(const void* buffer, int width, int height)>;

    Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg = nullptr,
           AcceleratedPaintCallback on_accel_paint = nullptr, MenuOverlay* menu = nullptr,
           CursorChangeCallback on_cursor_change = nullptr,
           FullscreenChangeCallback on_fullscreen_change = nullptr,
           PhysicalSizeCallback physical_size_cb = nullptr,
           ThemeColorCallback on_theme_color = nullptr,
           CursorVisibilityCallback on_cursor_visibility = nullptr,
           OsdVisibleCallback on_osd_visible = nullptr,
           PopupShowCallback on_popup_show = nullptr,
           PopupSizeCallback on_popup_size = nullptr,
           AcceleratedPaintCallback on_accel_popup_paint = nullptr
#ifdef __APPLE__
           , IOSurfacePaintCallback on_iosurface_paint = nullptr
#endif
#ifdef _WIN32
           , WinSharedTexturePaintCallback on_win_shared_paint = nullptr
#endif
           );

    // CefClient
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefProcessId source_process,
                                   CefRefPtr<CefProcessMessage> message) override;

    // CefDisplayHandler
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t level,
                          const CefString& message,
                          const CefString& source,
                          int line) override;
    bool OnCursorChange(CefRefPtr<CefBrowser> browser,
                        CefCursorHandle cursor,
                        cef_cursor_type_t type,
                        const CefCursorInfo& custom_cursor_info) override;
    void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser,
                                bool fullscreen) override;

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override;
    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override;
    void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                 const RectList& dirtyRects, const void* buffer,
                 int width, int height) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList& dirtyRects,
                            const CefAcceleratedPaintInfo& info) override;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    // CefLoadHandler
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;

    // CefContextMenuHandler
    bool RunContextMenu(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefContextMenuParams> params,
                        CefRefPtr<CefMenuModel> model,
                        CefRefPtr<CefRunContextMenuCallback> callback) override;

    bool isClosed() const { return is_closed_; }
    CefRefPtr<CefBrowser> browser() const {
        std::lock_guard<std::mutex> lock(browser_mutex_);
        return browser_;
    }

    // Input forwarding (InputReceiver)
    void sendMouseMove(int x, int y, int modifiers) override;
    void sendMouseClick(int x, int y, bool down, int button, int clickCount, int modifiers) override;
    void sendMouseWheel(int x, int y, float deltaX, float deltaY, int modifiers) override;
    void sendKeyEvent(int key, bool down, int modifiers) override;
    void sendChar(int charCode, int modifiers) override;
    void sendTouch(int id, float x, float y, float radiusX, float radiusY,
                   float pressure, int type, int modifiers) override;
    void sendFocus(bool focused) override;
    void paste(const char* mimeType, const void* data, size_t len) override;
    void copy() override;
    void cut() override;
    void selectAll() override;
    void undo() override;
    void redo() override;
    void goBack() override;
    void goForward() override;
    void resize(int width, int height, int physical_w, int physical_h);
    void forceRepaint();
    void loadUrl(const std::string& url);

#ifdef __APPLE__
    // Flush coalesced scroll events (call once per frame after event processing)
    void flushScroll();
#endif

    // Override scale factor (0 = use physical/logical ratio)
    void setScaleOverride(float scale) { scale_override_ = scale; }

    // Execute JavaScript in the browser
    void executeJS(const std::string& code);

    // Exit browser fullscreen mode (call when window exits fullscreen)
    void exitFullscreen();

    // Player signal helpers
    void emitPlaying();
    void emitPaused();
    void emitSeeking();
    void emitFinished();
    void emitCanceled();
    void emitError(const std::string& msg);
    void emitRateChanged(double rate);
    void updatePosition(double positionMs);
    void updateDuration(double durationMs);

private:
    int width_;
    int height_;
    PaintCallback on_paint_;
    PlayerMessageCallback on_player_msg_;
    AcceleratedPaintCallback on_accel_paint_;
    PopupShowCallback on_popup_show_;
    PopupSizeCallback on_popup_size_;
    AcceleratedPaintCallback on_accel_popup_paint_;
#ifdef __APPLE__
    IOSurfacePaintCallback on_iosurface_paint_;
#endif
#ifdef _WIN32
    WinSharedTexturePaintCallback on_win_shared_paint_;
#endif
    MenuOverlay* menu_ = nullptr;
    CursorChangeCallback on_cursor_change_;
    FullscreenChangeCallback on_fullscreen_change_;
    ThemeColorCallback on_theme_color_;
    CursorVisibilityCallback on_cursor_visibility_;
    OsdVisibleCallback on_osd_visible_;
    PhysicalSizeCallback physical_size_cb_;
    float scale_override_ = 0.0f;  // 0 = use physical/logical ratio
    int physical_w_ = 0;  // Stored physical dimensions (set during resize)
    int physical_h_ = 0;
#ifdef __APPLE__
    float accum_scroll_x_ = 0.0f;  // Sub-pixel scroll accumulator
    float accum_scroll_y_ = 0.0f;
    int scroll_x_ = 0, scroll_y_ = 0;  // Last scroll position for coalesced event
    int scroll_mods_ = 0;
    bool has_pending_scroll_ = false;
#endif
    mutable std::mutex browser_mutex_;  // Protects browser_ across threads
    std::atomic<bool> is_closed_ = false;
    CefRefPtr<CefBrowser> browser_;     // Guarded by browser_mutex_

    // Popup (dropdown) state
    bool popup_visible_ = false;
    CefRect popup_rect_;
    int popup_pixel_width_ = 0;
    int popup_pixel_height_ = 0;
    std::vector<uint8_t> popup_buffer_;
    std::vector<uint8_t> composite_buffer_;  // Main view + popup blended

    IMPLEMENT_REFCOUNTING(Client);
    DISALLOW_COPY_AND_ASSIGN(Client);
};

// Simplified client for overlay browser (no player, no menu)
class OverlayClient : public CefClient, public CefRenderHandler, public CefLifeSpanHandler, public CefDisplayHandler, public InputReceiver {
public:
    using PaintCallback = std::function<void(const void* buffer, int width, int height)>;
    using LoadServerCallback = std::function<void(const std::string& url)>;

    OverlayClient(int width, int height, PaintCallback on_paint, LoadServerCallback on_load_server,
                  PhysicalSizeCallback physical_size_cb = nullptr,
                  AcceleratedPaintCallback on_accel_paint = nullptr
#ifdef __APPLE__
                  , IOSurfacePaintCallback on_iosurface_paint = nullptr
#endif
#ifdef _WIN32
                  , WinSharedTexturePaintCallback on_win_shared_paint = nullptr
#endif
                  );

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefProcessId source_process,
                                   CefRefPtr<CefProcessMessage> message) override;

    // CefDisplayHandler
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t level,
                          const CefString& message,
                          const CefString& source,
                          int line) override;

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                 const RectList& dirtyRects, const void* buffer,
                 int width, int height) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList& dirtyRects,
                            const CefAcceleratedPaintInfo& info) override;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    bool isClosed() const { return is_closed_; }
    CefRefPtr<CefBrowser> browser() const {
        std::lock_guard<std::mutex> lock(browser_mutex_);
        return browser_;
    }
    void resize(int width, int height, int physical_w, int physical_h);
    void setScaleOverride(float scale) { scale_override_ = scale; }
#ifdef __APPLE__
    void flushScroll();
#endif
    void sendFocus(bool focused) override;
    void sendMouseMove(int x, int y, int modifiers) override;
    void sendMouseClick(int x, int y, bool down, int button, int clickCount, int modifiers) override;
    void sendMouseWheel(int x, int y, float deltaX, float deltaY, int modifiers) override;
    void sendKeyEvent(int key, bool down, int modifiers) override;
    void sendChar(int charCode, int modifiers) override;
    void sendTouch(int id, float x, float y, float radiusX, float radiusY,
                   float pressure, int type, int modifiers) override;
    void paste(const char* mimeType, const void* data, size_t len) override;
    void copy() override;
    void cut() override;
    void selectAll() override;
    void undo() override;
    void redo() override;
    void goBack() override {}
    void goForward() override {}

private:
    int width_;
    int height_;
    PaintCallback on_paint_;
    LoadServerCallback on_load_server_;
    PhysicalSizeCallback physical_size_cb_;
    AcceleratedPaintCallback on_accel_paint_;
#ifdef __APPLE__
    IOSurfacePaintCallback on_iosurface_paint_;
#endif
#ifdef _WIN32
    WinSharedTexturePaintCallback on_win_shared_paint_;
#endif
    float scale_override_ = 0.0f;
    int physical_w_ = 0;  // Stored physical dimensions (set during resize)
    int physical_h_ = 0;
#ifdef __APPLE__
    float accum_scroll_x_ = 0.0f;  // Sub-pixel scroll accumulator
    float accum_scroll_y_ = 0.0f;
    int scroll_x_ = 0, scroll_y_ = 0;
    int scroll_mods_ = 0;
    bool has_pending_scroll_ = false;
#endif
    mutable std::mutex browser_mutex_;  // Protects browser_ across threads
    std::atomic<bool> is_closed_ = false;
    CefRefPtr<CefBrowser> browser_;     // Guarded by browser_mutex_

    IMPLEMENT_REFCOUNTING(OverlayClient);
    DISALLOW_COPY_AND_ASSIGN(OverlayClient);
};
