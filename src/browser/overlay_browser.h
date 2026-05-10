#pragma once

#include "../cef/cef_client.h"
#include <string>

class WebBrowser;
class ServerProbeClient;

// Business logic wrapper for the server selection overlay browser.
// Wraps a CefLayer owned by Browsers; configures server selection,
// connectivity checks, and overlay fade/dismiss.
class OverlayBrowser {
public:
    OverlayBrowser(CefRefPtr<CefLayer> layer, WebBrowser& main_browser);
    ~OverlayBrowser();

    CefRefPtr<CefBrowser> browser() { return layer_->browser(); }
    CefRefPtr<CefLayer> layer() { return layer_; }
    void waitForClose() { layer_->waitForClose(); }
    bool isClosed() const { return layer_->isClosed(); }

    static CefRefPtr<CefDictionaryValue> injectionProfile();

private:
    bool handleMessage(const std::string& name,
                       CefRefPtr<CefListValue> args,
                       CefRefPtr<CefBrowser> browser);

    CefRefPtr<CefLayer> layer_;
    WebBrowser& main_browser_;
    CefRefPtr<ServerProbeClient> active_probe_;
};
