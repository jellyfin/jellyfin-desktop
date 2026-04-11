#include "cef_app.h"
#include "resource_handler.h"
#include "../settings.h"
#include "embedded_js.h"
#include "../logging.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_frame.h"
#include "include/cef_v8.h"
#include <cmath>

#ifdef __APPLE__
#include <unistd.h>
#include <fcntl.h>
#include <CoreFoundation/CoreFoundation.h>
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

#ifdef __linux__
    // Force X11 for CEF's internal rendering -- Wayland OSR has scaling issues
    command_line->AppendSwitchWithValue("ozone-platform", "x11");
#endif

#ifdef __APPLE__
    // OSCrypt on macOS otherwise prompts for the login keychain on every
    // launch (unsigned/ad-hoc app has no stable keychain ACL). use-mock-keychain
    // bypasses the keychain entirely; password-store=basic also keeps the
    // password manager from reaching for the encryption key.
    command_line->AppendSwitch("use-mock-keychain");
    command_line->AppendSwitchWithValue("password-store", "basic");
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
    jmpNative->SetValue("appExit", CefV8Value::CreateFunction("appExit", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("setSettingValue", CefV8Value::CreateFunction("setSettingValue", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("themeColor", CefV8Value::CreateFunction("themeColor", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("setOsdVisible", CefV8Value::CreateFunction("setOsdVisible", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("setCursorVisible", CefV8Value::CreateFunction("setCursorVisible", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("toggleFullscreen", CefV8Value::CreateFunction("toggleFullscreen", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("menuItemSelected", CefV8Value::CreateFunction("menuItemSelected", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("menuDismissed", CefV8Value::CreateFunction("menuDismissed", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
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

    // Append player plugins to shim and execute all JS in one call
    shim_str += '\n';
    shim_str += embedded_js.at("mpv-player-core.js");
    shim_str += '\n';
    shim_str += embedded_js.at("mpv-video-player.js");
    shim_str += '\n';
    shim_str += embedded_js.at("mpv-audio-player.js");
    shim_str += '\n';
    shim_str += embedded_js.at("input-plugin.js");
    shim_str += '\n';
    shim_str += embedded_js.at("context-menu.js");
    frame->ExecuteJavaScript(shim_str, frame->GetURL(), 0);
}

static void callJsGlobal(CefRefPtr<CefFrame> frame, const char* fn_name,
                         const CefV8ValueList& v8args) {
    CefRefPtr<CefV8Context> ctx = frame->GetV8Context();
    if (!ctx || !ctx->Enter()) return;
    CefRefPtr<CefV8Value> fn = ctx->GetGlobal()->GetValue(fn_name);
    if (fn && fn->IsFunction()) fn->ExecuteFunction(nullptr, v8args);
    ctx->Exit();
}

bool App::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefProcessId source_process,
                                   CefRefPtr<CefProcessMessage> message) {
    std::string name = message->GetName().ToString();
    CefRefPtr<CefListValue> args = message->GetArgumentList();

    if (name == "serverConnectivityResult") {
        CefV8ValueList v8args;
        v8args.push_back(CefV8Value::CreateString(args->GetString(0)));
        v8args.push_back(CefV8Value::CreateBool(args->GetBool(1)));
        v8args.push_back(CefV8Value::CreateString(args->GetString(2)));
        callJsGlobal(frame, "_onServerConnectivityResult", v8args);
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
    } else if (name == "setOsdVisible") {
        if (arguments.size() >= 1 && arguments[0]->IsBool()) args->SetBool(0, arguments[0]->GetBoolValue());
    } else if (name == "setCursorVisible") {
        if (arguments.size() >= 1 && arguments[0]->IsBool()) args->SetBool(0, arguments[0]->GetBoolValue());
    } else if (name == "menuItemSelected") {
        if (arguments.size() >= 1) args->SetInt(0, v8ToInt(arguments[0], 0));
    }
    // playerStop, playerPause, playerPlay, appExit, menuDismissed: no args needed

    browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
    return true;
}

#ifdef __APPLE__
// Wake pipe for external_message_pump (macOS only).
// Immediate work (delay_ms == 0) writes to the pipe, waking CFRunLoop.
// Delayed work (delay_ms > 0) uses a CFRunLoopTimer.
static int g_wake_pipe[2] = {-1, -1};
static CFRunLoopTimerRef g_delayed_timer = nullptr;

static void cancel_delayed_timer() {
    if (g_delayed_timer) {
        CFRunLoopTimerInvalidate(g_delayed_timer);
        CFRelease(g_delayed_timer);
        g_delayed_timer = nullptr;
    }
}

static void delayed_timer_callback(CFRunLoopTimerRef, void*) {
    g_delayed_timer = nullptr;  // one-shot, already invalidated by firing
    App::DoWork();
}

void App::InitWakePipe() {
    pipe(g_wake_pipe);
    fcntl(g_wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_wake_pipe[1], F_SETFL, O_NONBLOCK);
}

int App::WakeFd() { return g_wake_pipe[0]; }

void App::OnScheduleMessagePumpWork(int64_t delay_ms) {
    if (delay_ms <= 0) {
        // Immediate work — wake the pipe so CFRunLoop fires
        char c = 1;
        (void)write(g_wake_pipe[1], &c, 1);
    } else {
        // Delayed work — schedule a one-shot CFRunLoopTimer
        cancel_delayed_timer();
        CFAbsoluteTime fire_time = CFAbsoluteTimeGetCurrent() + delay_ms / 1000.0;
        CFRunLoopTimerContext ctx = {0, nullptr, nullptr, nullptr, nullptr};
        g_delayed_timer = CFRunLoopTimerCreate(
            kCFAllocatorDefault, fire_time, 0, 0, 0,
            delayed_timer_callback, &ctx);
        CFRunLoopAddTimer(CFRunLoopGetMain(), g_delayed_timer, kCFRunLoopDefaultMode);
    }
}

void App::DoWork() {
    cancel_delayed_timer();
    // Drain wake pipe
    char buf[64];
    while (read(g_wake_pipe[0], buf, sizeof(buf)) > 0) {}
    CefDoMessageLoopWork();
}

void App::ScheduleWork() {
    char c = 1;
    (void)write(g_wake_pipe[1], &c, 1);
}
#else
void App::OnScheduleMessagePumpWork(int64_t) {
    // multi_threaded_message_loop on Linux — no pump work needed
}
#endif
