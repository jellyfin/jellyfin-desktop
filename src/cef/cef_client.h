#pragma once

#include "../common.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_display_handler.h"
#include <condition_variable>
#include <mutex>

// Main browser client -- handles jellyfin-web and player commands.
// OnAcceleratedPaint sends dmabuf to main Wayland subsurface via g_platform.present().
class Client : public CefClient, public CefRenderHandler,
               public CefLifeSpanHandler, public CefLoadHandler,
               public CefContextMenuHandler, public CefDisplayHandler {
public:
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override;
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&,
                 const void*, int w, int h) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                            const RectList&, const CefAcceleratedPaintInfo& info) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser>) override;
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override;
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                     ErrorCode, const CefString& errorText, const CefString& failedUrl) override;

    // CefDisplayHandler
    void OnFullscreenModeChange(CefRefPtr<CefBrowser>, bool fullscreen) override;
    bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                        cef_cursor_type_t type, const CefCursorInfo&) override;

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                  CefProcessId, CefRefPtr<CefProcessMessage> message) override;

    // Suppress context menu
    bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                        CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel>,
                        CefRefPtr<CefRunContextMenuCallback> callback) override {
        callback->Cancel();
        return true;
    }

    void resize(int w, int h, int physical_w, int physical_h);
    bool isClosed() const { return closed_; }
    bool isLoaded() const { return loaded_; }
    CefRefPtr<CefBrowser> browser() { return browser_; }
    void waitForClose();
    void waitForLoad();
    void execJs(const std::string& js);
    // Unblock waiters when browser was never created or died early
    void markClosed() { closed_ = true; loaded_ = true; close_cv_.notify_all(); load_cv_.notify_all(); }

private:
    int width_ = 1280, height_ = 720;
    int physical_w_ = 1280, physical_h_ = 720;
    CefRefPtr<CefBrowser> browser_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> loaded_{false};
    std::mutex close_mtx_;
    std::condition_variable close_cv_;
    std::mutex load_mtx_;
    std::condition_variable load_cv_;
    IMPLEMENT_REFCOUNTING(Client);
};

// Overlay browser client -- handles server selection/loading UI.
// OnAcceleratedPaint sends dmabuf to overlay Wayland subsurface via g_platform.overlay_present().
class OverlayClient : public CefClient, public CefRenderHandler,
                      public CefLifeSpanHandler, public CefLoadHandler {
public:
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override;
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&,
                 const void*, int w, int h) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                            const RectList&, const CefAcceleratedPaintInfo& info) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser>) override;
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override;

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                  CefProcessId, CefRefPtr<CefProcessMessage> message) override;

    void resize(int w, int h, int physical_w, int physical_h);
    bool isClosed() const { return closed_; }
    bool isLoaded() const { return loaded_; }
    CefRefPtr<CefBrowser> browser() { return browser_; }
    void waitForClose();
    void waitForLoad();
    void execJs(const std::string& js);
    void markClosed() { closed_ = true; loaded_ = true; close_cv_.notify_all(); load_cv_.notify_all(); }

private:
    int width_ = 1280, height_ = 720;
    int physical_w_ = 1280, physical_h_ = 720;
    CefRefPtr<CefBrowser> browser_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> loaded_{false};
    std::mutex close_mtx_;
    std::condition_variable close_cv_;
    std::mutex load_mtx_;
    std::condition_variable load_cv_;
    IMPLEMENT_REFCOUNTING(OverlayClient);
};
