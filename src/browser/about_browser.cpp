#include "about_browser.h"
#include "app_menu.h"
#include "browsers.h"
#include "../common.h"
#include "logging.h"
#include "../platform/platform.h"
#include "include/cef_task.h"

#include <functional>

extern Platform g_platform;

namespace {

AboutBrowser* s_self = nullptr;

class FnTask : public CefTask {
public:
    explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override { if (fn_) fn_(); }
private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(FnTask);
};

}  // namespace

bool AboutBrowser::is_open() { return s_self != nullptr; }

CefRefPtr<CefDictionaryValue> AboutBrowser::injectionProfile() {
    static const char* const kFunctions[] = {
        "aboutOpenPath", "aboutDismiss",
    };
    CefRefPtr<CefListValue> fns = CefListValue::Create();
    for (size_t i = 0; i < sizeof(kFunctions) / sizeof(*kFunctions); i++)
        fns->SetString(i, kFunctions[i]);
    CefRefPtr<CefDictionaryValue> d = CefDictionaryValue::Create();
    d->SetList("functions", fns);
    d->SetList("scripts", CefListValue::Create());
    return d;
}

AboutBrowser::AboutBrowser()
    : layer_(g_browsers->create(injectionProfile()))
{
    layer_->setName("about");
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
        AboutBrowser* self = s_self;
        s_self = nullptr;
        if (!self) return;
        CefPostTask(TID_UI, CefRefPtr<CefTask>(new FnTask([self]() { delete self; })));
    });
}

AboutBrowser::~AboutBrowser() {
    release_layer(layer_.get());
}

void AboutBrowser::open() {
    if (s_self) {
        LOG_DEBUG(LOG_CEF, "AboutBrowser::open: already open, ignoring");
        return;
    }
    if (!g_browsers) {
        LOG_WARN(LOG_CEF, "AboutBrowser::open: no Browsers instance, ignoring");
        return;
    }
    LOG_INFO(LOG_CEF, "AboutBrowser::open");

    s_self = new AboutBrowser();
    s_self->layer_->setVisible(true);
    s_self->layer_->create("app://resources/about.html");
}

bool AboutBrowser::handleMessage(const std::string& name,
                                 CefRefPtr<CefListValue> args,
                                 CefRefPtr<CefBrowser> browser) {
    if (name == "aboutDismiss") {
        LOG_INFO(LOG_CEF, "AboutBrowser: aboutDismiss");
        if (g_browsers) g_browsers->setActive(prev_active_);
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
