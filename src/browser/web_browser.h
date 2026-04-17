#pragma once

#include "../cef/cef_client.h"
#include <string>

// Business logic wrapper for the main jellyfin-web browser.
// Owns a CefLayer (pure CEF) and handles all player, media session,
// settings, and fullscreen policy IPC.
class WebBrowser {
public:
    WebBrowser(RenderTarget target);

    // Forwarded from the CEF client
    CefRefPtr<CefBrowser> browser() { return client_->browser(); }
    void execJs(const std::string& js) { client_->execJs(js); }
    void resize(int w, int h, int pw, int ph) { client_->resize(w, h, pw, ph); }
    bool isClosed() const { return client_->isClosed(); }
    bool isLoaded() const { return client_->isLoaded(); }
    void waitForClose() { client_->waitForClose(); }
    void waitForLoad() { client_->waitForLoad(); }
    void create(const CefWindowInfo& wi, const CefBrowserSettings& bs, const std::string& url) {
        client_->create(wi, bs, url);
    }
    void reset() { client_->reset(); }
    void loadUrl(const std::string& url) { client_->loadUrl(url); }
    CefRefPtr<CefLayer> client() { return client_; }

private:
    bool handleMessage(const std::string& name,
                       CefRefPtr<CefListValue> args,
                       CefRefPtr<CefBrowser> browser);

    CefRefPtr<CefLayer> client_;
    bool was_fullscreen_before_osd_ = false;
};
