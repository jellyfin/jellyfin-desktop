#pragma once

#include "include/cef_browser.h"
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
#include <vector>

// Callback invoked for IPC messages from the renderer process.
// Returns true if the message was handled.
using MessageHandler = std::function<bool(const std::string& name,
                                         CefRefPtr<CefListValue> args,
                                         CefRefPtr<CefBrowser> browser)>;

// Callback invoked after the browser is created (OnAfterCreated).
using CreatedCallback = std::function<void(CefRefPtr<CefBrowser>)>;

// Callback invoked just before the browser is destroyed (OnBeforeClose).
using BeforeCloseCallback = std::function<void()>;

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
    CefLayer(RenderTarget target, int w, int h, int pw, int ph)
        : target_(target), width_(w), height_(h),
          physical_w_(pw), physical_h_(ph) {}

    void setMessageHandler(MessageHandler handler) { message_handler_ = std::move(handler); }
    void setCreatedCallback(CreatedCallback cb) { on_after_created_ = std::move(cb); }
    void setBeforeCloseCallback(BeforeCloseCallback cb) { on_before_close_ = std::move(cb); }

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
    void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override;
    void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&,
                 const void*, int w, int h) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                            const RectList&, const CefAcceleratedPaintInfo& info) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser>) override;
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int popup_id,
                       const CefString& target_url, const CefString& target_frame_name,
                       WindowOpenDisposition target_disposition, bool user_gesture,
                       const CefPopupFeatures&, CefWindowInfo&,
                       CefRefPtr<CefClient>&, CefBrowserSettings&,
                       CefRefPtr<CefDictionaryValue>&, bool* no_javascript_access) override;
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override;
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                     ErrorCode, const CefString& errorText, const CefString& failedUrl) override;

    // CefDisplayHandler
    void OnFullscreenModeChange(CefRefPtr<CefBrowser>, bool fullscreen) override;
    bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                        cef_cursor_type_t type, const CefCursorInfo&) override;
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level,
                          const CefString& message, const CefString& source,
                          int line) override;

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

    // Create the underlying CEF browser. Stores wi/bs for use in reset().
    void create(const CefWindowInfo& wi, const CefBrowserSettings& bs, const std::string& url);

    // Tear down the current browser and recreate with no URL (blank).
    // Asynchronous: the new browser is ready when OnAfterCreated fires.
    // Safe to call before the initial create has completed, or while a
    // previous reset is still in flight — subsequent calls are absorbed
    // into the pending cycle.
    void reset();

    // Navigate the current browser to `url`. If a reset is in flight or the
    // initial create hasn't completed yet, the URL is buffered and applied
    // when the browser becomes ready.
    void loadUrl(const std::string& url);

private:
    // Lifecycle states. Normal is the steady state; PendingReset means
    // reset() was called before the initial OnAfterCreated and is waiting
    // for it to fire so it can issue the close; Recreating means CloseBrowser
    // has been issued (or is about to fire from the one-shot) and we're
    // awaiting the blank replacement's OnAfterCreated.
    enum class State { Normal, PendingReset, Recreating };

    RenderTarget target_;
    int width_, height_;
    int physical_w_, physical_h_;
    CefRect popup_rect_;
    bool popup_visible_ = false;
    // Native popup menu orchestration. OnPopupShow fires a renderer query
    // for the focused <select>'s options; OnPopupSize delivers the rect.
    // try_show_popup waits for both, then hands off to the platform's
    // native menu (macOS) or the compositor popup subsurface (Wayland).
    std::vector<std::string> popup_options_;
    int popup_selected_idx_ = -1;
    bool popup_size_received_ = false;
    bool popup_options_received_ = false;

    void reset_popup_state();
    void try_show_popup();
    void dispatch_popup_selection(int index);
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
    BeforeCloseCallback on_before_close_;
    CefWindowInfo window_info_;
    CefBrowserSettings browser_settings_;
    State state_ = State::Normal;
    std::string pending_url_;
    IMPLEMENT_REFCOUNTING(CefLayer);
};
