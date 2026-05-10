#include "about_browser.h"
#include "app_menu.h"
#include "browsers.h"
#include "../common.h"
#include "../mpv/event.h"
#include "logging.h"
#include "../platform/platform.h"
#include "include/cef_task.h"

#include <functional>

extern Platform g_platform;

AboutBrowser* g_about_browser = nullptr;

namespace {
class FnTask : public CefTask {
public:
    explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override { if (fn_) fn_(); }
private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(FnTask);
};
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
    : layer_(g_browsers->create(injectionProfile()))
{
    prev_active_ = g_browsers->active();

    layer_->setMessageHandler([this](const std::string& name,
                                     CefRefPtr<CefListValue> args,
                                     CefRefPtr<CefBrowser> browser) {
        return handleMessage(name, args, browser);
    });
    layer_->setCreatedCallback([](CefRefPtr<CefBrowser> browser) {
        if (g_browsers) g_browsers->setActive(browser);
    });
    layer_->setContextMenuBuilder(&app_menu::build);
    layer_->setContextMenuDispatcher(&app_menu::dispatch);
    layer_->setBeforeCloseCallback([]() {
        AboutBrowser* self = g_about_browser;
        g_about_browser = nullptr;
        if (!self) return;
        CefPostTask(TID_UI, CefRefPtr<CefTask>(new FnTask([self]() { delete self; })));
    });
}

AboutBrowser::~AboutBrowser() {
    if (g_browsers && layer_) g_browsers->remove(layer_.get());
}

void AboutBrowser::open() {
    if (g_about_browser) {
        LOG_DEBUG(LOG_CEF, "AboutBrowser::open: already open, ignoring");
        return;
    }
    if (!g_browsers) {
        LOG_WARN(LOG_CEF, "AboutBrowser::open: no Browsers instance, ignoring");
        return;
    }
    LOG_INFO(LOG_CEF, "AboutBrowser::open");

    g_about_browser = new AboutBrowser();
    g_about_browser->layer_->setVisible(true);
    g_about_browser->layer_->create("app://resources/about.html");
}

bool AboutBrowser::handleMessage(const std::string& name,
                                 CefRefPtr<CefListValue> args,
                                 CefRefPtr<CefBrowser> browser) {
    if (name == "aboutDismiss") {
        LOG_INFO(LOG_CEF, "AboutBrowser: aboutDismiss");
        if (g_browsers) g_browsers->setActive(prev_active_);
        layer_->setVisible(false);
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
