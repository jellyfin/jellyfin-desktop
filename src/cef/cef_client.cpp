#include "cef_client.h"
#include "../settings.h"
#include "../logging.h"
#include "../player/media_session.h"
#include "../player/media_session_thread.h"
#include "../cjson/cJSON.h"
#include "include/cef_parser.h"
#include "../titlebar_color.h"
#include "include/cef_urlrequest.h"
#include <cstdio>
#ifndef _WIN32
#include <unistd.h>
#endif

extern MediaType g_media_type;
extern void update_idle_inhibit();

// =====================================================================
// Settings helper (shared between Client and OverlayClient)
// =====================================================================

static MediaMetadata parseMetadataJson(const std::string& json) {
    MediaMetadata meta;
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return meta;

    cJSON* item;
    if ((item = cJSON_GetObjectItem(root, "Name")) && cJSON_IsString(item))
        meta.title = item->valuestring;
    // Episodes: SeriesName as artist; audio: first element of Artists array
    if ((item = cJSON_GetObjectItem(root, "SeriesName")) && cJSON_IsString(item))
        meta.artist = item->valuestring;
    if (meta.artist.empty()) {
        if ((item = cJSON_GetObjectItem(root, "Artists")) && cJSON_IsArray(item)) {
            cJSON* first = cJSON_GetArrayItem(item, 0);
            if (first && cJSON_IsString(first))
                meta.artist = first->valuestring;
        }
    }
    // Episodes: SeasonName as album; audio: Album
    if ((item = cJSON_GetObjectItem(root, "SeasonName")) && cJSON_IsString(item))
        meta.album = item->valuestring;
    if (meta.album.empty()) {
        if ((item = cJSON_GetObjectItem(root, "Album")) && cJSON_IsString(item))
            meta.album = item->valuestring;
    }
    if ((item = cJSON_GetObjectItem(root, "IndexNumber")) && cJSON_IsNumber(item))
        meta.track_number = item->valueint;
    // RunTimeTicks is in 100ns units → microseconds
    if ((item = cJSON_GetObjectItem(root, "RunTimeTicks")) && cJSON_IsNumber(item))
        meta.duration_us = static_cast<int64_t>(item->valuedouble) / 10;
    if ((item = cJSON_GetObjectItem(root, "Type")) && cJSON_IsString(item)) {
        std::string type = item->valuestring;
        if (type == "Audio") meta.media_type = MediaType::Audio;
        else if (type == "Movie" || type == "Episode" || type == "Video" || type == "MusicVideo")
            meta.media_type = MediaType::Video;
    }
    cJSON_Delete(root);
    return meta;
}

static void applySettingValue(const std::string& section, const std::string& key, const std::string& value) {
    auto& s = Settings::instance();
    if (key == "hwdec") s.setHwdec(value);
    else if (key == "audioPassthrough") s.setAudioPassthrough(value);
    else if (key == "audioExclusive") s.setAudioExclusive(value == "true");
    else if (key == "audioChannels") s.setAudioChannels(value);
    else if (key == "logLevel") s.setLogLevel(value);
    else LOG_WARN(LOG_CEF, "Unknown setting key: %s.%s", section.c_str(), key.c_str());
    s.saveAsync();
}

// =====================================================================
// Connectivity check for overlay browser
// =====================================================================

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
            if (response_body_.find("\"Id\"") != std::string::npos) {
                success = true;
                resolved_url = response->GetURL().ToString();
                size_t pos = resolved_url.find("/System/Info/Public");
                if (pos != std::string::npos)
                    resolved_url = resolved_url.substr(0, pos);
            }
        }

        auto frame = browser_ ? browser_->GetMainFrame() : nullptr;
        if (frame) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("serverConnectivityResult");
            msg->GetArgumentList()->SetString(0, original_url_);
            msg->GetArgumentList()->SetBool(1, success);
            msg->GetArgumentList()->SetString(2, resolved_url);
            frame->SendProcessMessage(PID_RENDERER, msg);
        }
    }

    void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    void OnDownloadData(CefRefPtr<CefURLRequest>, const void* data, size_t len) override {
        response_body_.append(static_cast<const char*>(data), len);
    }
    bool GetAuthCredentials(bool, const CefString&, int, const CefString&, const CefString&,
                            CefRefPtr<CefAuthCallback>) override { return false; }

private:
    CefRefPtr<CefBrowser> browser_;
    std::string original_url_;
    std::string response_body_;
    IMPLEMENT_REFCOUNTING(ConnectivityURLRequestClient);
};

// =====================================================================
// Client -- main browser
// =====================================================================

void Client::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) {
    rect.Set(0, 0, width_, height_);
}

bool Client::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) {
    float scale = (physical_w_ > 0 && width_ > 0)
        ? static_cast<float>(physical_w_) / width_
        : 1.0f;
    info.device_scale_factor = scale;
    info.rect = CefRect(0, 0, width_, height_);
    info.available_rect = info.rect;
    return true;
}

void Client::resize(int w, int h, int physical_w, int physical_h) {
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

void Client::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                     const void* buffer, int w, int h) {
    if (type != PET_VIEW) return;
    if (g_platform.present_software)
        g_platform.present_software(buffer, w, h);
}

void Client::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                                const RectList&, const CefAcceleratedPaintInfo& info) {
    if (type != PET_VIEW) return;
    g_platform.present(info);
}

void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    browser_ = browser;
    browser->GetHost()->NotifyScreenInfoChanged();
    browser->GetHost()->WasResized();
    browser->GetHost()->Invalidate(PET_VIEW);
    #ifdef _WIN32
    extern void platform_push_input(CefRefPtr<CefBrowser> b);
    platform_push_input(browser);
    #endif
}

void Client::OnBeforeClose(CefRefPtr<CefBrowser>) {
    browser_ = nullptr;
    closed_ = true;
    loaded_ = true;  // unblock waitForLoad if browser dies before loading
    close_cv_.notify_all();
    load_cv_.notify_all();
}

void Client::waitForClose() {
    std::unique_lock<std::mutex> lock(close_mtx_);
    close_cv_.wait(lock, [this] { return closed_.load(); });
}

void Client::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) {
    if (frame->IsMain()) {
        loaded_ = true;
        load_cv_.notify_all();
    }
}

void Client::waitForLoad() {
    std::unique_lock<std::mutex> lock(load_mtx_);
    load_cv_.wait(lock, [this] { return loaded_.load(); });
}

void Client::OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                         ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) {
    LOG_ERROR(LOG_CEF, "OnLoadError: %s error=%d %s",
              failedUrl.ToString().c_str(), errorCode, errorText.ToString().c_str());
}

void Client::OnFullscreenModeChange(CefRefPtr<CefBrowser>, bool fullscreen) {
    g_platform.set_fullscreen(fullscreen);
}

bool Client::OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                            cef_cursor_type_t type, const CefCursorInfo&) {
    g_platform.set_cursor(type);
    return true;
}

void Client::execJs(const std::string& js) {
    if (browser_ && browser_->GetMainFrame())
        browser_->GetMainFrame()->ExecuteJavaScript(js, "", 0);
}

bool Client::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                                      CefProcessId, CefRefPtr<CefProcessMessage> message) {
    if (!g_mpv.IsValid()) return false;
    auto name = message->GetName().ToString();
    auto args = message->GetArgumentList();
    // All mpv calls are async -- never block CEF's thread.
    if (name == "playerLoad") {
        std::string url = args->GetString(0).ToString();
        int startMs = args->GetSize() > 1 ? args->GetInt(1) : 0;
        int audioIdx = args->GetSize() > 2 ? args->GetInt(2) : 0;
        int subIdx = args->GetSize() > 3 ? args->GetInt(3) : 0;
        LOG_INFO(LOG_CEF, "playerLoad: audio=%d sub=%d start=%dms url=%s",
                 audioIdx, subIdx, startMs, url.c_str());
        MpvHandle::LoadOptions opts;
        opts.startSecs = startMs / 1000.0;
        opts.audioTrack = audioIdx;
        opts.subTrack = subIdx;
        g_mpv.LoadFile(url, opts);
    } else if (name == "playerStop") {
        g_mpv.Stop();
        // Exit fullscreen when player stops — return to windowed library view
        g_platform.set_fullscreen(false);
    } else if (name == "playerPause") {
        g_mpv.Pause();
    } else if (name == "playerPlay") {
        g_mpv.Play();
    } else if (name == "playerSeek") {
        double pos = args->GetInt(0) / 1000.0;
        g_mpv.SeekAbsolute(pos);
    } else if (name == "playerSetVolume") {
        g_mpv.SetVolume(args->GetInt(0));
    } else if (name == "playerSetMuted") {
        g_mpv.SetMuted(args->GetInt(0) != 0);
    } else if (name == "playerSetSpeed") {
        g_mpv.SetSpeed(args->GetInt(0) / 1000.0);
    } else if (name == "playerSetSubtitle") {
        LOG_INFO(LOG_CEF, "playerSetSubtitle: %d", args->GetInt(0));
        g_mpv.SetSubtitleTrack(args->GetInt(0));
    } else if (name == "playerSetAudio") {
        g_mpv.SetAudioTrack(args->GetInt(0));
    } else if (name == "playerSetAudioDelay") {
        g_mpv.SetAudioDelay(args->GetDouble(0));
    } else if (name == "toggleFullscreen") {
        g_platform.toggle_fullscreen();
    } else if (name == "saveServerUrl") {
        std::string url = args->GetString(0).ToString();
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
    } else if (name == "setSettingValue") {
        std::string section = args->GetString(0).ToString();
        std::string key = args->GetString(1).ToString();
        std::string value = args->GetString(2).ToString();
        applySettingValue(section, key, value);
    } else if (name == "themeColor") {
        std::string color = args->GetString(0).ToString();
        if (g_titlebar_color) g_titlebar_color->onThemeColor(color);
    } else if (name == "notifyMetadata") {
        std::string json = args->GetString(0).ToString();
        MediaMetadata meta = parseMetadataJson(json);
        g_media_type = meta.media_type;
        update_idle_inhibit();
        if (g_media_session)
            g_media_session->setMetadata(meta);
    } else if (name == "notifyArtwork") {
        std::string artworkUri = args->GetString(0).ToString();
        if (g_media_session) g_media_session->setArtwork(artworkUri);
    } else if (name == "notifyQueueChange") {
        bool canNext = args->GetBool(0);
        bool canPrev = args->GetBool(1);
        if (g_media_session) {
            g_media_session->setCanGoNext(canNext);
            g_media_session->setCanGoPrevious(canPrev);
        }
    } else if (name == "notifyPlaybackState") {
        std::string state = args->GetString(0).ToString();
        if (g_media_session) {
            if (state == "Playing") g_media_session->setPlaybackState(PlaybackState::Playing);
            else if (state == "Paused") g_media_session->setPlaybackState(PlaybackState::Paused);
            else g_media_session->setPlaybackState(PlaybackState::Stopped);
        }
    } else if (name == "notifySeek") {
        int posMs = args->GetInt(0);
        if (g_media_session)
            g_media_session->emitSeeked(static_cast<int64_t>(posMs) * 1000);
    } else if (name == "setCursorVisible") {
        g_platform.set_cursor(args->GetBool(0) ? CT_POINTER : CT_NONE);
    } else if (name == "appExit") {
        initiate_shutdown();
    } else {
        return false;
    }
    return true;
}

// =====================================================================
// OverlayClient -- server selection/loading browser
// =====================================================================

void OverlayClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) {
    rect.Set(0, 0, width_, height_);
}

bool OverlayClient::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) {
    float scale = (physical_w_ > 0 && width_ > 0)
        ? static_cast<float>(physical_w_) / width_
        : 1.0f;
    info.device_scale_factor = scale;
    info.rect = CefRect(0, 0, width_, height_);
    info.available_rect = info.rect;
    return true;
}

void OverlayClient::resize(int w, int h, int physical_w, int physical_h) {
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

void OverlayClient::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                            const void* buffer, int w, int h) {
    if (type != PET_VIEW) return;
    if (g_platform.overlay_present_software)
        g_platform.overlay_present_software(buffer, w, h);
}

void OverlayClient::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                                       const RectList&, const CefAcceleratedPaintInfo& info) {
    if (type != PET_VIEW) return;
    g_platform.overlay_present(info);
}

void OverlayClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    browser_ = browser;
    browser->GetHost()->NotifyScreenInfoChanged();
    browser->GetHost()->WasResized();
    browser->GetHost()->Invalidate(PET_VIEW);
    #ifdef _WIN32
    extern void platform_push_input(CefRefPtr<CefBrowser> b);
    platform_push_input(browser);
    #endif
}

void OverlayClient::OnBeforeClose(CefRefPtr<CefBrowser>) {
    browser_ = nullptr;
    closed_ = true;
    loaded_ = true;
    close_cv_.notify_all();
    load_cv_.notify_all();
}

void OverlayClient::waitForClose() {
    std::unique_lock<std::mutex> lock(close_mtx_);
    close_cv_.wait(lock, [this] { return closed_.load(); });
}

void OverlayClient::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) {
    if (frame->IsMain()) {
        loaded_ = true;
        load_cv_.notify_all();
    }
}

void OverlayClient::waitForLoad() {
    std::unique_lock<std::mutex> lock(load_mtx_);
    load_cv_.wait(lock, [this] { return loaded_.load(); });
}

void OverlayClient::execJs(const std::string& js) {
    if (browser_ && browser_->GetMainFrame())
        browser_->GetMainFrame()->ExecuteJavaScript(js, "", 0);
}

bool OverlayClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                                             CefProcessId, CefRefPtr<CefProcessMessage> message) {
    auto name = message->GetName().ToString();
    auto args = message->GetArgumentList();

    if (name == "loadServer") {
        std::string url = args->GetString(0).ToString();
        LOG_INFO(LOG_CEF, "Overlay: loadServer %s", url.c_str());
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
        // Navigate main browser to the server
        if (g_client && g_client->browser())
            g_client->browser()->GetMainFrame()->LoadURL(url);
        // Fade entire overlay subsurface from opaque to transparent via
        // wp_alpha_modifier, then hide and close. Same visual effect as the
        // old OpenGL compositor alpha blending.
        g_platform.fade_overlay(0.5f);
        if (g_titlebar_color) g_titlebar_color->onOverlayDismissed();
        if (browser)
            browser->GetHost()->CloseBrowser(false);
        return true;
    } else if (name == "saveServerUrl") {
        std::string url = args->GetString(0).ToString();
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
        return true;
    } else if (name == "setSettingValue") {
        std::string section = args->GetString(0).ToString();
        std::string key = args->GetString(1).ToString();
        std::string value = args->GetString(2).ToString();
        applySettingValue(section, key, value);
        return true;
    } else if (name == "checkServerConnectivity") {
        std::string url = args->GetString(0).ToString();
        if (url.find("://") == std::string::npos) url = "http://" + url;
        if (!url.empty() && url.back() == '/') url.pop_back();
        std::string check_url = url + "/System/Info/Public";
        CefRefPtr<CefRequest> request = CefRequest::Create();
        request->SetURL(check_url);
        request->SetMethod("GET");
        CefURLRequest::Create(request, new ConnectivityURLRequestClient(browser, url), nullptr);
        return true;
    }

    return false;
}
