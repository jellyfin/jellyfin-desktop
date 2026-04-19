#pragma once

#include "../cef/cef_client.h"

// Business-logic wrapper for the About panel CEF browser.
//
// Lifecycle: create-on-open, destroy-on-close. Static AboutBrowser::open()
// is a no-op if a panel is already up; otherwise it allocates the singleton,
// creates the CEF browser at app://resources/about.html, and hands it input.
//
// On dismiss (aboutDismiss IPC), the instance restores input to whatever
// browser had it before, hides the platform subsurface, and closes the CEF
// browser. OnBeforeClose nulls g_about_browser and posts a deferred
// self-delete on the CEF UI thread so the instance is freed after the
// callback returns rather than mid-invocation.
class AboutBrowser {
public:
    static void open();

    CefRefPtr<CefBrowser> browser() { return client_->browser(); }
    void resize(int w, int h, int pw, int ph) { client_->resize(w, h, pw, ph); }
    bool isClosed() const { return client_->isClosed(); }

private:
    AboutBrowser();

    bool handleMessage(const std::string& name,
                       CefRefPtr<CefListValue> args,
                       CefRefPtr<CefBrowser> browser);

    CefRefPtr<CefLayer> client_;
    CefRefPtr<CefBrowser> prev_active_;
};
