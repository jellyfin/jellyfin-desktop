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

class Browsers;
struct PlatformSurface;

// Callback invoked for IPC messages from the renderer process.
// Returns true if the message was handled.
using MessageHandler = std::function<bool(const std::string& name,
                                         CefRefPtr<CefListValue> args,
                                         CefRefPtr<CefBrowser> browser)>;

// Callback invoked after the browser is created (OnAfterCreated).
using CreatedCallback = std::function<void(CefRefPtr<CefBrowser>)>;

// Callback invoked just before the browser is destroyed (OnBeforeClose).
using BeforeCloseCallback = std::function<void()>;

// Callbacks for app-level context menu items. CefLayer is policy-free: it
// asks the app to append items and to dispatch unknown command IDs.
using ContextMenuBuilder = std::function<void(CefRefPtr<CefMenuModel>)>;
using ContextMenuDispatcher = std::function<bool(int command_id)>;

// Generic CEF browser client — pure rendering, lifecycle, context menu,
// keyboard. Business logic is injected via setMessageHandler / setCreatedCallback.
// CefLayer holds a generic PlatformSurface*; presents/resizes/visibility
// route through g_platform.surface_*.
class CefLayer : public CefClient, public CefRenderHandler,
                 public CefLifeSpanHandler, public CefLoadHandler,
                 public CefContextMenuHandler, public CefDisplayHandler,
                 public CefKeyboardHandler {
public:
    CefLayer(Browsers& browsers, PlatformSurface* surface);
    ~CefLayer() override;

    void setName(std::string name) { name_ = std::move(name); }
    const std::string& name() const { return name_; }

    void setMessageHandler(MessageHandler handler) { message_handler_ = std::move(handler); }
    void setCreatedCallback(CreatedCallback cb) { on_after_created_ = std::move(cb); }
    void setBeforeCloseCallback(BeforeCloseCallback cb) { on_before_close_ = std::move(cb); }
    void setContextMenuBuilder(ContextMenuBuilder cb) { context_menu_builder_ = std::move(cb); }
    void setContextMenuDispatcher(ContextMenuDispatcher cb) { context_menu_dispatcher_ = std::move(cb); }

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
    // Mirror of the renderer-side rAF deadline loop (cef_app.cpp). Each
    // resize bumps a 5s sliding deadline; while live, periodic
    // Invalidate(PET_VIEW) on TID_UI keeps the host nudging the renderer
    // even when JS rAF wouldn't fire (e.g. when the page is static and
    // the renderer skipped a compositor frame).
    void kickInvalidateLoop();
    bool isClosed() const { return closed_; }
    bool isLoaded() const { return loaded_; }
    CefRefPtr<CefBrowser> browser() { return browser_; }
    void waitForClose();
    void waitForLoad();
    void execJs(const std::string& js);
    void setRefreshRate(double hz);
    void setVisible(bool visible);
    void fade(float fade_sec,
              std::function<void()> on_fade_start,
              std::function<void()> on_complete);

    PlatformSurface* surface() const { return surface_; }

    // Native-shim injection profile travels to the renderer's
    // OnBrowserCreated; carries jmpNative function list + script list.
    // Set once by the owning subclass; reused across reset() cycles.
    void setInjectionProfile(CefRefPtr<CefDictionaryValue> p) { extra_info_ = std::move(p); }

    // Create the underlying CEF browser. Builds CefWindowInfo and
    // CefBrowserSettings from the Browsers display state.
    void create(const std::string& url);

    // Tear down the current browser and recreate with no URL (blank).
    void reset();

    // Navigate the current browser to `url`.
    void loadUrl(const std::string& url);

    // Called by Browsers when this layer stops being the input target,
    // or just before its surface is freed. Tears down anything that
    // shouldn't outlive active status — currently the popup.
    void onDeactivated();

private:
    enum class State { Normal, PendingReset, Recreating };

    Browsers& browsers_;
    PlatformSurface* surface_ = nullptr;
    std::string name_;
    int width_ = 0, height_ = 0;
    int physical_w_ = 0, physical_h_ = 0;
    int frame_rate_ = 0;
    // Popup state pairs 1:1 with its surface — each CefLayer owns its
    // popup on the platform side (PlatformSurface gains popup fields per
    // backend).
    CefRect popup_rect_;
    bool popup_visible_ = false;
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
    ContextMenuBuilder context_menu_builder_;
    ContextMenuDispatcher context_menu_dispatcher_;
    CefRefPtr<CefDictionaryValue> extra_info_;
    State state_ = State::Normal;
    std::string pending_url_;
    std::atomic<bool> invalidate_running_{false};
    std::atomic<bool> invalidate_stop_{false};
    int saved_frame_rate_ = 0;  // TID_UI-only: nonzero while boosted
    void invalidateTick();

    // Debounced WasResized: many WM configures per drag would each fire
    // a CEF re-layout. Wayland viewport (surface_resize) still applies
    // immediately; only the CEF host notify is coalesced to one per
    // display-refresh period.
    std::atomic<bool> resize_scheduled_{false};
    std::atomic<int64_t> last_was_resized_ns_{0};
    void applyPendingResize();

    // Nudge-loop stop heuristic: after 3 consecutive same-size paints
    // the renderer has stabilised at the post-resize dims, so stop both
    // the host Invalidate loop and the renderer rAF loop.
    int last_paint_w_ = 0, last_paint_h_ = 0;
    int consecutive_size_match_ = 0;
    // Tracks the rate currently applied via setFrameRate so the
    // stable-streak target and CEF compositor rate stay in lockstep.
    int stable_match_target_ = 0;
    static constexpr int kBoostFrameRate = 480;
    // Single source of truth: sets CEF windowless rate AND derives the
    // stable-size streak target (1 frame per 20Hz, ceil).
    void setFrameRate(int hz);
    // Bumped on every resize(); noteStableSize requires the generation
    // observed when its run started to still match — otherwise a resize
    // landed mid-run and the stable-size signal is stale.
    std::atomic<uint64_t> resize_gen_{0};
    uint64_t run_start_gen_ = 0;
    void noteStableSize(int w, int h);
    IMPLEMENT_REFCOUNTING(CefLayer);
};
