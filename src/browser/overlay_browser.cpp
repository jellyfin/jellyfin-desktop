#include "overlay_browser.h"
#include "app_menu.h"
#include "web_browser.h"
#include "../common.h"
#include "../cef/message_bus.h"
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

CefRefPtr<CefDictionaryValue> OverlayBrowser::injectionProfile() {
    static const char* const kFunctions[] = {
        "send",
        "menuItemSelected", "menuDismissed",
    };
    static const char* const kScripts[] = {
        "jmp-bus.js",
        "context-menu.js",
    };
    CefRefPtr<CefListValue> fns = CefListValue::Create();
    for (size_t i = 0; i < sizeof(kFunctions) / sizeof(*kFunctions); i++)
        fns->SetString(i, kFunctions[i]);
    CefRefPtr<CefListValue> scripts = CefListValue::Create();
    for (size_t i = 0; i < sizeof(kScripts) / sizeof(*kScripts); i++)
        scripts->SetString(i, kScripts[i]);
    CefRefPtr<CefDictionaryValue> d = CefDictionaryValue::Create();
    d->SetList("functions", fns);
    d->SetList("scripts", scripts);
    return d;
}

OverlayBrowser::OverlayBrowser(RenderTarget target, WebBrowser& main_browser,
                               int w, int h, int pw, int ph)
    : client_(new CefLayer(target, w, h, pw, ph))
    , main_browser_(main_browser)
{
    client_->setCreatedCallback([](CefRefPtr<CefBrowser> browser) {
        // Overlay wins input whenever it's created.
        input::set_active_browser(browser);
        g_bus.registerNamespace("overlay", browser);
    });
    client_->setContextMenuBuilder(&app_menu::build);
    client_->setContextMenuDispatcher(&app_menu::dispatch);

    installBusHandlers();
}

// =====================================================================
// MessageBus handler registration (overlay.*)
// =====================================================================
//
// Runs in parallel with the legacy handleMessage arms. Outbound replies use
// g_bus.emit (e.g. "overlay.savedServerUrl") instead of hand-built
// CefProcessMessages so the wire format is uniform.

void OverlayBrowser::installBusHandlers() {
    g_bus.on("overlay.getSavedServerUrl", [](CefRefPtr<CefDictionaryValue>) {
        auto reply = CefDictionaryValue::Create();
        reply->SetString("url", Settings::instance().serverUrl());
        g_bus.emit("overlay.savedServerUrl", reply);
    });
    g_bus.on("overlay.navigateMain", [this](CefRefPtr<CefDictionaryValue> p) {
        if (!p->HasKey("url") || p->GetType("url") != VTYPE_STRING) return;
        std::string url = p->GetString("url").ToString();
        LOG_INFO(LOG_CEF, "Overlay: navigateMain {}", url.c_str());
        Settings::instance().setServerUrl(url);
        Settings::instance().saveAsync();
        main_browser_.loadUrl(url);
    });
    g_bus.on("overlay.dismissOverlay", [this](CefRefPtr<CefDictionaryValue>) {
        LOG_INFO(LOG_CEF, "Overlay: dismissOverlay");
        if (auto b = main_browser_.browser())
            input::set_active_browser(b);
        CefRefPtr<CefBrowser> overlay_browser = client_->browser();
        g_platform.fade_overlay(OVERLAY_FADE_DURATION_SEC,
            []() {
                g_mpv.SetBackgroundColor(kVideoBgColor.hex);
                if (g_titlebar_color) g_titlebar_color->onOverlayDismissed();
            },
            [overlay_browser]() {
                if (overlay_browser)
                    overlay_browser->GetHost()->CloseBrowser(false);
            });
    });
    g_bus.on("overlay.saveServerUrl", [](CefRefPtr<CefDictionaryValue> p) {
        if (!p->HasKey("url") || p->GetType("url") != VTYPE_STRING) return;
        Settings::instance().setServerUrl(p->GetString("url").ToString());
        Settings::instance().saveAsync();
    });
    g_bus.on("overlay.setSettingValue", [](CefRefPtr<CefDictionaryValue> p) {
        std::string section = p->HasKey("section") ? p->GetString("section").ToString() : "";
        std::string key     = p->HasKey("key")     ? p->GetString("key").ToString()     : "";
        std::string value   = p->HasKey("value")   ? p->GetString("value").ToString()   : "";
        applySettingValue(section, key, value);
    });
    g_bus.on("overlay.checkServerConnectivity", [this](CefRefPtr<CefDictionaryValue> p) {
        if (!p->HasKey("url") || p->GetType("url") != VTYPE_STRING) return;
        std::string url = p->GetString("url").ToString();
        if (active_probe_) active_probe_->Cancel();
        active_probe_ = new ServerProbeClient(
            jellyfin_api::normalize_input(url),
            [url](bool success, const std::string& base_url) {
                auto reply = CefDictionaryValue::Create();
                reply->SetString("url", url);
                reply->SetBool("success", success);
                reply->SetString("baseUrl", success ? base_url : url);
                g_bus.emit("overlay.serverConnectivityResult", reply);
            });
        active_probe_->Start();
    });
    g_bus.on("overlay.cancelServerConnectivity", [this](CefRefPtr<CefDictionaryValue>) {
        if (active_probe_) {
            active_probe_->Cancel();
            active_probe_ = nullptr;
        }
        main_browser_.reset();
    });
}

