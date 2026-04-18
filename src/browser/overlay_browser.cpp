#include "overlay_browser.h"
#include "web_browser.h"
#include "../common.h"
#include "../jellyfin_api.h"
#include "../settings.h"
#include "logging.h"
#include "../titlebar_color.h"
#include "../input/dispatch.h"
#include "include/cef_urlrequest.h"

#include <functional>
#include <utility>

constexpr float OVERLAY_FADE_DURATION_SEC = 0.25f;

// =====================================================================
// Server connectivity probe
// =====================================================================
//
// Two-phase probe: HEAD with redirect-follow to resolve the real server
// URL, then GET {base}/System/Info/Public to validate it's a Jellyfin
// server. Pure URL/JSON work lives in jellyfin_api; this class is just
// CEF HTTP glue.
//
// Cancellable: Cancel() aborts the active CefURLRequest and disables the
// completion callback so a late OnRequestComplete (e.g. UR_CANCELED) is a
// no-op.

class ServerProbeClient : public CefURLRequestClient {
public:
    using Callback = std::function<void(bool success, const std::string& base_url)>;

    ServerProbeClient(std::string normalized_url, Callback cb)
        : url_(std::move(normalized_url)), cb_(std::move(cb)) {}

    void Start() {
        current_request_ = MakeRequest("HEAD", url_);
    }

    void Cancel() {
        cb_ = nullptr;
        if (current_request_) {
            current_request_->Cancel();
            current_request_ = nullptr;
        }
    }

    void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
        if (!cb_) return;  // canceled

        if (phase_ == Phase::Head) {
            std::string resolved = url_;
            if (auto response = request->GetResponse()) {
                CefString final_url = response->GetURL();
                if (!final_url.empty()) resolved = final_url.ToString();
            }
            base_ = jellyfin_api::extract_base_url(resolved);
            phase_ = Phase::Get;
            current_request_ = MakeRequest("GET", base_ + "/System/Info/Public");
            return;
        }

        bool success = false;
        auto response = request->GetResponse();
        if (request->GetRequestStatus() == UR_SUCCESS
            && response && response->GetStatus() == 200
            && jellyfin_api::is_valid_public_info(body_)) {
            success = true;
        }
        auto cb = std::move(cb_);
        current_request_ = nullptr;
        cb(success, base_);
    }

    void OnDownloadData(CefRefPtr<CefURLRequest>, const void* data, size_t len) override {
        if (phase_ == Phase::Get) body_.append(static_cast<const char*>(data), len);
    }

    void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    bool GetAuthCredentials(bool, const CefString&, int, const CefString&,
                            const CefString&, CefRefPtr<CefAuthCallback>) override {
        return false;
    }

private:
    enum class Phase { Head, Get };

    CefRefPtr<CefURLRequest> MakeRequest(const char* method, const std::string& url) {
        auto req = CefRequest::Create();
        req->SetURL(url);
        req->SetMethod(method);
        return CefURLRequest::Create(req, this, nullptr);
    }

    std::string url_;
    Callback cb_;
    Phase phase_ = Phase::Head;
    std::string base_;
    std::string body_;
    CefRefPtr<CefURLRequest> current_request_;

    IMPLEMENT_REFCOUNTING(ServerProbeClient);
};

// =====================================================================
// Helpers
// =====================================================================

static void applySettingValue(const std::string& section, const std::string& key, const std::string& value) {
    auto& s = Settings::instance();
    if (key == "hwdec") s.setHwdec(value);
    else if (key == "audioPassthrough") s.setAudioPassthrough(value);
    else if (key == "audioExclusive") s.setAudioExclusive(value == "true");
    else if (key == "audioChannels") s.setAudioChannels(value);
    else if (key == "logLevel") s.setLogLevel(value);
    else LOG_WARN(LOG_CEF, "Unknown setting key: {}.{}", section.c_str(), key.c_str());
    s.saveAsync();
}

// =====================================================================
// OverlayBrowser
// =====================================================================

OverlayBrowser::~OverlayBrowser() = default;

OverlayBrowser::OverlayBrowser(RenderTarget target, WebBrowser& main_browser,
                               int w, int h, int pw, int ph)
    : client_(new CefLayer(target, w, h, pw, ph))
    , main_browser_(main_browser)
{
    client_->setMessageHandler([this](const std::string& name,
                                      CefRefPtr<CefListValue> args,
                                      CefRefPtr<CefBrowser> browser) {
        return handleMessage(name, args, browser);
    });
    client_->setCreatedCallback([](CefRefPtr<CefBrowser> browser) {
        // Overlay wins input whenever it's created.
        input::set_active_browser(browser);
    });
}

bool OverlayBrowser::handleMessage(const std::string& name,
                                   CefRefPtr<CefListValue> args,
                                   CefRefPtr<CefBrowser> browser) {
    if (name == "navigateMain") {
        // Start the main browser loading the given URL. Does NOT hide the
        // overlay — the JS side owns the pre-fade delay so the user can still
        // cancel during that window.
        std::string url = args->GetString(0).ToString();
        LOG_INFO(LOG_CEF, "Overlay: navigateMain {}", url.c_str());
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
        // loadUrl handles all cases: live browser, initial create pending,
        // or mid-reset — buffers the URL when the browser isn't ready.
        main_browser_.loadUrl(url);
    } else if (name == "dismissOverlay") {
        // Commit: hand input to main, start the fade, close when done.
        LOG_INFO(LOG_CEF, "Overlay: dismissOverlay");
        if (auto b = main_browser_.browser())
            input::set_active_browser(b);
        CefRefPtr<CefBrowser> overlay_browser = browser;
        g_platform.fade_overlay(OVERLAY_FADE_DURATION_SEC,
            []() {
                g_mpv.SetBackgroundColor(kVideoBgColor.hex);
                if (g_titlebar_color) g_titlebar_color->onOverlayDismissed();
            },
            [overlay_browser]() {
                if (overlay_browser)
                    overlay_browser->GetHost()->CloseBrowser(false);
            });
    } else if (name == "saveServerUrl") {
        std::string url = args->GetString(0).ToString();
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
    } else if (name == "setSettingValue") {
        std::string section = args->GetString(0).ToString();
        std::string key = args->GetString(1).ToString();
        std::string value = args->GetString(2).ToString();
        applySettingValue(section, key, value);
    } else if (name == "checkServerConnectivity") {
        std::string url = args->GetString(0).ToString();
        if (active_probe_) active_probe_->Cancel();
        active_probe_ = new ServerProbeClient(
            jellyfin_api::normalize_input(url),
            [browser, url](bool success, const std::string& base_url) {
                auto frame = browser ? browser->GetMainFrame() : nullptr;
                if (!frame) return;
                auto msg = CefProcessMessage::Create("serverConnectivityResult");
                msg->GetArgumentList()->SetString(0, url);
                msg->GetArgumentList()->SetBool(1, success);
                msg->GetArgumentList()->SetString(2, success ? base_url : url);
                frame->SendProcessMessage(PID_RENDERER, msg);
            });
        active_probe_->Start();
    } else if (name == "cancelServerConnectivity") {
        if (active_probe_) {
            active_probe_->Cancel();
            active_probe_ = nullptr;
        }
        // Kill the pre-load: closes the render process and recreates the main
        // browser blank, so no stale JS/service-workers/history survive.
        main_browser_.reset();
    } else {
        return false;
    }
    return true;
}
