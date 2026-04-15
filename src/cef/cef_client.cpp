#include "cef_client.h"
#include "logging.h"
#include "../cjson/cJSON.h"
#include "../platform/platform.h"
#include <cstdio>

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
    if (!browser) return;
    auto frame = browser->GetFocusedFrame();
    if (!frame) frame = browser->GetMainFrame();
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
    LOG_INFO(LOG_CEF, "CefLayer::resize logical={}x{} physical={}x{} browser={}",
             w, h, physical_w, physical_h, static_cast<void*>(browser_.get()));
    width_ = w;
    height_ = h;
    physical_w_ = physical_w;
    physical_h_ = physical_h;
    if (browser_) {
        browser_->GetHost()->NotifyScreenInfoChanged();
        browser_->GetHost()->WasResized();
        browser_->GetHost()->Invalidate(PET_VIEW);
    }
}

void CefLayer::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirty,
                       const void* buffer, int w, int h) {
    if (type != PET_VIEW) return;
    target_.present_software(dirty, buffer, w, h);
}

void CefLayer::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                                  const RectList&, const CefAcceleratedPaintInfo& info) {
    if (type != PET_VIEW) return;
    target_.present(info);
}

void CefLayer::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    LOG_INFO(LOG_CEF, "CefLayer::OnAfterCreated browser={} id={}",
             static_cast<void*>(browser.get()), browser ? browser->GetIdentifier() : -1);
    browser_ = browser;
    if (g_shutting_down.load(std::memory_order_relaxed)) {
        browser->GetHost()->CloseBrowser(true);
        return;
    }
    browser->GetHost()->NotifyScreenInfoChanged();
    browser->GetHost()->WasResized();
    browser->GetHost()->Invalidate(PET_VIEW);
    if (on_after_created_) on_after_created_(browser);
}

void CefLayer::OnBeforeClose(CefRefPtr<CefBrowser>) {
    browser_ = nullptr;
    closed_ = true;
    loaded_ = true;
    close_cv_.notify_all();
    load_cv_.notify_all();
}

void CefLayer::waitForClose() {
    std::unique_lock<std::mutex> lock(close_mtx_);
    close_cv_.wait(lock, [this] { return closed_.load(); });
}

void CefLayer::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int code) {
    LOG_INFO(LOG_CEF, "CefLayer::OnLoadEnd main={} code={} url={}",
             frame->IsMain() ? 1 : 0, code,
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
    LOG_ERROR(LOG_CEF, "OnLoadError: {} error={} {}",
              failedUrl.ToString(), static_cast<int>(errorCode), errorText.ToString());
}

void CefLayer::OnFullscreenModeChange(CefRefPtr<CefBrowser>, bool fullscreen) {
    g_platform.set_fullscreen(fullscreen);
}

bool CefLayer::OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                              cef_cursor_type_t type, const CefCursorInfo&) {
    g_platform.set_cursor(type);
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

    // Context menu commands are browser-level, handled here.
    if (name == "menuItemSelected") {
        int cmd = args->GetInt(0);
        if (pending_menu_callback_) {
            pending_menu_callback_->Cancel();
            pending_menu_callback_ = nullptr;
        }
        if (browser_) {
            auto frame = browser_->GetFocusedFrame();
            if (!frame) frame = browser_->GetMainFrame();
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
            default: break;
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
