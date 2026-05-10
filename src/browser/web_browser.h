#pragma once

#include "../cef/cef_client.h"
#include <string>

// Business logic wrapper for the main jellyfin-web browser.
// Wraps a CefLayer owned by Browsers; configures the player, media session,
// settings, and fullscreen policy IPC handlers.
class WebBrowser {
public:
    explicit WebBrowser(CefRefPtr<CefLayer> layer);
    ~WebBrowser();

    CefRefPtr<CefBrowser> browser() { return layer_->browser(); }
    CefRefPtr<CefLayer> layer() { return layer_; }
    void loadUrl(const std::string& url) { layer_->loadUrl(url); }
    void reset() { layer_->reset(); }
    void waitForLoad() { layer_->waitForLoad(); }
    void waitForClose() { layer_->waitForClose(); }
    bool isClosed() const { return layer_->isClosed(); }
    void execJs(const std::string& js) { layer_->execJs(js); }

    // Native-shim injection profile for this browser.
    static CefRefPtr<CefDictionaryValue> injectionProfile();

private:
    bool handleMessage(const std::string& name,
                       CefRefPtr<CefListValue> args,
                       CefRefPtr<CefBrowser> browser);

    CefRefPtr<CefLayer> layer_;
    bool was_fullscreen_before_osd_ = false;
};
