#pragma once

#include "include/cef_browser.h"
#include "include/cef_values.h"

#include <functional>
#include <string>
#include <unordered_map>

// Single global JS↔C++ message bus.
//
// Wire shape:
//   JS → C++:  jmpNative.send(name, payload)
//              → "jmp.send" process message (cef_app.cpp NativeV8Handler)
//              → CefLayer::OnProcessMessageReceived
//              → MessageBus::dispatch(name, payload)
//              → registered Handler
//
//   C++ → JS:  MessageBus::emit(name, payload)
//              → "jmp.onMessage" process message to the namespace's browser
//              → cef_app.cpp App::OnProcessMessageReceived (render side)
//              → window.jmp.onMessage(name, payload)
//
// `emit` is callable from any thread — it self-hops to TID_UI when needed.
// `dispatch`, `on`, `registerNamespace`, and `unregisterNamespace` are not
// synchronized; call them on TID_UI (or pre-CefInitialize, before TID_UI
// exists). In practice handlers are registered during browser construction
// and dispatch fires from `OnProcessMessageReceived`, both already TID_UI.
class MessageBus {
public:
    using Handler = std::function<void(CefRefPtr<CefDictionaryValue> payload)>;

    // Register a handler for an exact message name (e.g. "player.load").
    void on(const std::string& name, Handler handler);

    // Map a namespace prefix (the part of a name before the first '.') to the
    // browser that owns it for outbound emissions.
    void registerNamespace(const std::string& ns, CefRefPtr<CefBrowser> browser);
    void unregisterNamespace(const std::string& ns);

    // Drop all namespace → browser mappings. Call before CefShutdown so the
    // bus doesn't keep CefRefPtr<CefBrowser> alive past the framework's
    // teardown.
    void clearNamespaces();

    // Inbound: invoke the handler registered for `name`. Returns true if one
    // was registered (whether or not it threw); false if no handler exists.
    bool dispatch(const std::string& name, CefRefPtr<CefDictionaryValue> payload);

    // Outbound: ship `name`/`payload` to the renderer that owns the namespace
    // prefix of `name`. Silently drops if the namespace has no registered
    // browser (e.g. emit during shutdown, or before browsers are up).
    void emit(const std::string& name, CefRefPtr<CefDictionaryValue> payload);

private:
    std::unordered_map<std::string, Handler> handlers_;
    std::unordered_map<std::string, CefRefPtr<CefBrowser>> namespaces_;
};

extern MessageBus g_bus;
