#include "cef_client.h"
#include "logging.h"
#include "../browser/browsers.h"
#include "../platform/platform.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "include/cef_values.h"
#include <cstdio>
#include <functional>

namespace {
// Small CefTask adapter for the two remaining C++-scheduled tasks (popup
// dispatch + reset deferred-create). Slices 4/5 migrate both to Rust.
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

static CefRefPtr<CefListValue> serializeMenuModel(CefRefPtr<CefMenuModel> model) {
    CefRefPtr<CefListValue> arr = CefListValue::Create();
    for (size_t i = 0; i < model->GetCount(); i++) {
        CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
        auto type = model->GetTypeAt(i);
        if (type == MENUITEMTYPE_SEPARATOR) {
            item->SetBool("sep", true);
        } else {
            int id = model->GetCommandIdAt(i);
            std::string label = stripAccelerator(model->GetLabelAt(i).ToString());
            item->SetInt("id", id);
            item->SetString("label", label);
            item->SetBool("enabled", model->IsEnabledAt(i));
        }
        arr->SetDictionary(arr->GetSize(), item);
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
    CefRefPtr<CefValue> v = CefValue::Create();
    v->SetString(text);
    // CefWriteJSON requires a dictionary/list root; wrap the string in a
    // 1-element list and strip the brackets to recover a bare JSON string.
    CefRefPtr<CefListValue> wrapper = CefListValue::Create();
    wrapper->SetValue(0, v);
    CefRefPtr<CefValue> root = CefValue::Create();
    root->SetList(wrapper);
    std::string s = CefWriteJSON(root, JSON_WRITER_DEFAULT).ToString();
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']')
        return s.substr(1, s.size() - 2);
    return "\"\"";
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

// =====================================================================
// CEF browser op thunks — invoked from Rust (jfn_cef::client::Inner) on
// TID_UI when scheduling resize / invalidate / frame-rate work.
// =====================================================================

namespace {

void ops_notify_screen_info_changed(void* c) {
    auto* self = static_cast<CefLayer*>(c);
    if (auto b = self->browser()) b->GetHost()->NotifyScreenInfoChanged();
}
void ops_was_resized(void* c) {
    auto* self = static_cast<CefLayer*>(c);
    if (auto b = self->browser()) b->GetHost()->WasResized();
}
void ops_invalidate(void* c) {
    auto* self = static_cast<CefLayer*>(c);
    if (auto b = self->browser()) b->GetHost()->Invalidate(PET_VIEW);
}
void ops_send_external_begin_frame(void* c) {
#ifdef __APPLE__
    auto* self = static_cast<CefLayer*>(c);
    if (auto b = self->browser()) b->GetHost()->SendExternalBeginFrame();
#else
    (void)c;
#endif
}
void ops_set_windowless_frame_rate(void* c, int hz) {
    auto* self = static_cast<CefLayer*>(c);
    if (auto b = self->browser()) b->GetHost()->SetWindowlessFrameRate(hz);
}
void ops_exec_js(void* c, const char* js_utf8, size_t len) {
    auto* self = static_cast<CefLayer*>(c);
    auto b = self->browser();
    if (!b) return;
    if (auto f = b->GetMainFrame())
        f->ExecuteJavaScript(CefString(std::string(js_utf8, len)), "", 0);
}
void ops_send_process_message_named(void* c, const char* name_utf8, size_t len) {
    auto* self = static_cast<CefLayer*>(c);
    auto b = self->browser();
    if (!b) return;
    auto frame = focused_or_main(b);
    if (!frame) return;
    auto msg = CefProcessMessage::Create(CefString(std::string(name_utf8, len)));
    frame->SendProcessMessage(PID_RENDERER, msg);
}
void ops_dispatch_popup_selection(void* c, int index) {
    auto* self = static_cast<CefLayer*>(c);
    if (self->isClosed()) return;
    auto b = self->browser();
    if (!b) return;
    if (auto frame = focused_or_main(b)) {
        auto msg = CefProcessMessage::Create("applyPopupSelection");
        msg->GetArgumentList()->SetInt(0, index);
        frame->SendProcessMessage(PID_RENDERER, msg);
    }
    // Only public path to CancelWidget on a CEF OSR popup is a mouse-wheel
    // event outside popup_position_ — render_widget_host_view_osr.cc:1337-1343.
    CefMouseEvent me{};
    me.x = -1;
    me.y = -1;
    b->GetHost()->SendMouseWheelEvent(me, /*deltaX=*/0, /*deltaY=*/1);
}

}  // namespace

CefLayer::CefLayer(Browsers& browsers, PlatformSurface* surface)
    : browsers_(browsers), surface_(surface), rs_(jfn_cef_layer_new()) {
    JfnCefBrowserOps ops{};
    ops.ctx = this;
    ops.notify_screen_info_changed = ops_notify_screen_info_changed;
    ops.was_resized = ops_was_resized;
    ops.invalidate = ops_invalidate;
    ops.send_external_begin_frame = ops_send_external_begin_frame;
    ops.set_windowless_frame_rate = ops_set_windowless_frame_rate;
    ops.exec_js = ops_exec_js;
    ops.send_process_message_named = ops_send_process_message_named;
    ops.dispatch_popup_selection = ops_dispatch_popup_selection;
    jfn_cef_layer_set_browser_ops(rs_, &ops);
    jfn_cef_layer_set_surface(rs_, surface);
}

CefLayer::~CefLayer() {
    // Clear ops before freeing rs_ so any in-flight TID_UI task that runs
    // after destruction sees None and exits without touching this CefLayer.
    jfn_cef_layer_clear_browser_ops(rs_);
    jfn_cef_layer_free(rs_);
}

void CefLayer::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) {
    int w = 0, h = 0;
    jfn_cef_layer_get_view_rect(rs_, &w, &h);
    rect.Set(0, 0, w, h);
}

bool CefLayer::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) {
    float scale = 1.0f;
    int w = 0, h = 0;
    jfn_cef_layer_get_screen_info(rs_, &scale, &w, &h);
    info.device_scale_factor = scale;
    info.rect = CefRect(0, 0, w, h);
    info.available_rect = info.rect;
    return true;
}

void CefLayer::resize(int w, int h, int physical_w, int physical_h) {
    LOG_TRACE(LOG_CEF, "CefLayer::resize name={} logical={}x{} physical={}x{}",
             name_.c_str(), w, h, physical_w, physical_h);
    jfn_cef_layer_resize(rs_, w, h, physical_w, physical_h);
}

void CefLayer::kickInvalidateLoop() {
    jfn_cef_layer_kick_invalidate_loop(rs_);
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
    jfn_cef_layer_set_refresh_rate(rs_, hz);
}

void CefLayer::OnPopupShow(CefRefPtr<CefBrowser>, bool show) {
    jfn_cef_layer_on_popup_show(rs_, show);
}

void CefLayer::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) {
    jfn_cef_layer_on_popup_size(rs_, rect.x, rect.y, rect.width, rect.height);
}

void CefLayer::onDeactivated() {
    jfn_cef_layer_on_deactivated(rs_);
}

void CefLayer::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirty,
                       const void* buffer, int w, int h) {
    if (type != PET_POPUP && type != PET_VIEW) return;
    std::vector<JfnRect> rects;
    rects.reserve(dirty.size());
    for (const auto& r : dirty)
        rects.push_back({r.x, r.y, r.width, r.height});
    jfn_cef_layer_on_paint(rs_, type == PET_POPUP,
                           rects.data(), rects.size(), buffer, w, h);
}

void CefLayer::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                                  const RectList&, const CefAcceleratedPaintInfo& info) {
    if (type != PET_POPUP && type != PET_VIEW) return;
    jfn_cef_layer_on_accelerated_paint(rs_, type == PET_POPUP, &info);
}

void CefLayer::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    LOG_DEBUG(LOG_CEF, "CefLayer::OnAfterCreated name={}", name_.c_str());
    browser_ = browser;
    jfn_cef_layer_set_closed(rs_, false);
    jfn_cef_layer_set_loaded(rs_, false);
    if (g_shutting_down.load(std::memory_order_relaxed)) {
        browser->GetHost()->CloseBrowser(true);
        return;
    }
    // WasResized fires here, so bump gen so should_present_paint recomputes
    // skip/pump from frame_rate on the first paint.
    jfn_cef_layer_bump_resize_gen(rs_);
    browser->GetHost()->NotifyScreenInfoChanged();
    browser->GetHost()->WasResized();
    browser->GetHost()->Invalidate(PET_VIEW);
    jfn_cef_layer_kick_invalidate_loop(rs_);

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
    // Signal the nudge loop to exit so the posted-task Arc clones keeping
    // Rust state alive can drop and the layer can finish destruction.
    jfn_cef_layer_stop_invalidate(rs_);
    jfn_cef_layer_set_closed(rs_, true);
    jfn_cef_layer_set_loaded(rs_, true);
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
    int initial_fr = jfn_cef_layer_frame_rate(rs_);
    bs.windowless_frame_rate = initial_fr > 0 ? initial_fr : browsers_.frame_rate();

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
            // actual CreateBrowser call. Skip during shutdown: CefShutdown()
            // drains pending tasks, and creating a browser here would race
            // with the shutdown teardown and cause a hang.
            if (!g_shutting_down.load(std::memory_order_relaxed))
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

void CefLayer::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int code) {
    LOG_INFO(LOG_CEF, "CefLayer::OnLoadEnd name={} main={} code={} url={}",
             name_.c_str(), frame->IsMain() ? 1 : 0, code,
             frame->GetURL().ToString().c_str());
    if (frame->IsMain())
        jfn_cef_layer_set_loaded(rs_, true);
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
        std::vector<std::string> opts;
        std::vector<const char*> opt_ptrs;
        if (list) {
            size_t n = list->GetSize();
            opts.reserve(n);
            opt_ptrs.reserve(n);
            for (size_t i = 0; i < n; i++)
                opts.push_back(list->GetString(i).ToString());
            for (const auto& s : opts)
                opt_ptrs.push_back(s.c_str());
        }
        jfn_cef_layer_set_popup_options(rs_,
            opt_ptrs.empty() ? nullptr : opt_ptrs.data(),
            opt_ptrs.size(),
            args->GetInt(1));
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

    CefRefPtr<CefListValue> call_args = CefListValue::Create();
    call_args->SetList(0, serializeMenuModel(model));
    call_args->SetInt(1, params->GetXCoord());
    call_args->SetInt(2, params->GetYCoord());
    CefRefPtr<CefValue> root = CefValue::Create();
    root->SetList(call_args);
    std::string json = CefWriteJSON(root, JSON_WRITER_DEFAULT).ToString();
    browser->GetMainFrame()->ExecuteJavaScript(
        "window._showContextMenu.apply(null," + json + ")",
        "", 0);
    return true;
}
