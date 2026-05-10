#pragma once

#include "../cef/cef_client.h"

// Business-logic wrapper for the About panel CEF browser.
class AboutBrowser {
public:
    static void open();

    CefRefPtr<CefBrowser> browser() { return layer_->browser(); }
    bool isClosed() const { return layer_->isClosed(); }
    ~AboutBrowser();

    static CefRefPtr<CefDictionaryValue> injectionProfile();

private:
    AboutBrowser();

    bool handleMessage(const std::string& name,
                       CefRefPtr<CefListValue> args,
                       CefRefPtr<CefBrowser> browser);

    CefRefPtr<CefLayer> layer_;
    CefRefPtr<CefBrowser> prev_active_;
};
