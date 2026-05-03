#pragma once

#include "../cef/cef_client.h"

// Business-logic wrapper for the About panel CEF browser.
//
// Lifecycle: create-on-open, destroy-on-close. Static AboutBrowser::open()
// is a no-op if a panel is already up; otherwise it allocates the singleton,
// creates the CEF browser at app://resources/about.html, and hands it input.
//
// On macOS, AboutBrowser::open() injects the panel into the currently active
// browser instead of creating a separate CEF browser.
//
// On dismiss (aboutDismiss IPC), the standalone path restores input to the
// previous browser, hides the platform subsurface, and closes the About
// browser. OnBeforeClose nulls g_about_browser and posts a deferred self-delete
// on the CEF UI thread so the instance is freed after the callback returns.
class AboutBrowser {
public:
    static void open();

    CefRefPtr<CefBrowser> browser() { return client_->browser(); }
    void resize(int w, int h, int pw, int ph) { client_->resize(w, h, pw, ph); }
    bool isClosed() const { return client_->isClosed(); }

    // Native-shim injection profile for this browser. See WebBrowser for
    // details. About only needs two jmpNative functions and no scripts —
    // about.html loads its own JS via <script>.
    static CefRefPtr<CefDictionaryValue> injectionProfile();

private:
    AboutBrowser();

    bool handleMessage(const std::string& name,
                       CefRefPtr<CefListValue> args,
                       CefRefPtr<CefBrowser> browser);

    CefRefPtr<CefLayer> client_;
    CefRefPtr<CefBrowser> prev_active_;
};
