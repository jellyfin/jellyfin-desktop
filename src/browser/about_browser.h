#pragma once

#include "../cef/cef_client.h"

// Business-logic wrapper for the About panel CEF browser.
// Lifetime is managed internally — open() creates a singleton instance
// that self-deletes via BeforeCloseCallback; no external owner pointer.
class AboutBrowser {
public:
    static void open();
    static bool is_open();

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
