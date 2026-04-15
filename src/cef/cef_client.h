#pragma once

#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_keyboard_handler.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

// Callback invoked for IPC messages from the renderer process.
// Returns true if the message was handled.
using MessageHandler = std::function<bool(const std::string& name,
                                         CefRefPtr<CefListValue> args,
                                         CefRefPtr<CefBrowser> browser)>;

// Callback invoked after the browser is created (OnAfterCreated).
using CreatedCallback = std::function<void(CefRefPtr<CefBrowser>)>;

// Render target callbacks — decouple the client from the platform layer.
struct RenderTarget {
    void (*present)(const CefAcceleratedPaintInfo& info);
    void (*present_software)(const CefRenderHandler::RectList& dirty,
                             const void* buffer, int w, int h);
};

// Generic CEF browser client — pure rendering, lifecycle, context menu,
// keyboard. Business logic is injected via setMessageHandler / setCreatedCallback.
// Used for both the main browser and overlay browser; the only difference
// is the RenderTarget passed at construction.
class CefLayer : public CefClient, public CefRenderHandler,
                 public CefLifeSpanHandler, public CefLoadHandler,
                 public CefContextMenuHandler, public CefDisplayHandler,
                 public CefKeyboardHandler {
public:
    explicit CefLayer(RenderTarget target) : target_(target) {}

    void setMessageHandler(MessageHandler handler) { message_handler_ = std::move(handler); }
    void setCreatedCallback(CreatedCallback cb) { on_after_created_ = std::move(cb); }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }

    bool OnPreKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent&,
                       CefEventHandle, bool* is_keyboard_shortcut) override;

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

    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                             CefRefPtr<CefContextMenuParams>,
                             CefRefPtr<CefMenuModel> model) override;
    bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                        CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel>,
                        CefRefPtr<CefRunContextMenuCallback>) override;

    void resize(int w, int h, int physical_w, int physical_h);
    bool isClosed() const { return closed_; }
    bool isLoaded() const { return loaded_; }
    CefRefPtr<CefBrowser> browser() { return browser_; }
    void waitForClose();
    void waitForLoad();
    void execJs(const std::string& js);

private:
    RenderTarget target_;
    int width_ = 1280, height_ = 720;
    int physical_w_ = 1280, physical_h_ = 720;
    CefRefPtr<CefBrowser> browser_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> loaded_{false};
    std::mutex close_mtx_;
    std::condition_variable close_cv_;
    std::mutex load_mtx_;
    std::condition_variable load_cv_;
    CefRefPtr<CefRunContextMenuCallback> pending_menu_callback_;
    MessageHandler message_handler_;
    CreatedCallback on_after_created_;
    IMPLEMENT_REFCOUNTING(CefLayer);
};
