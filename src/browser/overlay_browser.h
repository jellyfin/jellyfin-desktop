#pragma once

#include "../cef/cef_client.h"
#include <string>

class WebBrowser;
class ServerProbeClient;

// Business logic wrapper for the server selection overlay browser.
// Owns an CefLayer (pure CEF) and handles server selection,
// connectivity checks, and overlay fade/dismiss.
class OverlayBrowser {
public:
    OverlayBrowser(RenderTarget target, WebBrowser& main_browser);
    ~OverlayBrowser();

    CefRefPtr<CefBrowser> browser() { return client_->browser(); }
    void execJs(const std::string& js) { client_->execJs(js); }
    void resize(int w, int h, int pw, int ph) { client_->resize(w, h, pw, ph); }
    bool isClosed() const { return client_->isClosed(); }
    bool isLoaded() const { return client_->isLoaded(); }
    void waitForClose() { client_->waitForClose(); }
    void waitForLoad() { client_->waitForLoad(); }
    CefRefPtr<CefLayer> client() { return client_; }

private:
    bool handleMessage(const std::string& name,
                       CefRefPtr<CefListValue> args,
                       CefRefPtr<CefBrowser> browser);

    CefRefPtr<CefLayer> client_;
    WebBrowser& main_browser_;
    CefRefPtr<ServerProbeClient> active_probe_;
};
