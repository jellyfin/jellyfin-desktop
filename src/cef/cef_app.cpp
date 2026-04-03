#include "cef/cef_app.h"
#include "cef/resource_handler.h"
#include "settings.h"
#include "embedded_js.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_frame.h"
#include <cmath>
#include <cstring>
#include <regex>
#include "logging.h"

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
    command_line->AppendSwitch("bwsi");  // Browse without sign-in
    command_line->AppendSwitchWithValue("disable-features",
        "PushMessaging,BackgroundSync,SafeBrowsing,Translate,OptimizationHints,"
        "MediaRouter,DialMediaRouteProvider,AcceptCHFrame,AutofillServerCommunication,"
        "CertificateTransparencyComponentUpdater,SyncNotificationServiceWhenSignedIn,"
        "SpellCheck,SpellCheckService,PasswordManager");
    // Empty API keys prevent any Google API calls
    command_line->AppendSwitchWithValue("google-api-key", "");
    command_line->AppendSwitchWithValue("google-default-client-id", "");
    command_line->AppendSwitchWithValue("google-default-client-secret", "");

#ifdef __APPLE__
    // macOS: Use mock keychain to avoid system keychain prompts
    command_line->AppendSwitch("use-mock-keychain");
    // Single process mode - avoids Mach port rendezvous issues with ad-hoc signed app bundles
    // The rendezvous service registration fails for ad-hoc signed apps in /Applications
    command_line->AppendSwitch("single-process");
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
    // Force X11 mode on Linux - Wayland OSR has scaling issues
    command_line->AppendSwitchWithValue("ozone-platform", "x11");
#endif

    if (disable_gpu_compositing_) {
        command_line->AppendSwitch("disable-gpu-compositing");
    }
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

void App::OnScheduleMessagePumpWork(int64_t delay_ms) {
    // Called by CEF (from any thread) when it needs CefDoMessageLoopWork()
    if (delay_ms <= 0) {
        work_pending_.store(true, std::memory_order_relaxed);
        // Always wake — the eventfd is a kernel counter, extra writes coalesce
        // into one drain and are cheap. This avoids the fragile is_waiting_
        // protocol that could miss wakes during renderer subprocess startup.
        if (wake_callback_) wake_callback_();
    } else {
        // Delayed work — cancel any existing timer, then schedule a new one
        auto old = timer_id_.exchange(0, std::memory_order_relaxed);
        if (old != 0) SDL_RemoveTimer(old);
        timer_id_.store(
            SDL_AddTimer(static_cast<Uint32>(delay_ms), TimerCallback, nullptr),
            std::memory_order_relaxed);
    }
}

Uint32 App::TimerCallback(void* /*userdata*/, SDL_TimerID /*id*/, Uint32 /*interval*/) {
    timer_id_.store(0, std::memory_order_relaxed);
    if (wake_callback_) wake_callback_();
    return 0;  // Don't repeat
}

void App::DoWork() {
    if (is_active_.load(std::memory_order_relaxed)) {
        work_pending_.store(true, std::memory_order_relaxed);
        return;
    }

    is_active_.store(true, std::memory_order_relaxed);

    // Process current work, then drain follow-up immediate work that was
    // scheduled during CefDoMessageLoopWork.  Draining here prevents each
    // follow-up from pushing a wake event that spins the main loop.
    int iterations = 0;
    do {
        work_pending_.store(false, std::memory_order_relaxed);
        CefDoMessageLoopWork();
        iterations++;
    } while (work_pending_.load(std::memory_order_relaxed) && iterations < 5);

    is_active_.store(false, std::memory_order_relaxed);
    // Don't clear work_pending_ here — if CEF threads set it during the last
    // iteration, the main loop needs to see it to avoid sleeping.
}

std::string escapeString(const std::string& str) {
    std::regex pattern("\\\\|\\\"");
    return std::regex_replace(str, pattern, "\\$&");
}

void App::OnContextCreated(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) {
    LOG_DEBUG(LOG_CEF, "OnContextCreated: %s", frame->GetURL().ToString().c_str());

    // Load settings (renderer process is separate from browser process)
    Settings::instance().load();

    CefRefPtr<CefV8Value> window = context->GetGlobal();
    CefRefPtr<NativeV8Handler> handler = new NativeV8Handler(browser);

    // Create window.jmpNative for native calls
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
    window->SetValue("jmpNative", jmpNative, V8_PROPERTY_ATTRIBUTE_READONLY);

    // Inject the JavaScript shim that creates window.api, window.NativeShell, etc.
    std::string shim_str(embedded_js.at("native-shim.js"));

    // Replace placeholder with saved server URL
    std::string placeholder = "__SERVER_URL__";
    size_t pos = shim_str.find(placeholder);
    if (pos != std::string::npos) {
        shim_str.replace(pos, placeholder.length(), Settings::instance().serverUrl());
    }

    // Replace placeholder with saved settings JSON
    std::string settings_placeholder = "__SETTINGS_JSON__";
    pos = shim_str.find(settings_placeholder);
    if (pos != std::string::npos) {
        shim_str.replace(pos, settings_placeholder.length(), escapeString(Settings::instance().cliSettingsJson()));
    }

    frame->ExecuteJavaScript(shim_str, frame->GetURL(), 0);

    // Inject the player plugins
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

        // Call the JS callback
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
        // base64 is safe for JS strings (no escaping needed)
        std::string js = "if (window._onClipboardResult) {"
                        "  window._onClipboardResult('" + mimeType + "', '" + base64Data + "');"
                        "}";
        frame->ExecuteJavaScript(js, frame->GetURL(), 0);
        return true;
    }

    LOG_DEBUG(LOG_CEF, "App IPC Unhandled: %s", name.c_str());
    return false;
}

// V8 handler implementation - sends IPC messages to browser process
// Extract int from a V8 value, accepting both integer and floating-point numbers.
// JS numbers are doubles; IsInt() rejects non-integer doubles like 1799998.401,
// which silently dropped resume positions and seek targets.
static int v8ToInt(const CefRefPtr<CefV8Value>& val, int fallback) {
    if (val->IsInt()) return val->GetIntValue();
    if (val->IsDouble()) return static_cast<int>(std::lround(val->GetDoubleValue()));
    return fallback;
}

bool NativeV8Handler::Execute(const CefString& name,
                              CefRefPtr<CefV8Value> object,
                              const CefV8ValueList& arguments,
                              CefRefPtr<CefV8Value>& retval,
                              CefString& exception) {
    LOG_DEBUG(LOG_CEF, "V8 Execute: %s", name.ToString().c_str());

    // playerLoad(url, startMs, audioIdx, subIdx, metadataJson)
    if (name == "playerLoad") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string url = arguments[0]->GetStringValue().ToString();
            int startMs = arguments.size() > 1 ? v8ToInt(arguments[1], 0) : 0;
            int audioIdx = arguments.size() > 2 ? v8ToInt(arguments[2], -1) : -1;
            int subIdx = arguments.size() > 3 ? v8ToInt(arguments[3], -1) : -1;
            std::string metadataJson = arguments.size() > 4 && arguments[4]->IsString() ? arguments[4]->GetStringValue().ToString() : "{}";

            LOG_DEBUG(LOG_CEF, "V8 playerLoad: %s startMs=%d", url.c_str(), startMs);

            // Send IPC message to browser process
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerLoad");
            CefRefPtr<CefListValue> args = msg->GetArgumentList();
            args->SetString(0, url);
            args->SetInt(1, startMs);
            args->SetInt(2, audioIdx);
            args->SetInt(3, subIdx);
            args->SetString(4, metadataJson);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerStop") {
        LOG_DEBUG(LOG_CEF, "V8 playerStop");
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerStop");
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (name == "playerPause") {
        LOG_DEBUG(LOG_CEF, "V8 playerPause");
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerPause");
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (name == "playerPlay") {
        LOG_DEBUG(LOG_CEF, "V8 playerPlay (unpause)");
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerPlay");
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (name == "playerSeek") {
        if (arguments.size() >= 1) {
            int ms = v8ToInt(arguments[0], 0);
            LOG_DEBUG(LOG_CEF, "V8 playerSeek: %dms", ms);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSeek");
            msg->GetArgumentList()->SetInt(0, ms);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerSetVolume") {
        if (arguments.size() >= 1) {
            int vol = v8ToInt(arguments[0], 100);
            LOG_DEBUG(LOG_CEF, "V8 playerSetVolume: %d", vol);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetVolume");
            msg->GetArgumentList()->SetInt(0, vol);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerSetMuted") {
        if (arguments.size() >= 1 && arguments[0]->IsBool()) {
            bool muted = arguments[0]->GetBoolValue();
            LOG_DEBUG(LOG_CEF, "V8 playerSetMuted: %d", muted);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetMuted");
            msg->GetArgumentList()->SetBool(0, muted);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerSetSpeed") {
        if (arguments.size() >= 1) {
            int rateX1000 = v8ToInt(arguments[0], 1000);
            LOG_DEBUG(LOG_CEF, "V8 playerSetSpeed: %d", rateX1000);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetSpeed");
            msg->GetArgumentList()->SetInt(0, rateX1000);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerSetSubtitle") {
        if (arguments.size() >= 1) {
            int sid = v8ToInt(arguments[0], -1);
            LOG_DEBUG(LOG_CEF, "V8 playerSetSubtitle: %d", sid);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetSubtitle");
            msg->GetArgumentList()->SetInt(0, sid);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerSetAudio") {
        if (arguments.size() >= 1) {
            int aid = v8ToInt(arguments[0], -1);
            LOG_DEBUG(LOG_CEF, "V8 playerSetAudio: %d", aid);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetAudio");
            msg->GetArgumentList()->SetInt(0, aid);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerSetAudioDelay") {
        if (arguments.size() >= 1 && arguments[0]->IsDouble()) {
            double delay = arguments[0]->GetDoubleValue();
            LOG_DEBUG(LOG_CEF, "V8 playerSetAudioDelay: %.2f", delay);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetAudioDelay");
            msg->GetArgumentList()->SetDouble(0, delay);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyMetadata") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string metadata = arguments[0]->GetStringValue().ToString();
            LOG_DEBUG(LOG_CEF, "V8 notifyMetadata: %.100s...", metadata.c_str());
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyMetadata");
            msg->GetArgumentList()->SetString(0, metadata);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyPosition") {
        if (arguments.size() >= 1 && (arguments[0]->IsInt() || arguments[0]->IsDouble())) {
            int posMs = v8ToInt(arguments[0], 0);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyPosition");
            msg->GetArgumentList()->SetInt(0, posMs);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifySeek") {
        if (arguments.size() >= 1 && (arguments[0]->IsInt() || arguments[0]->IsDouble())) {
            int posMs = v8ToInt(arguments[0], 0);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifySeek");
            msg->GetArgumentList()->SetInt(0, posMs);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyPlaybackState") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string state = arguments[0]->GetStringValue().ToString();
            LOG_DEBUG(LOG_CEF, "V8 notifyPlaybackState: %s", state.c_str());
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyPlaybackState");
            msg->GetArgumentList()->SetString(0, state);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyArtwork") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string artworkUri = arguments[0]->GetStringValue().ToString();
            LOG_DEBUG(LOG_CEF, "V8 notifyArtwork: %.50s...", artworkUri.c_str());
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyArtwork");
            msg->GetArgumentList()->SetString(0, artworkUri);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyQueueChange") {
        if (arguments.size() >= 2 && arguments[0]->IsBool() && arguments[1]->IsBool()) {
            bool canNext = arguments[0]->GetBoolValue();
            bool canPrev = arguments[1]->GetBoolValue();
            LOG_DEBUG(LOG_CEF, "V8 notifyQueueChange: canNext=%d canPrev=%d", canNext, canPrev);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyQueueChange");
            msg->GetArgumentList()->SetBool(0, canNext);
            msg->GetArgumentList()->SetBool(1, canPrev);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyRateChange") {
        if (arguments.size() >= 1 && arguments[0]->IsDouble()) {
            double rate = arguments[0]->GetDoubleValue();
            LOG_DEBUG(LOG_CEF, "V8 notifyRateChange: %.2f", rate);
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyRateChange");
            msg->GetArgumentList()->SetDouble(0, rate);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "setSettingValue") {
        if (arguments.size() >= 3 && arguments[0]->IsString() && arguments[1]->IsString() && arguments[2]->IsString()) {
            std::string section = arguments[0]->GetStringValue().ToString();
            std::string key = arguments[1]->GetStringValue().ToString();
            std::string value = arguments[2]->GetStringValue().ToString();
            LOG_INFO(LOG_CEF, "V8 setSettingValue: %s.%s = %s", section.c_str(), key.c_str(), value.c_str());
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("setSettingValue");
            msg->GetArgumentList()->SetString(0, section);
            msg->GetArgumentList()->SetString(1, key);
            msg->GetArgumentList()->SetString(2, value);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "saveServerUrl") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string url = arguments[0]->GetStringValue().ToString();
            LOG_INFO(LOG_CEF, "V8 saveServerUrl: %s", url.c_str());
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("saveServerUrl");
            msg->GetArgumentList()->SetString(0, url);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "loadServer") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string url = arguments[0]->GetStringValue().ToString();
            LOG_INFO(LOG_CEF, "V8 loadServer: %s", url.c_str());
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("loadServer");
            msg->GetArgumentList()->SetString(0, url);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "checkServerConnectivity") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string url = arguments[0]->GetStringValue().ToString();
            LOG_DEBUG(LOG_CEF, "V8 checkServerConnectivity: %s", url.c_str());
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("checkServerConnectivity");
            msg->GetArgumentList()->SetString(0, url);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "setClipboard") {
        if (arguments.size() >= 2 && arguments[0]->IsString() && arguments[1]->IsString()) {
            std::string mimeType = arguments[0]->GetStringValue().ToString();
            std::string base64Data = arguments[1]->GetStringValue().ToString();
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("setClipboard");
            msg->GetArgumentList()->SetString(0, mimeType);
            msg->GetArgumentList()->SetString(1, base64Data);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "getClipboard") {
        std::string mimeType = arguments.size() >= 1 && arguments[0]->IsString()
            ? arguments[0]->GetStringValue().ToString() : "text/plain";
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("getClipboard");
        msg->GetArgumentList()->SetString(0, mimeType);
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (name == "appExit") {
        LOG_INFO(LOG_CEF, "V8 appExit");
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("appExit");
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (name == "themeColor") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string color = arguments[0]->GetStringValue().ToString();
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("themeColor");
            msg->GetArgumentList()->SetString(0, color);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "setOsdVisible") {
        if (arguments.size() >= 1 && arguments[0]->IsBool()) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("osdVisible");
            msg->GetArgumentList()->SetBool(0, arguments[0]->GetBoolValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    return false;
}
