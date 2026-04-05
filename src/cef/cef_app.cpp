#include "cef_app.h"
#include "resource_handler.h"
#include "../settings.h"
#include "embedded_js.h"
#include "../logging_linux.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_frame.h"
#include <cmath>

#ifdef __APPLE__
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#endif

void App::OnBeforeCommandLineProcessing(const CefString& process_type,
                                        CefRefPtr<CefCommandLine> command_line) {
    // Disable all Google services
    command_line->AppendSwitch("disable-background-networking");
    command_line->AppendSwitch("disable-client-side-phishing-detection");
    command_line->AppendSwitch("disable-default-apps");
    command_line->AppendSwitch("disable-extensions");
    command_line->AppendSwitch("disable-component-update");
    command_line->AppendSwitch("disable-sync");
    command_line->AppendSwitch("disable-translate");
    command_line->AppendSwitch("disable-domain-reliability");
    command_line->AppendSwitch("disable-breakpad");
    command_line->AppendSwitch("disable-notifications");
    command_line->AppendSwitch("disable-spell-checking");
    command_line->AppendSwitch("no-pings");
    command_line->AppendSwitch("bwsi");
    command_line->AppendSwitchWithValue("disable-features",
        "PushMessaging,BackgroundSync,SafeBrowsing,Translate,OptimizationHints,"
        "MediaRouter,DialMediaRouteProvider,AcceptCHFrame,AutofillServerCommunication,"
        "CertificateTransparencyComponentUpdater,SyncNotificationServiceWhenSignedIn,"
        "SpellCheck,SpellCheckService,PasswordManager");
    command_line->AppendSwitchWithValue("google-api-key", "");
    command_line->AppendSwitchWithValue("google-default-client-id", "");
    command_line->AppendSwitchWithValue("google-default-client-secret", "");

#ifdef __APPLE__
    command_line->AppendSwitch("single-process");
#else
    // Force X11 for CEF's internal rendering -- Wayland OSR has scaling issues
    command_line->AppendSwitchWithValue("ozone-platform", "x11");
#endif
}

void App::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
    registrar->AddCustomScheme("app",
        CEF_SCHEME_OPTION_STANDARD |
        CEF_SCHEME_OPTION_SECURE |
        CEF_SCHEME_OPTION_LOCAL |
        CEF_SCHEME_OPTION_CORS_ENABLED);
}

void App::OnContextInitialized() {
    LOG_INFO(LOG_CEF, "CEF context initialized");
    CefRegisterSchemeHandlerFactory("app", "", new EmbeddedSchemeHandlerFactory());
}

void App::OnContextCreated(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) {
    // Load settings (renderer process is separate from browser process)
    Settings::instance().load();

    CefRefPtr<CefV8Value> window = context->GetGlobal();
    CefRefPtr<NativeV8Handler> handler = new NativeV8Handler(browser);

    CefRefPtr<CefV8Value> jmpNative = CefV8Value::CreateObject(nullptr, nullptr);
    jmpNative->SetValue("playerLoad", CefV8Value::CreateFunction("playerLoad", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerStop", CefV8Value::CreateFunction("playerStop", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerPause", CefV8Value::CreateFunction("playerPause", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerPlay", CefV8Value::CreateFunction("playerPlay", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSeek", CefV8Value::CreateFunction("playerSeek", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSetVolume", CefV8Value::CreateFunction("playerSetVolume", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSetMuted", CefV8Value::CreateFunction("playerSetMuted", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSetSpeed", CefV8Value::CreateFunction("playerSetSpeed", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSetSubtitle", CefV8Value::CreateFunction("playerSetSubtitle", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSetAudio", CefV8Value::CreateFunction("playerSetAudio", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSetAudioDelay", CefV8Value::CreateFunction("playerSetAudioDelay", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("saveServerUrl", CefV8Value::CreateFunction("saveServerUrl", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("loadServer", CefV8Value::CreateFunction("loadServer", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("checkServerConnectivity", CefV8Value::CreateFunction("checkServerConnectivity", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyMetadata", CefV8Value::CreateFunction("notifyMetadata", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyPosition", CefV8Value::CreateFunction("notifyPosition", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifySeek", CefV8Value::CreateFunction("notifySeek", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyPlaybackState", CefV8Value::CreateFunction("notifyPlaybackState", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyArtwork", CefV8Value::CreateFunction("notifyArtwork", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyQueueChange", CefV8Value::CreateFunction("notifyQueueChange", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyRateChange", CefV8Value::CreateFunction("notifyRateChange", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("setClipboard", CefV8Value::CreateFunction("setClipboard", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("getClipboard", CefV8Value::CreateFunction("getClipboard", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("appExit", CefV8Value::CreateFunction("appExit", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("setSettingValue", CefV8Value::CreateFunction("setSettingValue", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("themeColor", CefV8Value::CreateFunction("themeColor", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("setOsdVisible", CefV8Value::CreateFunction("setOsdVisible", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("toggleFullscreen", CefV8Value::CreateFunction("toggleFullscreen", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("overlayFadeComplete", CefV8Value::CreateFunction("overlayFadeComplete", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("jmpNative", jmpNative, V8_PROPERTY_ATTRIBUTE_READONLY);

    // Inject JS shim
    std::string shim_str(embedded_js.at("native-shim.js"));

    std::string placeholder = "__SERVER_URL__";
    size_t pos = shim_str.find(placeholder);
    if (pos != std::string::npos)
        shim_str.replace(pos, placeholder.length(), Settings::instance().serverUrl());

    std::string settings_placeholder = "__SETTINGS_JSON__";
    pos = shim_str.find(settings_placeholder);
    if (pos != std::string::npos)
        shim_str.replace(pos, settings_placeholder.length(), Settings::instance().cliSettingsJson());

    frame->ExecuteJavaScript(shim_str, frame->GetURL(), 0);

    // Inject player plugins
    frame->ExecuteJavaScript(embedded_js.at("mpv-player-core.js"), frame->GetURL(), 0);
    frame->ExecuteJavaScript(embedded_js.at("mpv-video-player.js"), frame->GetURL(), 0);
    frame->ExecuteJavaScript(embedded_js.at("mpv-audio-player.js"), frame->GetURL(), 0);
    frame->ExecuteJavaScript(embedded_js.at("input-plugin.js"), frame->GetURL(), 0);
}

bool App::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefProcessId source_process,
                                   CefRefPtr<CefProcessMessage> message) {
    std::string name = message->GetName().ToString();

    if (name == "serverConnectivityResult") {
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        std::string url = args->GetString(0).ToString();
        bool success = args->GetBool(1);
        std::string resolved_url = args->GetString(2).ToString();
        std::string js = "if (window._onServerConnectivityResult) {"
                        "  window._onServerConnectivityResult('" + url + "', " +
                        (success ? "true" : "false") + ", '" + resolved_url + "');"
                        "}";
        frame->ExecuteJavaScript(js, frame->GetURL(), 0);
        return true;
    }

    if (name == "clipboardResult") {
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        std::string mimeType = args->GetString(0).ToString();
        std::string base64Data = args->GetString(1).ToString();
        std::string js = "if (window._onClipboardResult) {"
                        "  window._onClipboardResult('" + mimeType + "', '" + base64Data + "');"
                        "}";
        frame->ExecuteJavaScript(js, frame->GetURL(), 0);
        return true;
    }

    return false;
}

// V8 handler -- sends IPC messages to browser process
static int v8ToInt(const CefRefPtr<CefV8Value>& val, int fallback) {
    if (val->IsInt()) return val->GetIntValue();
    if (val->IsDouble()) return static_cast<int>(std::lround(val->GetDoubleValue()));
    return fallback;
}

bool NativeV8Handler::Execute(const CefString& name,
                              CefRefPtr<CefV8Value>,
                              const CefV8ValueList& arguments,
                              CefRefPtr<CefV8Value>&,
                              CefString&) {
    // Simple IPC relay: create message with same name and forward args
    CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(name);
    CefRefPtr<CefListValue> args = msg->GetArgumentList();

    if (name == "playerLoad") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            args->SetString(0, arguments[0]->GetStringValue());
            args->SetInt(1, arguments.size() > 1 ? v8ToInt(arguments[1], 0) : 0);
            args->SetInt(2, arguments.size() > 2 ? v8ToInt(arguments[2], -1) : -1);
            args->SetInt(3, arguments.size() > 3 ? v8ToInt(arguments[3], -1) : -1);
            args->SetString(4, arguments.size() > 4 && arguments[4]->IsString()
                ? arguments[4]->GetStringValue() : "{}");
        }
    } else if (name == "playerSeek" || name == "playerSetVolume" || name == "playerSetSpeed" ||
               name == "playerSetSubtitle" || name == "playerSetAudio" ||
               name == "notifyPosition" || name == "notifySeek") {
        if (arguments.size() >= 1) args->SetInt(0, v8ToInt(arguments[0], 0));
    } else if (name == "playerSetMuted") {
        if (arguments.size() >= 1 && arguments[0]->IsBool()) args->SetBool(0, arguments[0]->GetBoolValue());
    } else if (name == "playerSetAudioDelay" || name == "notifyRateChange") {
        if (arguments.size() >= 1 && arguments[0]->IsDouble()) args->SetDouble(0, arguments[0]->GetDoubleValue());
    } else if (name == "saveServerUrl" || name == "loadServer" || name == "checkServerConnectivity" ||
               name == "notifyMetadata" || name == "notifyPlaybackState" || name == "notifyArtwork" ||
               name == "themeColor") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) args->SetString(0, arguments[0]->GetStringValue());
    } else if (name == "notifyQueueChange") {
        if (arguments.size() >= 2 && arguments[0]->IsBool() && arguments[1]->IsBool()) {
            args->SetBool(0, arguments[0]->GetBoolValue());
            args->SetBool(1, arguments[1]->GetBoolValue());
        }
    } else if (name == "setSettingValue") {
        if (arguments.size() >= 3) {
            args->SetString(0, arguments[0]->GetStringValue());
            args->SetString(1, arguments[1]->GetStringValue());
            args->SetString(2, arguments[2]->GetStringValue());
        }
    } else if (name == "setClipboard") {
        if (arguments.size() >= 2) {
            args->SetString(0, arguments[0]->GetStringValue());
            args->SetString(1, arguments[1]->GetStringValue());
        }
    } else if (name == "getClipboard") {
        args->SetString(0, arguments.size() >= 1 && arguments[0]->IsString()
            ? arguments[0]->GetStringValue() : "text/plain");
    } else if (name == "setOsdVisible") {
        if (arguments.size() >= 1 && arguments[0]->IsBool()) args->SetBool(0, arguments[0]->GetBoolValue());
    }
    // playerStop, playerPause, playerPlay, appExit: no args needed

    browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
    return true;
}

#ifdef __APPLE__
// Wake pipe for external_message_pump (macOS only)
static int g_wake_pipe[2] = {-1, -1};
static std::atomic<bool> g_cef_work_pending{false};
static std::atomic<bool> g_cef_work_active{false};

void App::InitWakePipe() {
    pipe(g_wake_pipe);
    fcntl(g_wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_wake_pipe[1], F_SETFL, O_NONBLOCK);
}

int App::WakeFd() { return g_wake_pipe[0]; }

void App::OnScheduleMessagePumpWork(int64_t) {
    if (g_cef_work_active.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (g_cef_work_pending.compare_exchange_strong(expected, true, std::memory_order_release)) {
        char c = 1;
        (void)write(g_wake_pipe[1], &c, 1);
    }
}

void App::DoWork() {
    g_cef_work_active.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 5; i++) {
        g_cef_work_pending.store(false, std::memory_order_relaxed);
        CefDoMessageLoopWork();
        if (!g_cef_work_pending.load(std::memory_order_acquire)) break;
    }
    g_cef_work_active.store(false, std::memory_order_relaxed);
    char buf[64];
    while (read(g_wake_pipe[0], buf, sizeof(buf)) > 0) {}
}
#else
void App::OnScheduleMessagePumpWork(int64_t) {
    // multi_threaded_message_loop on Linux — no pump work needed
}
#endif
