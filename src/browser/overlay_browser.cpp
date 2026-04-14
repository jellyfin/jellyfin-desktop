#include "overlay_browser.h"
#include "web_browser.h"
#include "../common.h"
#include "../settings.h"
#include "../logging.h"
#include "../titlebar_color.h"
#include "../input/dispatch.h"
#include "include/cef_urlrequest.h"

constexpr float OVERLAY_FADE_DELAY_SEC    = 1.0f;
constexpr float OVERLAY_FADE_DURATION_SEC = 0.25f;

// =====================================================================
// Connectivity check
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
// Helpers
// =====================================================================

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
// OverlayBrowser
// =====================================================================

OverlayBrowser::OverlayBrowser(RenderTarget target, WebBrowser& main_browser)
    : client_(new CefLayer(target))
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
    if (name == "loadServer") {
        std::string url = args->GetString(0).ToString();
        LOG_INFO(LOG_CEF, "Overlay: loadServer %s", url.c_str());
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
        // Navigate main browser to the server
        if (main_browser_.browser())
            main_browser_.browser()->GetMainFrame()->LoadURL(url);
        // Hand input back to the main browser
        input::set_active_browser(main_browser_.browser());
        // Close after fade
        CefRefPtr<CefBrowser> overlay_browser = browser;
        g_platform.fade_overlay(OVERLAY_FADE_DELAY_SEC, OVERLAY_FADE_DURATION_SEC,
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
        if (url.find("://") == std::string::npos) url = "http://" + url;
        if (!url.empty() && url.back() == '/') url.pop_back();
        std::string check_url = url + "/System/Info/Public";
        CefRefPtr<CefRequest> request = CefRequest::Create();
        request->SetURL(check_url);
        request->SetMethod("GET");
        CefURLRequest::Create(request, new ConnectivityURLRequestClient(browser, url), nullptr);
    } else {
        return false;
    }
    return true;
}
