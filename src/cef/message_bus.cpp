#include "message_bus.h"

#include "logging.h"

#include "include/cef_task.h"

#include <functional>

MessageBus g_bus;

namespace {
std::string namespace_of(const std::string& name) {
    auto dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// Posts a lambda to TID_UI without pulling in base::BindOnce. Same pattern as
// the FnTask classes in cef_client.cpp / about_browser.cpp.
class FnTask : public CefTask {
public:
    explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override { if (fn_) fn_(); }
private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(FnTask);
};
}

void MessageBus::on(const std::string& name, Handler handler) {
    handlers_[name] = std::move(handler);
}

void MessageBus::registerNamespace(const std::string& ns, CefRefPtr<CefBrowser> browser) {
    namespaces_[ns] = browser;
}

void MessageBus::unregisterNamespace(const std::string& ns) {
    namespaces_.erase(ns);
}

bool MessageBus::dispatch(const std::string& name, CefRefPtr<CefDictionaryValue> payload) {
    auto it = handlers_.find(name);
    if (it == handlers_.end()) {
        LOG_WARN(LOG_CEF, "MessageBus: no handler for '{}'", name);
        return false;
    }
    if (!payload) payload = CefDictionaryValue::Create();
    it->second(payload);
    return true;
}

void MessageBus::emit(const std::string& name, CefRefPtr<CefDictionaryValue> payload) {
    // SendProcessMessage requires TID_UI under multi_threaded_message_loop.
    // mpv events feed us from cef_consumer_thread; MPRIS/SMTC backends fire
    // transport callbacks from their own poll threads. Self-hop so every
    // caller is correct without each one re-implementing the post.
    if (!CefCurrentlyOn(TID_UI)) {
        std::string captured_name = name;
        CefRefPtr<CefDictionaryValue> captured_payload =
            payload ? payload->Copy(false) : CefDictionaryValue::Create();
        CefPostTask(TID_UI, CefRefPtr<CefTask>(new FnTask(
            [captured_name, captured_payload]() {
                g_bus.emit(captured_name, captured_payload);
            })));
        return;
    }

    std::string ns = namespace_of(name);
    auto it = namespaces_.find(ns);
    if (it == namespaces_.end() || !it->second) return;
    CefRefPtr<CefBrowser> browser = it->second;
    auto frame = browser->GetMainFrame();
    if (!frame) return;
    auto msg = CefProcessMessage::Create("jmp.onMessage");
    auto args = msg->GetArgumentList();
    args->SetString(0, name);
    auto pv = CefValue::Create();
    pv->SetDictionary(payload ? payload : CefDictionaryValue::Create());
    args->SetValue(1, pv);
    frame->SendProcessMessage(PID_RENDERER, msg);
}

void MessageBus::clearNamespaces() {
    namespaces_.clear();
}
