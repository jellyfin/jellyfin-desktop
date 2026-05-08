#include "about_browser.h"
#include "app_menu.h"
#include "browsers.h"
#include "../common.h"
#include "../mpv/event.h"
#include "logging.h"
#include "../input/dispatch.h"
#include "../platform/platform.h"
#include "include/cef_task.h"

#include <cmath>
#include <functional>

extern Platform g_platform;

AboutBrowser* g_about_browser = nullptr;

namespace {
// Matches the FnTask pattern used in src/cef/cef_client.cpp — lets us post
// lambdas to the CEF UI thread without pulling in base::BindOnce headers.
class FnTask : public CefTask {
public:
    explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override { if (fn_) fn_(); }
private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(FnTask);
};

int safe_pw() { return mpv::osd_pw() > 0 ? mpv::osd_pw() : 1; }
int safe_ph() { return mpv::osd_ph() > 0 ? mpv::osd_ph() : 1; }

// Derive logical dimensions from physical pixels and the reported display
// scale. Falls back to 1:1 (scale=1) when the scale hasn't been observed yet.
int safe_lw() {
    double s = mpv::display_scale();
    if (s <= 0.0) s = 1.0;
    int pw = safe_pw();
    return static_cast<int>(std::lround(pw / s));
}
int safe_lh() {
    double s = mpv::display_scale();
    if (s <= 0.0) s = 1.0;
    int ph = safe_ph();
    return static_cast<int>(std::lround(ph / s));
}
}

CefRefPtr<CefDictionaryValue> AboutBrowser::injectionProfile() {
    static const char* const kFunctions[] = {
        "aboutOpenPath", "aboutDismiss",
        "menuItemSelected", "menuDismissed",
    };
    static const char* const kScripts[] = { "context-menu.js" };
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

AboutBrowser::AboutBrowser()
    : client_(new CefLayer(
        RenderTarget{g_platform.about_present, g_platform.about_present_software},
        safe_lw(), safe_lh(), safe_pw(), safe_ph()))
{
    prev_active_ = input::active_browser();

    client_->setMessageHandler([this](const std::string& name,
                                      CefRefPtr<CefListValue> args,
                                      CefRefPtr<CefBrowser> browser) {
        return handleMessage(name, args, browser);
    });
    client_->setCreatedCallback([](CefRefPtr<CefBrowser> browser) {
        input::set_active_browser(browser);
    });
    client_->setContextMenuBuilder(&app_menu::build);
    client_->setContextMenuDispatcher(&app_menu::dispatch);
    client_->setBeforeCloseCallback([]() {
        // Null the global now so "About" can be re-opened; defer the actual
        // delete so the CefLayer's OnBeforeClose lambda is not torn down
        // mid-invocation (we're running inside it right now).
        AboutBrowser* self = g_about_browser;
        g_about_browser = nullptr;
        if (!self) return;
        CefPostTask(TID_UI, CefRefPtr<CefTask>(new FnTask([self]() { delete self; })));
    });
}

void AboutBrowser::open() {
    if (g_about_browser) {
        LOG_DEBUG(LOG_CEF, "AboutBrowser::open: already open, ignoring");
        return;
    }
    LOG_INFO(LOG_CEF, "AboutBrowser::open");

    g_about_browser = new AboutBrowser();
    g_platform.set_about_visible(true);

    CefWindowInfo wi;
    wi.SetAsWindowless(0);
    wi.shared_texture_enabled = g_platform.shared_texture_supported;
#ifdef __APPLE__
    wi.external_begin_frame_enabled = true;
#else
    wi.external_begin_frame_enabled = false;
#endif
    CefBrowserSettings bs;
    bs.background_color = 0;
    bs.windowless_frame_rate = g_display_hz.load(std::memory_order_relaxed);

    CefBrowserHost::CreateBrowser(wi, g_about_browser->client_,
                                  "app://resources/about.html", bs,
                                  injectionProfile(), nullptr);
}

bool AboutBrowser::handleMessage(const std::string& name,
                                 CefRefPtr<CefListValue> args,
                                 CefRefPtr<CefBrowser> browser) {
    if (name == "aboutDismiss") {
        LOG_INFO(LOG_CEF, "AboutBrowser: aboutDismiss");
        input::set_active_browser(prev_active_);
        g_platform.set_about_visible(false);
        if (browser) browser->GetHost()->CloseBrowser(false);
        return true;
    }
    if (name == "aboutOpenPath") {
        std::string path = args->GetString(0).ToString();
        if (path.empty()) {
            LOG_WARN(LOG_CEF, "aboutOpenPath: empty path, ignoring");
            return true;
        }
        if (g_platform.open_external_url)
            g_platform.open_external_url("file://" + path);
        return true;
    }
    return false;
}
