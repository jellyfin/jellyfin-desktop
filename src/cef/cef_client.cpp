#include "cef_client.h"
#include "logging.h"
#include "../browser/browsers.h"
#include "../cjson/cJSON.h"
#include "../mpv/event.h"
#include "../platform/platform.h"
#include "include/cef_task.h"
#include <cmath>
#include <chrono>
#include <cstdio>
#include <functional>

namespace {
// Small CefTask adapter so we can post lambdas to the CEF UI thread without
// pulling in base::BindOnce headers.
class FnTask : public CefTask {
public:
    explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override { if (fn_) fn_(); }
private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(FnTask);
};

CefRefPtr<CefFrame> focused_or_main(CefRefPtr<CefBrowser> browser) {
    if (!browser) return nullptr;
    CefRefPtr<CefFrame> frame = browser->GetFocusedFrame();
    return frame ? frame : browser->GetMainFrame();
}
}

extern Platform g_platform;
extern std::atomic<bool> g_shutting_down;

// =====================================================================
// Shared helpers (context menu, clipboard)
// =====================================================================

static std::string stripAccelerator(const std::string& label) {
    std::string out;
    out.reserve(label.size());
    for (size_t i = 0; i < label.size(); i++) {
        if (label[i] == '&') continue;
        out += label[i];
    }
    return out;
}

static cJSON* serializeMenuModel(CefRefPtr<CefMenuModel> model) {
    cJSON* arr = cJSON_CreateArray();
    for (size_t i = 0; i < model->GetCount(); i++) {
        cJSON* item = cJSON_CreateObject();
        auto type = model->GetTypeAt(i);
        if (type == MENUITEMTYPE_SEPARATOR) {
            cJSON_AddBoolToObject(item, "sep", 1);
        } else {
            int id = model->GetCommandIdAt(i);
            std::string label = stripAccelerator(model->GetLabelAt(i).ToString());
            cJSON_AddNumberToObject(item, "id", id);
            cJSON_AddStringToObject(item, "label", label.c_str());
            cJSON_AddBoolToObject(item, "enabled", model->IsEnabledAt(i));
        }
        cJSON_AddItemToArray(arr, item);
    }
    return arr;
}

#ifdef __APPLE__
constexpr uint32_t kActionModifier = EVENTFLAG_COMMAND_DOWN;
#else
constexpr uint32_t kActionModifier = EVENTFLAG_CONTROL_DOWN;
#endif

static bool is_paste_shortcut(const CefKeyEvent& e) {
    if (e.type != KEYEVENT_RAWKEYDOWN) return false;
    if ((e.modifiers & kActionModifier) == 0) return false;
    if (e.modifiers & EVENTFLAG_ALT_DOWN) return false;
    return e.windows_key_code == 'V';
}

static std::string js_string_literal(const std::string& text) {
    cJSON* j = cJSON_CreateString(text.c_str());
    if (!j) return "\"\"";
    char* s = cJSON_PrintUnformatted(j);
    std::string result = s ? s : "\"\"";
    if (s) cJSON_free(s);
    cJSON_Delete(j);
    return result;
}

static void paste_via_platform_clipboard(CefRefPtr<CefBrowser> browser) {
    auto frame = focused_or_main(browser);
    if (!frame) return;
    g_platform.clipboard_read_text_async([frame](std::string text) {
        if (text.empty()) return;
        std::string js = "document.execCommand('insertText',false," +
                         js_string_literal(text) + ");";
        frame->ExecuteJavaScript(js, frame->GetURL(), 0);
    });
}

static void do_paste(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) {
    if (g_platform.clipboard_read_text_async)
        paste_via_platform_clipboard(browser);
    else
        frame->Paste();
}

static bool try_intercept_paste(CefRefPtr<CefBrowser> browser,
                                const CefKeyEvent& e) {
    if (!g_platform.clipboard_read_text_async) return false;
    if (!is_paste_shortcut(e)) return false;
    paste_via_platform_clipboard(browser);
    return true;
}

// =====================================================================
// CefLayer
// =====================================================================

CefLayer::CefLayer(Browsers& browsers, PlatformSurface* surface)
    : browsers_(browsers), surface_(surface) {}

CefLayer::~CefLayer() = default;

void CefLayer::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) {
    rect.Set(0, 0, width_, height_);
}

bool CefLayer::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) {
    float scale = (physical_w_ > 0 && width_ > 0)
        ? static_cast<float>(physical_w_) / width_
        : 1.0f;
    info.device_scale_factor = scale;
    info.rect = CefRect(0, 0, width_, height_);
    info.available_rect = info.rect;
    return true;
}

void CefLayer::resize(int w, int h, int physical_w, int physical_h) {
    LOG_TRACE(LOG_CEF, "CefLayer::resize name={} logical={}x{} physical={}x{}",
             name_.c_str(), w, h, physical_w, physical_h);
    width_ = w;
    height_ = h;
    physical_w_ = physical_w;
    physical_h_ = physical_h;
    resize_gen_.fetch_add(1, std::memory_order_acq_rel);
    // Wayland viewport must update on every configure to avoid stale
    // src/dst — runs immediately.
    if (surface_ && g_platform.surface_resize)
        g_platform.surface_resize(surface_, w, h, physical_w, physical_h);
    if (!browser_) {
        kickInvalidateLoop();
        return;
    }
    // Debounce the CEF host notify (re-layout) to one display-refresh
    // period. Drag fires many configures per frame; coalescing them
    // saves N-1 wasted re-layouts.
    using namespace std::chrono;
    int64_t now = duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count();
    double hz = mpv::display_hz();
    int64_t period_ns = (hz > 0) ? static_cast<int64_t>(1e9 / hz) : 16'666'667LL;
    int64_t last = last_was_resized_ns_.load(std::memory_order_acquire);
    if (now - last >= period_ns) {
        last_was_resized_ns_.store(now, std::memory_order_release);
        browser_->GetHost()->NotifyScreenInfoChanged();
        browser_->GetHost()->WasResized();
        browser_->GetHost()->Invalidate(PET_VIEW);
        kickInvalidateLoop();
        return;
    }
    // Within the debounce window. Schedule a single deferred apply if
    // one isn't already pending; latest width_/height_ are what get
    // picked up.
    bool expected = false;
    if (resize_scheduled_.compare_exchange_strong(expected, true)) {
        CefRefPtr<CefLayer> self = this;
        int delay_ms = static_cast<int>((period_ns - (now - last)) / 1'000'000) + 1;
        if (delay_ms < 1) delay_ms = 1;
        CefPostDelayedTask(TID_UI,
            CefRefPtr<CefTask>(new FnTask([self]() { self->applyPendingResize(); })),
            delay_ms);
    }
    kickInvalidateLoop();
}

void CefLayer::applyPendingResize() {
    resize_scheduled_.store(false, std::memory_order_release);
    if (!browser_) return;
    using namespace std::chrono;
    int64_t now = duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count();
    last_was_resized_ns_.store(now, std::memory_order_release);
    // WasResized changes the target the renderer is converging toward;
    // any prior stable-size streak (possibly accumulated against the old
    // logical dims while this apply was pending) must be invalidated.
    resize_gen_.fetch_add(1, std::memory_order_acq_rel);
    browser_->GetHost()->NotifyScreenInfoChanged();
    browser_->GetHost()->WasResized();
    browser_->GetHost()->Invalidate(PET_VIEW);
    kickInvalidateLoop();
}

void CefLayer::setFrameRate(int hz) {
    if (hz <= 0) return;
    if (browser_) browser_->GetHost()->SetWindowlessFrameRate(hz);
    // 1 stable frame per 20Hz of compositor rate, ceil.
    stable_match_target_ = (hz + 19) / 20;
}

void CefLayer::kickInvalidateLoop() {
    invalidate_stop_.store(false, std::memory_order_release);
    bool expected = false;
    if (!invalidate_running_.compare_exchange_strong(expected, true)) return;
    run_start_gen_ = resize_gen_.load(std::memory_order_acquire);
    consecutive_size_match_ = 0;
    CefRefPtr<CefLayer> self = this;
    CefPostTask(TID_UI, CefRefPtr<CefTask>(new FnTask([self]() {
        // Boost CEF compositor rate while the loop is live — JS rAF ties
        // to compositor rate, so this accelerates frame production for
        // faster convergence to the post-resize size.
        if (self->browser_ && self->frame_rate_ > 0 && self->saved_frame_rate_ == 0) {
            self->saved_frame_rate_ = self->frame_rate_;
            self->setFrameRate(kBoostFrameRate);
        }
        self->invalidateTick();
    })));
}

void CefLayer::invalidateTick() {
    if (invalidate_stop_.load(std::memory_order_acquire)) {
        if (browser_ && saved_frame_rate_ > 0) {
            setFrameRate(saved_frame_rate_);
            saved_frame_rate_ = 0;
        }
        invalidate_running_.store(false, std::memory_order_release);
        LOG_DEBUG(LOG_CEF, "CefLayer::invalidateTick stopped name={}", name_.c_str());
        return;
    }
    if (browser_) browser_->GetHost()->Invalidate(PET_VIEW);
    CefRefPtr<CefLayer> self = this;
    CefPostDelayedTask(TID_UI,
        CefRefPtr<CefTask>(new FnTask([self]() { self->invalidateTick(); })),
        4);
}

void CefLayer::setVisible(bool visible) {
    if (surface_ && g_platform.surface_set_visible)
        g_platform.surface_set_visible(surface_, visible);
}

void CefLayer::fade(float fade_sec,
                    std::function<void()> on_fade_start,
                    std::function<void()> on_complete) {
    if (surface_ && g_platform.fade_surface) {
        g_platform.fade_surface(surface_, fade_sec,
                                std::move(on_fade_start),
                                std::move(on_complete));
        return;
    }
    // Backend without fade support — fire callbacks; on_complete typically
    // closes the browser, which destroys the surface via Browsers::remove.
    if (on_fade_start) on_fade_start();
    if (on_complete) on_complete();
}

void CefLayer::setRefreshRate(double hz) {
    if (hz <= 0) return;
    int target = static_cast<int>(std::ceil(hz));
    CefRefPtr<CefLayer> self = this;
    CefPostTask(TID_UI, CefRefPtr<CefTask>(new FnTask([self, target]() {
        self->frame_rate_ = target;
        // If a nudge-loop boost is active, just update what we'll restore
        // to and let the boost rate (480) keep running. Otherwise apply
        // the new rate immediately.
        if (self->saved_frame_rate_ > 0) {
            self->saved_frame_rate_ = target;
        } else {
            self->setFrameRate(target);
        }
    })));
}

void CefLayer::reset_popup_state() {
    popup_size_received_ = false;
    popup_options_received_ = false;
    popup_options_.clear();
    popup_selected_idx_ = -1;
}

void CefLayer::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) {
    popup_visible_ = show;
    reset_popup_state();
    if (!show) {
        if (surface_) g_platform.popup_hide(surface_);
        return;
    }
    if (CefRefPtr<CefFrame> frame = focused_or_main(browser)) {
        auto msg = CefProcessMessage::Create("getPopupOptions");
        frame->SendProcessMessage(PID_RENDERER, msg);
    }
}

void CefLayer::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) {
    popup_rect_ = rect;
    popup_size_received_ = true;
    try_show_popup();
}

void CefLayer::try_show_popup() {
    if (!popup_visible_ || !popup_size_received_ || !popup_options_received_)
        return;

    // on_selected fires only on native-menu backends (macOS). Compositor
    // backends ignore it; CEF dispatches selection itself on click.
    CefRefPtr<CefLayer> self = this;
    auto on_selected = [self](int index) {
        CefPostTask(TID_UI, CefRefPtr<CefTask>(new FnTask([self, index]() {
            self->dispatch_popup_selection(index);
        })));
    };

    Platform::PopupRequest req;
    req.x = popup_rect_.x;
    req.y = popup_rect_.y;
    req.lw = popup_rect_.width;
    req.lh = popup_rect_.height;
    req.options = popup_options_;
    req.initial_highlight = popup_selected_idx_;
    req.on_selected = std::move(on_selected);
    if (surface_) g_platform.popup_show(surface_, req);
}

void CefLayer::onDeactivated() {
    if (popup_visible_) {
        popup_visible_ = false;
        reset_popup_state();
        if (surface_) g_platform.popup_hide(surface_);
    }
}

void CefLayer::dispatch_popup_selection(int index) {
    if (closed_ || !browser_) return;
    if (CefRefPtr<CefFrame> frame = focused_or_main(browser_)) {
        auto msg = CefProcessMessage::Create("applyPopupSelection");
        msg->GetArgumentList()->SetInt(0, index);
        frame->SendProcessMessage(PID_RENDERER, msg);
    }
    // Only public path to CancelWidget on a CEF OSR popup is a mouse-wheel
    // event outside popup_position_ — render_widget_host_view_osr.cc:1337-1343.
    CefMouseEvent me{};
    me.x = -1;
    me.y = -1;
    browser_->GetHost()->SendMouseWheelEvent(me, /*deltaX=*/0, /*deltaY=*/1);
}

void CefLayer::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirty,
                       const void* buffer, int w, int h) {
    if (type == PET_POPUP) {
        if (surface_)
            g_platform.popup_present_software(surface_, buffer, w, h,
                                              popup_rect_.width, popup_rect_.height);
        return;
    }
    if (type != PET_VIEW) return;
    bool attached = false;
    if (surface_ && g_platform.surface_present_software)
        attached = g_platform.surface_present_software(surface_, dirty, buffer, w, h);
    // Only paints that actually landed on the surface count toward the
    // stable-size streak. The Wayland platform drops paints whose size
    // doesn't match mpv's current geometry; counting those would fire
    // stop while the surface still has no buffer attached.
    if (attached) noteStableSize(w, h);
}

void CefLayer::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                                  const RectList&, const CefAcceleratedPaintInfo& info) {
    if (type == PET_POPUP) {
        if (surface_)
            g_platform.popup_present(surface_, info, popup_rect_.width, popup_rect_.height);
        return;
    }
    if (type != PET_VIEW) return;
    bool attached = false;
    if (surface_ && g_platform.surface_present)
        attached = g_platform.surface_present(surface_, info);
    if (attached)
        noteStableSize(info.extra.visible_rect.width, info.extra.visible_rect.height);
}

void CefLayer::noteStableSize(int w, int h) {
    // Steady state has no work to do — nothing to stop.
    if (!invalidate_running_.load(std::memory_order_acquire)) return;
    // A resize after this run started invalidates any stable-size streak —
    // the matching paints may predate the new target.
    uint64_t gen = resize_gen_.load(std::memory_order_acquire);
    if (gen != run_start_gen_) {
        run_start_gen_ = gen;
        consecutive_size_match_ = 0;
        return;
    }
    if (w != last_paint_w_ || h != last_paint_h_) {
        last_paint_w_ = w;
        last_paint_h_ = h;
        consecutive_size_match_ = 1;
        return;
    }
    if (++consecutive_size_match_ != stable_match_target_) return;
    // Renderer stabilised. Signal stop to both the host Invalidate
    // loop and the renderer's rAF loop.
    LOG_DEBUG(LOG_CEF, "CefLayer::noteStableSize stop name={} size={}x{}",
              name_.c_str(), w, h);
    invalidate_stop_.store(true, std::memory_order_release);
    execJs("window.__cefStopRaf && window.__cefStopRaf();");
}

void CefLayer::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    LOG_DEBUG(LOG_CEF, "CefLayer::OnAfterCreated name={}", name_.c_str());
    browser_ = browser;
    closed_ = false;
    loaded_ = false;
    if (g_shutting_down.load(std::memory_order_relaxed)) {
        browser->GetHost()->CloseBrowser(true);
        return;
    }
    browser->GetHost()->NotifyScreenInfoChanged();
    browser->GetHost()->WasResized();
    browser->GetHost()->Invalidate(PET_VIEW);

    // Reset state machine: if reset() was called before the initial
    // OnAfterCreated, close the freshly created browser so the one-shot
    // before-close callback can spin up the blank replacement.
    if (state_ == State::PendingReset) {
        state_ = State::Recreating;
        browser->GetHost()->CloseBrowser(true);
        return;
    }
    // If we're coming out of a reset cycle, return to Normal; the blank
    // browser is up and any URL buffered during the reset is applied below.
    if (state_ == State::Recreating) {
        state_ = State::Normal;
    }

    if (on_after_created_) on_after_created_(browser);

    // Flush any URL buffered while the browser wasn't ready.
    if (!pending_url_.empty()) {
        browser->GetMainFrame()->LoadURL(pending_url_);
        pending_url_.clear();
    }
}

bool CefLayer::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
                             const CefString& target_url, const CefString&,
                             WindowOpenDisposition, bool, const CefPopupFeatures&,
                             CefWindowInfo&, CefRefPtr<CefClient>&,
                             CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&,
                             bool*) {
    // OSR has no host for default popups; route them to the OS.
    std::string url = target_url.ToString();
    // Leading '-' guard blocks argv-style option smuggling into xdg-open.
    if (url.empty() || url[0] == '-') {
        LOG_WARN(LOG_CEF, "OnBeforePopup: refusing URL: '{}'", url);
        return true;
    }
    g_platform.open_external_url(url);
    return true;
}

void CefLayer::OnBeforeClose(CefRefPtr<CefBrowser>) {
    browser_ = nullptr;
    closed_ = true;
    loaded_ = true;
    close_cv_.notify_all();
    load_cv_.notify_all();
    // Move out before invoking. The callback can safely install a new one
    // (via setBeforeCloseCallback) without destroying its own closure —
    // invoking `on_before_close_()` inline would if the callback then
    // nulled the slot.
    auto cb = std::move(on_before_close_);
    if (cb) cb();
}

void CefLayer::create(const std::string& url) {
    CefWindowInfo wi;
    wi.SetAsWindowless(0);
    wi.shared_texture_enabled = browsers_.use_shared_textures();
#ifdef __APPLE__
    // Drive BeginFrames from CVDisplayLink to eliminate phase lag against
    // CEF's internal 60Hz timer.
    wi.external_begin_frame_enabled = true;
#else
    wi.external_begin_frame_enabled = false;
#endif
    CefBrowserSettings bs;
    bs.background_color = 0;
    bs.windowless_frame_rate = frame_rate_ > 0 ? frame_rate_ : browsers_.frame_rate();

    // Auto-inject context-menu shim when the layer has a builder configured.
    // Every wrapper that wires setContextMenuBuilder also needs the JS-side
    // menuItemSelected/menuDismissed IPCs and the context-menu.js script;
    // central them here so wrappers don't repeat the listing.
    CefRefPtr<CefDictionaryValue> info = extra_info_;
    if (context_menu_builder_ && info) {
        info = info->Copy(false);
        auto fns = info->HasKey("functions") ? info->GetList("functions") : CefListValue::Create();
        fns = fns->Copy();
        fns->SetString(fns->GetSize(), "menuItemSelected");
        fns->SetString(fns->GetSize(), "menuDismissed");
        info->SetList("functions", fns);
        auto scripts = info->HasKey("scripts") ? info->GetList("scripts") : CefListValue::Create();
        scripts = scripts->Copy();
        scripts->SetString(scripts->GetSize(), "context-menu.js");
        info->SetList("scripts", scripts);
    }
    CefBrowserHost::CreateBrowser(wi, this, url, bs, info, nullptr);
}

void CefLayer::reset() {
    // Double-call guard: already tearing down or awaiting the replacement.
    if (state_ != State::Normal) return;

    // One-shot: when the current browser finishes closing, spin up a fresh
    // one with no URL. A blank browser has no origin state from the old one.
    // OnBeforeClose fires synchronously from within CEF's destroy chain, so
    // we MUST defer the CreateBrowser — calling it inline reenters CEF while
    // WebContents is mid-destroy and crashes inside libcef.
    CefRefPtr<CefLayer> self(this);
    setBeforeCloseCallback([self]() {
        // OnBeforeClose already moved this callback out of on_before_close_,
        // so we don't need to (and must not) clear it ourselves.
        CefPostTask(TID_UI, CefRefPtr<CefTask>(new FnTask([self]() {
            // Go through create() so requested_url_ is cleared alongside the
            // actual CreateBrowser call.
            self->create("");
        })));
    });

    if (browser_) {
        state_ = State::Recreating;
        browser_->GetHost()->CloseBrowser(true);
    } else {
        // Initial create still in flight. Defer the close to OnAfterCreated.
        state_ = State::PendingReset;
    }
}

void CefLayer::loadUrl(const std::string& url) {
    // If a reset is in flight or the initial create hasn't completed, buffer
    // the URL and let OnAfterCreated apply it when the browser is ready.
    if (state_ != State::Normal || !browser_) {
        pending_url_ = url;
        return;
    }
    browser_->GetMainFrame()->LoadURL(url);
}

void CefLayer::waitForClose() {
    std::unique_lock<std::mutex> lock(close_mtx_);
    close_cv_.wait(lock, [this] { return closed_.load(); });
}

void CefLayer::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int code) {
    LOG_INFO(LOG_CEF, "CefLayer::OnLoadEnd name={} main={} code={} url={}",
             name_.c_str(), frame->IsMain() ? 1 : 0, code,
             frame->GetURL().ToString().c_str());
    if (frame->IsMain()) {
        loaded_ = true;
        load_cv_.notify_all();
    }
}

void CefLayer::waitForLoad() {
    std::unique_lock<std::mutex> lock(load_mtx_);
    load_cv_.wait(lock, [this] { return loaded_.load(); });
}

void CefLayer::OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                           ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) {
    LOG_ERROR(LOG_CEF, "OnLoadError name={} url={} error={} {}",
              name_.c_str(), failedUrl.ToString(), static_cast<int>(errorCode), errorText.ToString());
}

void CefLayer::OnFullscreenModeChange(CefRefPtr<CefBrowser>, bool fullscreen) {
    g_platform.set_fullscreen(fullscreen);
}

bool CefLayer::OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                              cef_cursor_type_t type, const CefCursorInfo&) {
    g_platform.set_cursor(type);
    return true;
}

bool CefLayer::OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level,
                                const CefString& message, const CefString& source,
                                int line) {
    std::string msg = message.ToString();
    std::string src = source.ToString();
    // CEF: VERBOSE/DEBUG share a value. DEFAULT (0) → treat as INFO.
    if (level >= LOGSEVERITY_ERROR)
        LOG_ERROR(LOG_JS, "{} ({}:{})", msg.c_str(), src.c_str(), line);
    else if (level == LOGSEVERITY_WARNING)
        LOG_WARN(LOG_JS, "{} ({}:{})", msg.c_str(), src.c_str(), line);
    else if (level == LOGSEVERITY_INFO || level == LOGSEVERITY_DEFAULT)
        LOG_INFO(LOG_JS, "{} ({}:{})", msg.c_str(), src.c_str(), line);
    else  // VERBOSE/DEBUG
        LOG_DEBUG(LOG_JS, "{} ({}:{})", msg.c_str(), src.c_str(), line);
    return true;
}

void CefLayer::execJs(const std::string& js) {
    if (browser_ && browser_->GetMainFrame())
        browser_->GetMainFrame()->ExecuteJavaScript(js, "", 0);
}

bool CefLayer::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                                        CefProcessId, CefRefPtr<CefProcessMessage> message) {
    auto name = message->GetName().ToString();
    auto args = message->GetArgumentList();

    if (name == "popupOptions") {
        CefRefPtr<CefListValue> list = args->GetList(0);
        popup_options_.clear();
        if (list) {
            size_t n = list->GetSize();
            popup_options_.reserve(n);
            for (size_t i = 0; i < n; i++)
                popup_options_.push_back(list->GetString(i).ToString());
        }
        popup_selected_idx_ = args->GetInt(1);
        popup_options_received_ = true;
        try_show_popup();
        return true;
    }

// Context menu commands are browser-level, handled here.
    if (name == "menuItemSelected") {
        int cmd = args->GetInt(0);
        if (pending_menu_callback_) {
            pending_menu_callback_->Cancel();
            pending_menu_callback_ = nullptr;
        }
        if (browser_) {
            auto frame = focused_or_main(browser_);
            switch (cmd) {
            case MENU_ID_BACK: browser_->GoBack(); break;
            case MENU_ID_FORWARD: browser_->GoForward(); break;
            case MENU_ID_RELOAD: browser_->Reload(); break;
            case MENU_ID_RELOAD_NOCACHE: browser_->ReloadIgnoreCache(); break;
            case MENU_ID_STOPLOAD: browser_->StopLoad(); break;
            case MENU_ID_UNDO: frame->Undo(); break;
            case MENU_ID_REDO: frame->Redo(); break;
            case MENU_ID_CUT: frame->Cut(); break;
            case MENU_ID_COPY: frame->Copy(); break;
            case MENU_ID_PASTE: do_paste(browser_, frame); break;
            case MENU_ID_SELECT_ALL: frame->SelectAll(); break;
            default:
                if (context_menu_dispatcher_) context_menu_dispatcher_(cmd);
                break;
            }
        }
        return true;
    } else if (name == "menuDismissed") {
        if (pending_menu_callback_) {
            pending_menu_callback_->Cancel();
            pending_menu_callback_ = nullptr;
        }
        return true;
    }

    // Everything else delegates to the business logic handler.
    if (message_handler_)
        return message_handler_(name, args, browser);
    return false;
}

bool CefLayer::OnPreKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent& e,
                             CefEventHandle, bool*) {
    return try_intercept_paste(browser, e);
}

void CefLayer::OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                   CefRefPtr<CefContextMenuParams>,
                                   CefRefPtr<CefMenuModel> model) {
    model->Remove(MENU_ID_PRINT);
    model->Remove(MENU_ID_VIEW_SOURCE);
    if (model->GetIndexOf(MENU_ID_RELOAD) < 0)
        model->AddItem(MENU_ID_RELOAD, "Reload");
    while (model->GetCount() > 0 &&
           model->GetTypeAt(model->GetCount() - 1) == MENUITEMTYPE_SEPARATOR)
        model->RemoveAt(model->GetCount() - 1);
    if (context_menu_builder_) {
        model->AddSeparator();
        context_menu_builder_(model);
    }
}

bool CefLayer::RunContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                              CefRefPtr<CefContextMenuParams> params,
                              CefRefPtr<CefMenuModel> model,
                              CefRefPtr<CefRunContextMenuCallback> callback) {
    if (model->GetCount() == 0) {
        callback->Cancel();
        return true;
    }
    if (pending_menu_callback_) pending_menu_callback_->Cancel();
    pending_menu_callback_ = callback;

    cJSON* call_args = cJSON_CreateArray();
    cJSON_AddItemToArray(call_args, serializeMenuModel(model));
    cJSON_AddItemToArray(call_args, cJSON_CreateNumber(params->GetXCoord()));
    cJSON_AddItemToArray(call_args, cJSON_CreateNumber(params->GetYCoord()));
    char* json = cJSON_PrintUnformatted(call_args);
    browser->GetMainFrame()->ExecuteJavaScript(
        "window._showContextMenu.apply(null," + std::string(json) + ")",
        "", 0);
    cJSON_free(json);
    cJSON_Delete(call_args);
    return true;
}
