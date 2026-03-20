#include "cef/cef_client.h"
#include "ui/menu_overlay.h"
#include "settings.h"
#include "input/sdl_to_vk.h"
#include "include/cef_urlrequest.h"
#include "include/cef_parser.h"
#include <SDL3/SDL.h>
#include "logging.h"
#include <mutex>
#include <cctype>
#if !defined(__APPLE__) && !defined(_WIN32)
#include <unistd.h>  // For dup()
#endif

namespace {

char convertSchemeCharToLower(unsigned char c) {
    return static_cast<char>(std::tolower(c));
}

// Launch URL via the OS default handler (browser).
bool openUrlExternally(const std::string& url) {
    if (url.empty()) return false;
    if (!SDL_OpenURL(url.c_str())) {
        LOG_WARN(LOG_CEF, "Failed to open external URL: %s", url.c_str());
        return false;
    }
    LOG_DEBUG(LOG_CEF, "Opened external URL: %s", url.c_str());
    return true;
}

// Decide whether a popup target should be delegated to the OS instead of CEF.
bool shouldOpenPopupExternally(const std::string& url) {
    if (url.empty()) return false;

    CefURLParts parts;
    if (!CefParseURL(url, parts)) {
        return false;
    }

    std::string scheme = CefString(&parts.scheme).ToString();
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), convertSchemeCharToLower);

    // Security policy: only delegate HTTPS URLs to the OS (default browser).
    // If broader scheme support is desired, extend the check below explicitly.
    return scheme == "https";
}

// Apply popup policy and log decisions in one place for both clients.
void handlePopupNavigation(const std::string& url, const char* source_tag) {
    if (shouldOpenPopupExternally(url)) {
        openUrlExternally(url);
    } else {
        LOG_WARN(LOG_CEF, "%s blocked popup URL: %s", source_tag, url.c_str());
    }
}

void applySettingValue(const std::string& section, const std::string& key, const std::string& value) {
    auto& s = Settings::instance();
    if (key == "hwdec") s.setHwdec(value);
    else if (key == "audioPassthrough") s.setAudioPassthrough(value);
    else if (key == "audioExclusive") s.setAudioExclusive(value == "true");
    else if (key == "audioChannels") s.setAudioChannels(value);
    else if (key == "disableGpuCompositing") s.setDisableGpuCompositing(value == "true");
    else if (key == "titlebarThemeColor") s.setTitlebarThemeColor(value == "true");
    else if (key == "transparentTitlebar") s.setTransparentTitlebar(value == "true");
    else if (key == "logLevel") s.setLogLevel(value);
    else LOG_WARN(LOG_CEF, "Unknown setting key: %s.%s", section.c_str(), key.c_str());
    s.saveAsync();
}

void doCopy(CefRefPtr<CefBrowser> browser, bool cut) {
    if (!browser) return;
    auto frame = browser->GetFocusedFrame();
    if (!frame) frame = browser->GetMainFrame();
    if (!frame) return;
    std::string js = cut ?
        R"((function() {
            const text = window.getSelection().toString();
            if (text) {
                window.jmpNative?.setClipboard?.('text/plain', btoa(text));
            }
            document.execCommand('delete');
        })();)" :
        R"((function() {
            const el = document.activeElement;
            let text = '';
            if (el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA')) {
                text = el.value.substring(el.selectionStart, el.selectionEnd);
            } else {
                text = window.getSelection().toString();
            }
            if (text) {
                window.jmpNative?.setClipboard?.('text/plain', btoa(text));
            }
        })();)";
    frame->ExecuteJavaScript(js, "", 0);
}

void doPaste(CefRefPtr<CefBrowser> browser, const char* mimeType, const void* data, size_t len) {
    if (!browser || !data || len == 0) return;
    auto frame = browser->GetFocusedFrame();
    if (!frame) frame = browser->GetMainFrame();
    if (!frame) return;
    std::string b64Data = CefBase64Encode(data, len).ToString();
    std::string b64Mime = CefBase64Encode(mimeType, strlen(mimeType)).ToString();

    std::string js = R"((function() {
        const b64 = ')" + b64Data + R"(';
        const mime = atob(')" + b64Mime + R"(');
        const binary = atob(b64);
        const bytes = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);

        // For text, use execCommand which works reliably in inputs
        if (mime.startsWith('text/')) {
            const text = new TextDecoder().decode(bytes);
            document.execCommand('insertText', false, text);
            return;
        }

        // For binary (images etc), dispatch ClipboardEvent
        const blob = new Blob([bytes], {type: mime});
        const dt = new DataTransfer();
        dt.items.add(new File([blob], 'paste', {type: mime}));
        const event = new ClipboardEvent('paste', {
            clipboardData: dt,
            bubbles: true,
            cancelable: true
        });
        document.activeElement.dispatchEvent(event);
    })();)";
    frame->ExecuteJavaScript(js, "", 0);
}

struct ClipboardData {
    std::mutex mutex;
    std::string mimeType;
    std::vector<unsigned char> data;
};
static ClipboardData g_clipboard;

const void* clipboardCallback(void*, const char* mime_type, size_t* size) {
    std::lock_guard<std::mutex> lock(g_clipboard.mutex);
    if (g_clipboard.mimeType == mime_type) {
        *size = g_clipboard.data.size();
        return g_clipboard.data.data();
    }
    *size = 0;
    return nullptr;
}

void clipboardCleanup(void*) {
    std::lock_guard<std::mutex> lock(g_clipboard.mutex);
    g_clipboard.data.clear();
    g_clipboard.mimeType.clear();
}

bool handleSetClipboard(CefRefPtr<CefListValue> args) {
    std::string mimeType = args->GetString(0).ToString();
    std::string b64 = args->GetString(1).ToString();
    CefRefPtr<CefBinaryValue> decoded = CefBase64Decode(b64);
    if (!decoded) {
        LOG_ERROR(LOG_CEF, "Clipboard base64 decode failed");
        return true;
    }

    if (mimeType.rfind("text/", 0) == 0) {
        std::string text(decoded->GetSize(), '\0');
        decoded->GetData(text.data(), text.size(), 0);
        SDL_SetClipboardText(text.c_str());
    } else {
        {
            std::lock_guard<std::mutex> lock(g_clipboard.mutex);
            g_clipboard.mimeType = mimeType;
            g_clipboard.data.resize(decoded->GetSize());
            decoded->GetData(g_clipboard.data.data(), g_clipboard.data.size(), 0);
        }
        const char* mimeTypes[] = { mimeType.c_str() };
        SDL_SetClipboardData(clipboardCallback, clipboardCleanup, nullptr, mimeTypes, 1);
    }
    return true;
}

void handleGetClipboard(CefRefPtr<CefBrowser> browser, CefRefPtr<CefListValue> args) {
    if (!browser) return;
    std::string mimeType = args->GetString(0).ToString();
    std::string b64;
    size_t len = 0;
    void* data = SDL_GetClipboardData(mimeType.c_str(), &len);
    if (data && len > 0) {
        b64 = CefBase64Encode(data, len).ToString();
        SDL_free(data);
    }
    CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("clipboardResult");
    msg->GetArgumentList()->SetString(0, mimeType);
    msg->GetArgumentList()->SetString(1, b64);
    browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
}
} // namespace

// URL request client for server connectivity checks
class ConnectivityURLRequestClient : public CefURLRequestClient {
public:
    ConnectivityURLRequestClient(CefRefPtr<CefBrowser> browser, const std::string& originalUrl)
        : browser_(browser), original_url_(originalUrl) {}

    void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
        auto status = request->GetRequestStatus();
        auto response = request->GetResponse();

        bool success = false;
        std::string resolved_url = original_url_;

        if (status == UR_SUCCESS && response && response->GetStatus() == 200) {
            // Check if we got valid JSON with an "Id" field
            if (response_body_.find("\"Id\"") != std::string::npos) {
                success = true;
                // Use the final URL after redirects
                resolved_url = response->GetURL().ToString();
                // Strip /System/Info/Public to get base URL
                size_t pos = resolved_url.find("/System/Info/Public");
                if (pos != std::string::npos) {
                    resolved_url = resolved_url.substr(0, pos);
                }
            }
        }

        LOG_INFO(LOG_CEF, "Connectivity request complete: %s url=%s",
                 success ? "success" : "failed", resolved_url.c_str());

        // Send result back to renderer (if browser still valid)
        auto frame = browser_ ? browser_->GetMainFrame() : nullptr;
        if (frame) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("serverConnectivityResult");
            msg->GetArgumentList()->SetString(0, original_url_);
            msg->GetArgumentList()->SetBool(1, success);
            msg->GetArgumentList()->SetString(2, resolved_url);
            frame->SendProcessMessage(PID_RENDERER, msg);
        } else {
            LOG_DEBUG(LOG_CEF, "Connectivity result dropped - browser closed");
        }
    }

    void OnUploadProgress(CefRefPtr<CefURLRequest> request, int64_t current, int64_t total) override {}
    void OnDownloadProgress(CefRefPtr<CefURLRequest> request, int64_t current, int64_t total) override {}

    void OnDownloadData(CefRefPtr<CefURLRequest> request, const void* data, size_t data_length) override {
        response_body_.append(static_cast<const char*>(data), data_length);
    }

    bool GetAuthCredentials(bool isProxy, const CefString& host, int port,
                           const CefString& realm, const CefString& scheme,
                           CefRefPtr<CefAuthCallback> callback) override {
        return false;
    }

private:
    CefRefPtr<CefBrowser> browser_;
    std::string original_url_;
    std::string response_body_;

    IMPLEMENT_REFCOUNTING(ConnectivityURLRequestClient);
};

Client::Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg,
               AcceleratedPaintCallback on_accel_paint, MenuOverlay* menu,
               CursorChangeCallback on_cursor_change, FullscreenChangeCallback on_fullscreen_change,
               PhysicalSizeCallback physical_size_cb, ThemeColorCallback on_theme_color,
               OsdVisibleCallback on_osd_visible,
               PopupShowCallback on_popup_show, PopupSizeCallback on_popup_size,
               AcceleratedPaintCallback on_accel_popup_paint
#ifdef __APPLE__
               , IOSurfacePaintCallback on_iosurface_paint
#endif
#ifdef _WIN32
               , WinSharedTexturePaintCallback on_win_shared_paint
#endif
               )
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_player_msg_(std::move(on_player_msg)),
      on_accel_paint_(std::move(on_accel_paint)),
      on_popup_show_(std::move(on_popup_show)),
      on_popup_size_(std::move(on_popup_size)),
      on_accel_popup_paint_(std::move(on_accel_popup_paint)),
#ifdef __APPLE__
      on_iosurface_paint_(std::move(on_iosurface_paint)),
#endif
#ifdef _WIN32
      on_win_shared_paint_(std::move(on_win_shared_paint)),
#endif
      menu_(menu), on_cursor_change_(std::move(on_cursor_change)),
      on_fullscreen_change_(std::move(on_fullscreen_change)),
      on_theme_color_(std::move(on_theme_color)),
      on_osd_visible_(std::move(on_osd_visible)),
      physical_size_cb_(std::move(physical_size_cb)) {}

bool Client::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                               cef_log_severity_t level,
                               const CefString& message,
                               const CefString& source,
                               int line) {
    (void)browser; (void)level; (void)source; (void)line;
    LOG_DEBUG(LOG_JS_MAIN, "%s", message.ToString().c_str());
    return false;  // Allow default handling too
}

bool Client::OnCursorChange(CefRefPtr<CefBrowser> browser,
                            CefCursorHandle cursor,
                            cef_cursor_type_t type,
                            const CefCursorInfo& custom_cursor_info) {
    if (on_cursor_change_) {
        on_cursor_change_(type);
    }
    return true;  // We handled it
}

void Client::OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) {
    LOG_INFO(LOG_CEF, "OnFullscreenModeChange: %s", fullscreen ? "enter" : "exit");
    if (on_fullscreen_change_) {
        on_fullscreen_change_(fullscreen);
    }
}

bool Client::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       CefProcessId source_process,
                                       CefRefPtr<CefProcessMessage> message) {
    std::string name = message->GetName().ToString();
    CefRefPtr<CefListValue> args = message->GetArgumentList();

    LOG_DEBUG(LOG_CEF, "IPC received message: %s", name.c_str());

    if (name == "themeColor") {
        std::string color = args->GetString(0).ToString();
        if (on_theme_color_) {
            on_theme_color_(color);
        }
        return true;
    }

    if (name == "osdVisible") {
        bool visible = args->GetBool(0);
        if (on_osd_visible_) {
            on_osd_visible_(visible);
        }
        return true;
    }

    if (!on_player_msg_) return false;

    if (name == "playerLoad") {
        std::string url = args->GetString(0).ToString();
        int startMs = args->GetSize() > 1 ? args->GetInt(1) : 0;
        int audioIdx = args->GetSize() > 2 ? args->GetInt(2) : -1;
        int subIdx = args->GetSize() > 3 ? args->GetInt(3) : -1;
        std::string metadata = args->GetSize() > 4 ? args->GetString(4).ToString() : "{}";
        // Encode track indices in metadata JSON
        std::string prefix = "{";
        if (audioIdx >= 0) prefix += "\"_audioIdx\":" + std::to_string(audioIdx) + ",";
        if (subIdx >= 0) prefix += "\"_subIdx\":" + std::to_string(subIdx) + ",";
        if (prefix.size() > 1 && metadata.size() > 1) {
            metadata = prefix + metadata.substr(1);
        }
        on_player_msg_("load", url, startMs, metadata);
        return true;
    } else if (name == "playerStop") {
        LOG_DEBUG(LOG_CEF, "Calling on_player_msg_ for stop");
        on_player_msg_("stop", "", 0, "");
        LOG_DEBUG(LOG_CEF, "on_player_msg_ returned for stop");
        return true;
    } else if (name == "playerPause") {
        on_player_msg_("pause", "", 0, "");
        return true;
    } else if (name == "playerPlay") {
        on_player_msg_("play", "", 0, "");
        return true;
    } else if (name == "playerSeek") {
        int ms = args->GetInt(0);
        on_player_msg_("seek", "", ms, "");
        return true;
    } else if (name == "playerSetVolume") {
        int vol = args->GetInt(0);
        on_player_msg_("volume", "", vol, "");
        return true;
    } else if (name == "playerSetMuted") {
        bool muted = args->GetBool(0);
        on_player_msg_("mute", "", muted ? 1 : 0, "");
        return true;
    } else if (name == "playerSetSpeed") {
        int rateX1000 = args->GetInt(0);
        on_player_msg_("speed", "", rateX1000, "");
        return true;
    } else if (name == "playerSetSubtitle") {
        int sid = args->GetInt(0);
        on_player_msg_("subtitle", "", sid, "");
        return true;
    } else if (name == "playerSetAudio") {
        int aid = args->GetInt(0);
        on_player_msg_("audio", "", aid, "");
        return true;
    } else if (name == "playerSetAudioDelay") {
        double delay = args->GetDouble(0);
        on_player_msg_("audioDelay", "", 0, std::to_string(delay));
        return true;
    } else if (name == "saveServerUrl") {
        std::string url = args->GetString(0).ToString();
        LOG_INFO(LOG_CEF, "IPC saving server URL: %s", url.c_str());
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
        return true;
    } else if (name == "setSettingValue") {
        std::string section = args->GetString(0).ToString();
        std::string key = args->GetString(1).ToString();
        std::string value = args->GetString(2).ToString();
        LOG_INFO(LOG_CEF, "IPC setSettingValue: %s.%s = %s", section.c_str(), key.c_str(), value.c_str());
        applySettingValue(section, key, value);
        return true;
    } else if (name == "notifyMetadata") {
        std::string metadata = args->GetString(0).ToString();
        on_player_msg_("media_metadata", metadata, 0, "");
        return true;
    } else if (name == "notifyPosition") {
        int posMs = args->GetInt(0);
        on_player_msg_("media_position", "", posMs, "");
        return true;
    } else if (name == "notifySeek") {
        int posMs = args->GetInt(0);
        on_player_msg_("media_seeked", "", posMs, "");
        return true;
    } else if (name == "notifyPlaybackState") {
        std::string state = args->GetString(0).ToString();
        on_player_msg_("media_state", state, 0, "");
        return true;
    } else if (name == "notifyArtwork") {
        std::string artworkUri = args->GetString(0).ToString();
        on_player_msg_("media_artwork", artworkUri, 0, "");
        return true;
    } else if (name == "notifyQueueChange") {
        bool canNext = args->GetBool(0);
        bool canPrev = args->GetBool(1);
        // Encode both bools in intArg: bit 0 = canNext, bit 1 = canPrev
        int flags = (canNext ? 1 : 0) | (canPrev ? 2 : 0);
        on_player_msg_("media_queue", "", flags, "");
        return true;
    } else if (name == "notifyRateChange") {
        double rate = args->GetDouble(0);
        // Use the rate * 1000000 to pass as int (microseconds precision equivalent)
        // We'll decode this in main.cpp
        on_player_msg_("media_notify_rate", "", static_cast<int>(rate * 1000000), "");
        return true;
    } else if (name == "setClipboard") {
        return handleSetClipboard(args);
    } else if (name == "getClipboard") {
        handleGetClipboard(browser, args);
        return true;
    } else if (name == "appExit") {
        LOG_INFO(LOG_CEF, "App exit requested from web UI");
        SDL_Event qe{};
        qe.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&qe);
        return true;
    }

    return false;
}

void Client::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    // Return logical size - CEF multiplies by device_scale_factor for physical paint buffer
    static bool first = true;
    if (first) {
        LOG_INFO(LOG_CEF, "GetViewRect: %dx%d (logical)", width_, height_);
        first = false;
    }
    rect.Set(0, 0, width_, height_);
}

bool Client::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) {
    // Compute scale from physical/logical - CEF paints at logical * scale = physical
    // Prefer stored physical dimensions (set during resize) to avoid stale SDL queries
    int physical_w = physical_w_;
    int physical_h = physical_h_;
    if (physical_w <= 0 && physical_size_cb_) {
        physical_size_cb_(physical_w, physical_h);
    }
    float scale = (physical_w > 0 && width_ > 0) ? static_cast<float>(physical_w) / width_ : 1.0f;
    screen_info.device_scale_factor = scale;
    screen_info.depth = 32;
    screen_info.depth_per_component = 8;
    screen_info.is_monochrome = false;
    screen_info.rect = CefRect(0, 0, width_, height_);  // logical coordinates
    screen_info.available_rect = screen_info.rect;
    return true;
}

void Client::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) {
    popup_visible_ = show;
    if (!show) {
        popup_buffer_.clear();
        popup_pixel_width_ = 0;
        popup_pixel_height_ = 0;
    }
    if (on_popup_show_) {
        on_popup_show_(show);
    }
}

void Client::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) {
    popup_rect_ = rect;
    if (on_popup_size_) {
        on_popup_size_(rect.x, rect.y, rect.width, rect.height);
    }
}

void Client::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                     const RectList& dirtyRects, const void* buffer,
                     int width, int height) {
    static bool first = true;
    if (first) {
        LOG_INFO(LOG_CEF, "OnPaint: %dx%d type=%s", width, height,
                 type == PET_VIEW ? "VIEW" : "POPUP");
        first = false;
    }
    if (!on_paint_) return;

    if (type == PET_POPUP) {
        size_t size = width * height * 4;
        popup_buffer_.resize(size);
        memcpy(popup_buffer_.data(), buffer, size);
        popup_pixel_width_ = width;
        popup_pixel_height_ = height;
        // Request main view repaint to composite popup
        if (browser) {
            browser->GetHost()->Invalidate(PET_VIEW);
        }
        return;
    }

    // PET_VIEW - main view
    // Fast path: no popup, pass buffer directly (zero extra copies)
    if (!popup_visible_ || popup_buffer_.empty()) {
        on_paint_(buffer, width, height);
        return;
    }

    // Slow path: blend popup onto view (only when dropdown is visible)
    size_t size = width * height * 4;
    composite_buffer_.resize(size);
    memcpy(composite_buffer_.data(), buffer, size);

    // popup_rect_ is in logical/CSS coordinates; buffers are in physical pixels.
    // Scale popup position to physical space and use actual pixel dimensions.
    float scale = (width_ > 0) ? static_cast<float>(width) / width_ : 1.0f;
    int px = static_cast<int>(popup_rect_.x * scale);
    int py = static_cast<int>(popup_rect_.y * scale);
    int pw = popup_pixel_width_;
    int ph = popup_pixel_height_;
    for (int y = 0; y < ph; y++) {
        int dst_y = py + y;
        if (dst_y < 0 || dst_y >= height) continue;
        for (int x = 0; x < pw; x++) {
            int dst_x = px + x;
            if (dst_x < 0 || dst_x >= width) continue;
            int src_i = (y * pw + x) * 4;
            int dst_i = (dst_y * width + dst_x) * 4;
            if (src_i + 3 >= static_cast<int>(popup_buffer_.size())) continue;
            uint8_t alpha = popup_buffer_[src_i + 3];
            if (alpha == 255) {
                composite_buffer_[dst_i + 0] = popup_buffer_[src_i + 0];
                composite_buffer_[dst_i + 1] = popup_buffer_[src_i + 1];
                composite_buffer_[dst_i + 2] = popup_buffer_[src_i + 2];
                composite_buffer_[dst_i + 3] = 255;
            } else if (alpha > 0) {
                uint8_t inv = 255 - alpha;
                composite_buffer_[dst_i + 0] = (popup_buffer_[src_i + 0] * alpha + composite_buffer_[dst_i + 0] * inv) / 255;
                composite_buffer_[dst_i + 1] = (popup_buffer_[src_i + 1] * alpha + composite_buffer_[dst_i + 1] * inv) / 255;
                composite_buffer_[dst_i + 2] = (popup_buffer_[src_i + 2] * alpha + composite_buffer_[dst_i + 2] * inv) / 255;
                composite_buffer_[dst_i + 3] = 255;
            }
        }
    }
    on_paint_(composite_buffer_.data(), width, height);
}

void Client::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                 const RectList& dirtyRects,
                                 const CefAcceleratedPaintInfo& info) {
#ifdef __APPLE__
    static bool first = true;
    if (first) {
        LOG_INFO(LOG_CEF, "OnAcceleratedPaint: iosurface=%p format=%d size=%dx%d",
                 info.shared_texture_io_surface, info.format,
                 info.extra.coded_size.width, info.extra.coded_size.height);
        first = false;
    }

    if (on_iosurface_paint_ && type == PET_VIEW && info.shared_texture_io_surface) {
        int w = info.extra.coded_size.width;
        int h = info.extra.coded_size.height;
        if (w > 0 && h > 0) {
            on_iosurface_paint_(info.shared_texture_io_surface, info.format, w, h);
        }
    }
#elif !defined(_WIN32)
    static bool first = true;
    if (first) {
        LOG_INFO(LOG_CEF, "OnAcceleratedPaint: planes=%d modifier=0x%lx format=%d",
                 info.plane_count, info.modifier, info.format);
        if (info.plane_count > 0) {
            LOG_INFO(LOG_CEF, "  plane[0]: fd=%d stride=%u offset=%lu size=%lu",
                     info.planes[0].fd, info.planes[0].stride,
                     info.planes[0].offset, info.planes[0].size);
        }
        first = false;
    }

    // Import dmabuf for zero-copy rendering
    if (info.plane_count > 0) {
        int w = info.extra.coded_size.width;
        int h = info.extra.coded_size.height;
        if (w > 0 && h > 0) {
            int fd = dup(info.planes[0].fd);
            if (fd >= 0) {
                if (type == PET_VIEW && on_accel_paint_) {
                    on_accel_paint_(fd, info.planes[0].stride, info.modifier, w, h);
                } else if (type == PET_POPUP && on_accel_popup_paint_) {
                    on_accel_popup_paint_(fd, info.planes[0].stride, info.modifier, w, h);
                } else {
                    close(fd);
                }
            }
        }
    }
#elif defined(_WIN32)
    // Windows: import D3D11 shared texture handle for zero-copy DComp rendering
    if (on_win_shared_paint_) {
        int w = info.extra.coded_size.width;
        int h = info.extra.coded_size.height;
        if (w > 0 && h > 0) {
            on_win_shared_paint_(info.shared_texture_handle, static_cast<int>(type), w, h);
        }
    }
#endif
}

void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    {
        std::lock_guard<std::mutex> lock(browser_mutex_);
        browser_ = browser;
    }
    LOG_INFO(LOG_CEF, "Browser created");
}

bool Client::OnBeforePopup(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           int popup_id,
                           const CefString& target_url,
                           const CefString& target_frame_name,
                           WindowOpenDisposition target_disposition,
                           bool user_gesture,
                           const CefPopupFeatures& popupFeatures,
                           CefWindowInfo& windowInfo,
                           CefRefPtr<CefClient>& client,
                           CefBrowserSettings& settings,
                           CefRefPtr<CefDictionaryValue>& extra_info,
                           bool* no_javascript_access) {
    // Intercept all popup creation and route supported links to the external browser.
    (void)browser;
    (void)frame;
    (void)popup_id;
    (void)target_frame_name;
    (void)target_disposition;
    (void)user_gesture;
    (void)popupFeatures;
    (void)windowInfo;
    (void)client;
    (void)settings;
    (void)extra_info;
    (void)no_javascript_access;

    handlePopupNavigation(target_url.ToString(), "Main");

    // Block popup creation in-app to avoid extra CEF windows for external links.
    return true;
}

void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    LOG_INFO(LOG_CEF, "Browser closing");
    {
        std::lock_guard<std::mutex> lock(browser_mutex_);
        browser_ = nullptr;
    }
    is_closed_ = true;
}

void Client::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) {
    if (frame->IsMain()) {
        // Set focus after page load for proper visual focus on autofocus elements
        browser->GetHost()->SetFocus(true);
    }
}

void Client::sendMouseMove(int x, int y, int modifiers) {
    auto b = browser();
    if (!b) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    b->GetHost()->SendMouseMoveEvent(event, false);
}

void Client::sendMouseClick(int x, int y, bool down, int button, int clickCount, int modifiers) {
    auto b = browser();
    if (!b) return;
    LOG_DEBUG(LOG_CEF, "Mouse button %d %s at %d,%d clicks=%d",
              button, down ? "DOWN" : "UP", x, y, clickCount);
    CefMouseEvent event;
    event.x = x;
    event.y = y;

    CefBrowserHost::MouseButtonType btn_type;
    switch (button) {
        case 1:
            btn_type = MBT_LEFT;
            if (down) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
            break;
        case 2:
            btn_type = MBT_MIDDLE;
            if (down) modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
            break;
        case 3:
            btn_type = MBT_RIGHT;
            if (down) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
            break;
        default:
            btn_type = MBT_LEFT;
            if (down) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
            break;
    }
    event.modifiers = modifiers;

    b->GetHost()->SendMouseClickEvent(event, btn_type, !down, clickCount);
}

void Client::sendKeyEvent(int key, bool down, int modifiers) {
    auto b = browser();
    if (!b) return;

    CefKeyEvent event;
    event.windows_key_code = sdlKeyToWindowsVK(key);
#ifdef __APPLE__
    event.native_key_code = sdlKeyToMacNative(key);
    // macOS: set character fields for all keys that have character codes
    // Control keys need their char codes set or CEF may double-fire
    if (key >= 0x20 && key < 0x7F) {
        event.character = key;
        event.unmodified_character = key;
    } else if (key == 0x08 || key == 0x09 || key == 0x0D || key == 0x1B || key == 0x7F) {
        // Backspace, Tab, Enter, Escape, Delete
        event.character = key;
        event.unmodified_character = key;
    } else {
        event.character = 0;
        event.unmodified_character = 0;
    }
    // macOS: use RAWKEYDOWN like cefclient (KEYEVENT_KEYDOWN is never used)
    event.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
#else
    event.native_key_code = key;
    event.type = down ? KEYEVENT_KEYDOWN : KEYEVENT_KEYUP;
#endif
    event.modifiers = modifiers;
    b->GetHost()->SendKeyEvent(event);

    // Send CHAR event for Enter key to trigger form submission
    if (down && key == 0x0D) {
        event.type = KEYEVENT_CHAR;
        event.character = '\r';
        event.unmodified_character = '\r';
        b->GetHost()->SendKeyEvent(event);
    }
}

void Client::sendChar(int charCode, int modifiers) {
    auto b = browser();
    if (!b) return;
    CefKeyEvent event;
    event.windows_key_code = charCode;
    event.character = charCode;
    event.unmodified_character = charCode;
    event.type = KEYEVENT_CHAR;
    event.modifiers = modifiers;
    b->GetHost()->SendKeyEvent(event);
}

void Client::sendMouseWheel(int x, int y, float deltaX, float deltaY, int modifiers) {
#ifdef __APPLE__
    // macOS: accumulate scroll deltas — flushed once per frame via flushScroll()
    // to coalesce multiple trackpad events into a single CEF wheel event.
    scroll_x_ = x;
    scroll_y_ = y;
    scroll_mods_ = modifiers;
    accum_scroll_x_ += deltaX * 10.0f;
    accum_scroll_y_ += deltaY * 10.0f;
    has_pending_scroll_ = true;
#else
    auto b = browser();
    if (!b) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    // SDL3 provides smooth scroll values, scale for CEF (expects ~120 per notch)
    int pixelX = static_cast<int>(deltaX * 53.0f);
    int pixelY = static_cast<int>(deltaY * 53.0f);
    b->GetHost()->SendMouseWheelEvent(event, pixelX, pixelY);
#endif
}

#ifdef __APPLE__
void Client::flushScroll() {
    if (!has_pending_scroll_) return;
    has_pending_scroll_ = false;

    int pixelX = static_cast<int>(accum_scroll_x_);
    int pixelY = static_cast<int>(accum_scroll_y_);
    accum_scroll_x_ -= pixelX;
    accum_scroll_y_ -= pixelY;
    if (pixelX == 0 && pixelY == 0) return;

    auto b = browser();
    if (!b) return;

    CefMouseEvent event;
    event.x = scroll_x_;
    event.y = scroll_y_;
    event.modifiers = scroll_mods_ | EVENTFLAG_PRECISION_SCROLLING_DELTA;
    b->GetHost()->SendMouseWheelEvent(event, pixelX, pixelY);
}
#endif

void Client::sendFocus(bool focused) {
    auto b = browser();
    if (!b) return;
    b->GetHost()->SetFocus(focused);
}

void Client::sendTouch(int id, float x, float y, float radiusX, float radiusY,
                       float pressure, int type, int modifiers) {
    auto b = browser();
    if (!b) return;
    CefTouchEvent event;
    event.id = id;
    event.x = x;
    event.y = y;
    event.radius_x = radiusX;
    event.radius_y = radiusY;
    event.rotation_angle = 0;
    event.pressure = pressure;
    event.type = static_cast<cef_touch_event_type_t>(type);
    event.modifiers = modifiers;
    event.pointer_type = CEF_POINTER_TYPE_TOUCH;
    b->GetHost()->SendTouchEvent(event);
}

void Client::paste(const char* mimeType, const void* data, size_t len) {
    doPaste(browser(), mimeType, data, len);
}

void Client::copy() {
    doCopy(browser(), false);
}

void Client::cut() {
    doCopy(browser(), true);
}

void Client::selectAll() {
    auto b = browser();
    if (b) b->GetMainFrame()->SelectAll();
}

void Client::undo() {
    auto b = browser();
    if (b) b->GetMainFrame()->Undo();
}

void Client::redo() {
    auto b = browser();
    if (b) b->GetMainFrame()->Redo();
}

void Client::goBack() {
    auto b = browser();
    if (b) b->GoBack();
}

void Client::goForward() {
    auto b = browser();
    if (b) b->GoForward();
}

void Client::resize(int width, int height, int physical_w, int physical_h) {
    LOG_DEBUG(LOG_CEF, "Client::resize: logical=%dx%d physical=%dx%d", width, height, physical_w, physical_h);
    width_ = width;
    height_ = height;
    physical_w_ = physical_w;
    physical_h_ = physical_h;
    auto b = browser();
    if (b) {
        // NotifyScreenInfoChanged first so CEF re-queries GetScreenInfo and
        // caches the correct device_scale_factor BEFORE WasResized triggers a
        // paint.  Without this, CEF uses a stale scale from the previous size,
        // producing a paint buffer that is a few pixels off from the actual
        // physical dimensions (visible as a gap at the window edges).
        b->GetHost()->NotifyScreenInfoChanged();
        b->GetHost()->WasResized();
        b->GetHost()->Invalidate(PET_VIEW);
    }
}

void Client::forceRepaint() {
    auto b = browser();
    if (b) {
        b->GetHost()->Invalidate(PET_VIEW);
    }
}

void Client::loadUrl(const std::string& url) {
    auto b = browser();
    if (b) {
        b->GetMainFrame()->LoadURL(url);
    }
}

void Client::executeJS(const std::string& code) {
    auto b = browser();
    if (!b) return;
    CefRefPtr<CefFrame> frame = b->GetMainFrame();
    if (frame) {
        frame->ExecuteJavaScript(code, frame->GetURL(), 0);
    }
}

void Client::exitFullscreen() {
    auto b = browser();
    if (!b) return;
    b->GetHost()->ExitFullscreen(true);
}

void Client::emitPlaying() {
    executeJS("if(window._nativeEmit) window._nativeEmit('playing');");
}

void Client::emitPaused() {
    executeJS("if(window._nativeEmit) window._nativeEmit('paused');");
}

void Client::emitFinished() {
    executeJS("if(window._nativeEmit) window._nativeEmit('finished');");
}

void Client::emitCanceled() {
    executeJS("if(window._nativeEmit) window._nativeEmit('canceled');");
}

void Client::emitError(const std::string& msg) {
    executeJS("if(window._nativeEmit) window._nativeEmit('error', '" + msg + "');");
}

void Client::emitRateChanged(double rate) {
    executeJS("if(window._nativeSetRate) window._nativeSetRate(" + std::to_string(rate) + ");");
}

void Client::updatePosition(double positionMs) {
    executeJS("if(window._nativeUpdatePosition) window._nativeUpdatePosition(" + std::to_string(positionMs) + ");");
}

void Client::updateDuration(double durationMs) {
    executeJS("if(window._nativeUpdateDuration) window._nativeUpdateDuration(" + std::to_string(durationMs) + ");");
}

bool Client::RunContextMenu(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefContextMenuParams> params,
                            CefRefPtr<CefMenuModel> model,
                            CefRefPtr<CefRunContextMenuCallback> callback) {
    LOG_DEBUG(LOG_CEF, "RunContextMenu called, items=%zu pos=%d,%d menu_=%s",
              model->GetCount(), params->GetXCoord(), params->GetYCoord(),
              menu_ ? "yes" : "no");

    if (!menu_ || model->GetCount() == 0) {
        LOG_DEBUG(LOG_CEF, "ContextMenu cancelled (no menu or no items)");
        callback->Cancel();
        return true;
    }

    // Build menu items
    std::vector<MenuItem> items;
    for (size_t i = 0; i < model->GetCount(); i++) {
        if (model->GetTypeAt(i) == MENUITEMTYPE_SEPARATOR) continue;
        std::string label = model->GetLabelAt(i).ToString();
        if (label.empty()) continue;
        items.push_back({
            model->GetCommandIdAt(i),
            label,
            model->IsEnabledAt(i)
        });
    }

    if (items.empty()) {
        callback->Cancel();
        return true;
    }

    LOG_DEBUG(LOG_CEF, "Opening context menu with %zu items", items.size());
    menu_->open(params->GetXCoord(), params->GetYCoord(), items, callback);
    return true;
}

// OverlayClient implementation
OverlayClient::OverlayClient(int width, int height, PaintCallback on_paint, LoadServerCallback on_load_server,
                             PhysicalSizeCallback physical_size_cb, AcceleratedPaintCallback on_accel_paint
#ifdef __APPLE__
                             , IOSurfacePaintCallback on_iosurface_paint
#endif
#ifdef _WIN32
                             , WinSharedTexturePaintCallback on_win_shared_paint
#endif
                             )
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_load_server_(std::move(on_load_server)),
      physical_size_cb_(std::move(physical_size_cb)),
      on_accel_paint_(std::move(on_accel_paint))
#ifdef __APPLE__
      , on_iosurface_paint_(std::move(on_iosurface_paint))
#endif
#ifdef _WIN32
      , on_win_shared_paint_(std::move(on_win_shared_paint))
#endif
      {}

bool OverlayClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                      cef_log_severity_t level,
                                      const CefString& message,
                                      const CefString& source,
                                      int line) {
    (void)browser; (void)level; (void)source; (void)line;
    LOG_DEBUG(LOG_JS_OVERLAY, "%s", message.ToString().c_str());
    return false;
}

bool OverlayClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                              CefRefPtr<CefFrame> frame,
                                              CefProcessId source_process,
                                              CefRefPtr<CefProcessMessage> message) {
    std::string name = message->GetName().ToString();
    CefRefPtr<CefListValue> args = message->GetArgumentList();

    LOG_DEBUG(LOG_CEF, "Overlay IPC received: %s", name.c_str());

    if (name == "loadServer" && on_load_server_) {
        std::string url = args->GetString(0).ToString();
        on_load_server_(url);
        return true;
    }

    if (name == "saveServerUrl") {
        std::string url = args->GetString(0).ToString();
        LOG_INFO(LOG_CEF, "Overlay IPC saving server URL: %s", url.c_str());
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
        return true;
    }

    if (name == "setSettingValue") {
        std::string section = args->GetString(0).ToString();
        std::string key = args->GetString(1).ToString();
        std::string value = args->GetString(2).ToString();
        LOG_INFO(LOG_CEF, "Overlay IPC setSettingValue: %s.%s = %s", section.c_str(), key.c_str(), value.c_str());
        applySettingValue(section, key, value);
        return true;
    }

    if (name == "checkServerConnectivity") {
        std::string url = args->GetString(0).ToString();
        LOG_INFO(LOG_CEF, "Overlay IPC checking connectivity: %s", url.c_str());

        // Normalize URL
        if (url.find("://") == std::string::npos) {
            url = "http://" + url;
        }
        // Remove trailing slash
        if (!url.empty() && url.back() == '/') {
            url.pop_back();
        }

        std::string check_url = url + "/System/Info/Public";

        CefRefPtr<CefRequest> request = CefRequest::Create();
        request->SetURL(check_url);
        request->SetMethod("GET");

        CefRefPtr<ConnectivityURLRequestClient> client =
            new ConnectivityURLRequestClient(browser, url);
        CefURLRequest::Create(request, client, nullptr);
        return true;
    }

    if (name == "setClipboard") {
        return handleSetClipboard(args);
    }

    if (name == "getClipboard") {
        handleGetClipboard(browser, args);
        return true;
    }

    LOG_WARN(LOG_CEF, "Overlay IPC unhandled: %s", name.c_str());
    return false;
}

void OverlayClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    // Return logical size - CEF multiplies by device_scale_factor for physical paint buffer
    static bool first = true;
    if (first) {
        LOG_INFO(LOG_CEF, "Overlay GetViewRect: %dx%d (logical)", width_, height_);
        first = false;
    }
    rect.Set(0, 0, width_, height_);
}

bool OverlayClient::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) {
    // Compute scale from physical/logical - CEF paints at logical * scale = physical
    // Prefer stored physical dimensions (set during resize) to avoid stale SDL queries
    int physical_w = physical_w_;
    int physical_h = physical_h_;
    if (physical_w <= 0 && physical_size_cb_) {
        physical_size_cb_(physical_w, physical_h);
    }
    float scale = (physical_w > 0 && width_ > 0) ? static_cast<float>(physical_w) / width_ : 1.0f;
    screen_info.device_scale_factor = scale;
    screen_info.depth = 32;
    screen_info.depth_per_component = 8;
    screen_info.is_monochrome = false;
    screen_info.rect = CefRect(0, 0, width_, height_);  // logical coordinates
    screen_info.available_rect = screen_info.rect;
    return true;
}

void OverlayClient::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                             const RectList& dirtyRects, const void* buffer,
                             int width, int height) {
    static bool first = true;
    if (first) {
        LOG_INFO(LOG_CEF, "Overlay OnPaint: %dx%d", width, height);
        first = false;
    }
    if (on_paint_ && type == PET_VIEW) {
        on_paint_(buffer, width, height);
    }
}

void OverlayClient::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                        const RectList& dirtyRects,
                                        const CefAcceleratedPaintInfo& info) {
#ifdef __APPLE__
    static bool first = true;
    if (first) {
        LOG_INFO(LOG_CEF, "Overlay OnAcceleratedPaint: iosurface=%p format=%d size=%dx%d",
                 info.shared_texture_io_surface, info.format,
                 info.extra.coded_size.width, info.extra.coded_size.height);
        first = false;
    }

    if (on_iosurface_paint_ && type == PET_VIEW && info.shared_texture_io_surface) {
        int w = info.extra.coded_size.width;
        int h = info.extra.coded_size.height;
        if (w > 0 && h > 0) {
            on_iosurface_paint_(info.shared_texture_io_surface, info.format, w, h);
        }
    }
#elif !defined(_WIN32)
    static bool first = true;
    if (first) {
        LOG_INFO(LOG_CEF, "Overlay OnAcceleratedPaint: planes=%d modifier=0x%lx format=%d",
                 info.plane_count, info.modifier, info.format);
        first = false;
    }

    // Import dmabuf for zero-copy rendering
    if (on_accel_paint_ && type == PET_VIEW && info.plane_count > 0) {
        // Use CEF's actual dmabuf dimensions, not window size
        int w = info.extra.coded_size.width;
        int h = info.extra.coded_size.height;
        if (w > 0 && h > 0) {
            int fd = dup(info.planes[0].fd);
            if (fd >= 0) {
                on_accel_paint_(fd, info.planes[0].stride, info.modifier, w, h);
            }
        }
    }
#elif defined(_WIN32)
    // Windows: import D3D11 shared texture handle for zero-copy DComp rendering
    if (on_win_shared_paint_) {
        int w = info.extra.coded_size.width;
        int h = info.extra.coded_size.height;
        if (w > 0 && h > 0) {
            on_win_shared_paint_(info.shared_texture_handle, static_cast<int>(type), w, h);
        }
    }
#endif
}

void OverlayClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    {
        std::lock_guard<std::mutex> lock(browser_mutex_);
        browser_ = browser;
    }
    LOG_INFO(LOG_CEF, "Overlay browser created");
}

bool OverlayClient::OnBeforePopup(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  int popup_id,
                                  const CefString& target_url,
                                  const CefString& target_frame_name,
                                  WindowOpenDisposition target_disposition,
                                  bool user_gesture,
                                  const CefPopupFeatures& popupFeatures,
                                  CefWindowInfo& windowInfo,
                                  CefRefPtr<CefClient>& client,
                                  CefBrowserSettings& settings,
                                  CefRefPtr<CefDictionaryValue>& extra_info,
                                  bool* no_javascript_access) {
    // Intercept overlay popups and route supported links to the external browser.
    (void)browser;
    (void)frame;
    (void)popup_id;
    (void)target_frame_name;
    (void)target_disposition;
    (void)user_gesture;
    (void)popupFeatures;
    (void)windowInfo;
    (void)client;
    (void)settings;
    (void)extra_info;
    (void)no_javascript_access;

    handlePopupNavigation(target_url.ToString(), "Overlay");

    return true;
}

void OverlayClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    LOG_INFO(LOG_CEF, "Overlay browser closing");
    {
        std::lock_guard<std::mutex> lock(browser_mutex_);
        browser_ = nullptr;
    }
    is_closed_ = true;
}

void OverlayClient::resize(int width, int height, int physical_w, int physical_h) {
    width_ = width;
    height_ = height;
    physical_w_ = physical_w;
    physical_h_ = physical_h;
    auto b = browser();
    if (b) {
        b->GetHost()->NotifyScreenInfoChanged();
        b->GetHost()->WasResized();
        b->GetHost()->Invalidate(PET_VIEW);
    }
}

void OverlayClient::sendFocus(bool focused) {
    auto b = browser();
    if (b) b->GetHost()->SetFocus(focused);
}

void OverlayClient::sendMouseMove(int x, int y, int modifiers) {
    auto b = browser();
    if (!b) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    b->GetHost()->SendMouseMoveEvent(event, false);
}

void OverlayClient::sendMouseClick(int x, int y, bool down, int button, int clickCount, int modifiers) {
    auto b = browser();
    if (!b) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;

    CefBrowserHost::MouseButtonType btn_type;
    switch (button) {
        case 1: btn_type = MBT_LEFT; if (down) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON; break;
        case 2: btn_type = MBT_MIDDLE; if (down) modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON; break;
        case 3: btn_type = MBT_RIGHT; if (down) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON; break;
        default: btn_type = MBT_LEFT; if (down) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON; break;
    }
    event.modifiers = modifiers;
    b->GetHost()->SendMouseClickEvent(event, btn_type, !down, clickCount);
}

void OverlayClient::sendMouseWheel(int x, int y, float deltaX, float deltaY, int modifiers) {
#ifdef __APPLE__
    scroll_x_ = x;
    scroll_y_ = y;
    scroll_mods_ = modifiers;
    accum_scroll_x_ += deltaX * 10.0f;
    accum_scroll_y_ += deltaY * 10.0f;
    has_pending_scroll_ = true;
#else
    auto b = browser();
    if (!b) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    int pixelX = static_cast<int>(deltaX * 53.0f);
    int pixelY = static_cast<int>(deltaY * 53.0f);
    b->GetHost()->SendMouseWheelEvent(event, pixelX, pixelY);
#endif
}

#ifdef __APPLE__
void OverlayClient::flushScroll() {
    if (!has_pending_scroll_) return;
    has_pending_scroll_ = false;

    int pixelX = static_cast<int>(accum_scroll_x_);
    int pixelY = static_cast<int>(accum_scroll_y_);
    accum_scroll_x_ -= pixelX;
    accum_scroll_y_ -= pixelY;
    if (pixelX == 0 && pixelY == 0) return;

    auto b = browser();
    if (!b) return;

    CefMouseEvent event;
    event.x = scroll_x_;
    event.y = scroll_y_;
    event.modifiers = scroll_mods_ | EVENTFLAG_PRECISION_SCROLLING_DELTA;
    b->GetHost()->SendMouseWheelEvent(event, pixelX, pixelY);
}
#endif

void OverlayClient::sendKeyEvent(int key, bool down, int modifiers) {
    auto b = browser();
    if (!b) return;
    CefKeyEvent event;
    event.windows_key_code = sdlKeyToWindowsVK(key);
#ifdef __APPLE__
    event.native_key_code = sdlKeyToMacNative(key);
#else
    event.native_key_code = key;
#endif
    event.modifiers = modifiers;
    event.type = down ? KEYEVENT_KEYDOWN : KEYEVENT_KEYUP;
    b->GetHost()->SendKeyEvent(event);

    if (down && key == 0x0D) {
        event.type = KEYEVENT_CHAR;
        event.character = '\r';
        event.unmodified_character = '\r';
        b->GetHost()->SendKeyEvent(event);
    }
}

void OverlayClient::sendChar(int charCode, int modifiers) {
    auto b = browser();
    if (!b) return;
    CefKeyEvent event;
    event.windows_key_code = charCode;
    event.character = charCode;
    event.unmodified_character = charCode;
    event.type = KEYEVENT_CHAR;
    event.modifiers = modifiers;
    b->GetHost()->SendKeyEvent(event);
}

void OverlayClient::sendTouch(int id, float x, float y, float radiusX, float radiusY,
                              float pressure, int type, int modifiers) {
    auto b = browser();
    if (!b) return;
    CefTouchEvent event;
    event.id = id;
    event.x = x;
    event.y = y;
    event.radius_x = radiusX;
    event.radius_y = radiusY;
    event.rotation_angle = 0;
    event.pressure = pressure;
    event.type = static_cast<cef_touch_event_type_t>(type);
    event.modifiers = modifiers;
    event.pointer_type = CEF_POINTER_TYPE_TOUCH;
    b->GetHost()->SendTouchEvent(event);
}

void OverlayClient::paste(const char* mimeType, const void* data, size_t len) {
    doPaste(browser(), mimeType, data, len);
}

void OverlayClient::copy() {
    doCopy(browser(), false);
}

void OverlayClient::cut() {
    doCopy(browser(), true);
}

void OverlayClient::selectAll() {
    auto b = browser();
    if (b) b->GetMainFrame()->SelectAll();
}

void OverlayClient::undo() {
    auto b = browser();
    if (b) b->GetMainFrame()->Undo();
}

void OverlayClient::redo() {
    auto b = browser();
    if (b) b->GetMainFrame()->Redo();
}
