// Minimal CEF subprocess helper for macOS multi-process mode.
// Contains the render-process CefApp (V8 bridge, JS injection) without
// any SDL/mpv/Vulkan/Metal dependencies.

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_library_loader.h"
#include "settings.h"
#include "embedded_js.h"
#include <mach-o/dyld.h>
#include <filesystem>
#include <cmath>
#include <cstring>

// --- V8 handler (sends IPC to browser process) ---

static int v8ToInt(const CefRefPtr<CefV8Value>& val, int fallback) {
    if (val->IsInt()) return val->GetIntValue();
    if (val->IsDouble()) return static_cast<int>(std::lround(val->GetDoubleValue()));
    return fallback;
}

class HelperV8Handler : public CefV8Handler {
public:
    HelperV8Handler(CefRefPtr<CefBrowser> browser) : browser_(browser) {}

    bool Execute(const CefString& name,
                 CefRefPtr<CefV8Value> object,
                 const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& retval,
                 CefString& exception) override;

private:
    CefRefPtr<CefBrowser> browser_;
    IMPLEMENT_REFCOUNTING(HelperV8Handler);
};

bool HelperV8Handler::Execute(const CefString& name,
                              CefRefPtr<CefV8Value> /*object*/,
                              const CefV8ValueList& arguments,
                              CefRefPtr<CefV8Value>& /*retval*/,
                              CefString& /*exception*/) {
    // Simple dispatch: create IPC message with the function name and forward args
    auto sendMsg = [&](const std::string& msgName) {
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(msgName);
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
    };

    auto sendMsgStr = [&](const std::string& msgName, int idx) {
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(msgName);
        if (idx < (int)arguments.size() && arguments[idx]->IsString())
            msg->GetArgumentList()->SetString(0, arguments[idx]->GetStringValue());
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
    };

    auto sendMsgInt = [&](const std::string& msgName, int idx, int fallback) {
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(msgName);
        msg->GetArgumentList()->SetInt(0, idx < (int)arguments.size() ? v8ToInt(arguments[idx], fallback) : fallback);
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
    };

    std::string n = name.ToString();

    if (n == "playerLoad") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerLoad");
            auto args = msg->GetArgumentList();
            args->SetString(0, arguments[0]->GetStringValue());
            args->SetInt(1, arguments.size() > 1 ? v8ToInt(arguments[1], 0) : 0);
            args->SetInt(2, arguments.size() > 2 ? v8ToInt(arguments[2], -1) : -1);
            args->SetInt(3, arguments.size() > 3 ? v8ToInt(arguments[3], -1) : -1);
            args->SetString(4, arguments.size() > 4 && arguments[4]->IsString()
                ? arguments[4]->GetStringValue() : CefString("{}"));
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (n == "playerStop") { sendMsg("playerStop"); return true; }
    if (n == "playerPause") { sendMsg("playerPause"); return true; }
    if (n == "playerPlay") { sendMsg("playerPlay"); return true; }
    if (n == "playerSeek") { sendMsgInt("playerSeek", 0, 0); return true; }
    if (n == "playerSetVolume") { sendMsgInt("playerSetVolume", 0, 100); return true; }

    if (n == "playerSetMuted") {
        if (arguments.size() >= 1 && arguments[0]->IsBool()) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetMuted");
            msg->GetArgumentList()->SetBool(0, arguments[0]->GetBoolValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (n == "playerSetSpeed") { sendMsgInt("playerSetSpeed", 0, 1000); return true; }
    if (n == "playerSetSubtitle") { sendMsgInt("playerSetSubtitle", 0, -1); return true; }
    if (n == "playerSetAudio") { sendMsgInt("playerSetAudio", 0, -1); return true; }

    if (n == "playerSetAudioDelay") {
        if (arguments.size() >= 1 && arguments[0]->IsDouble()) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetAudioDelay");
            msg->GetArgumentList()->SetDouble(0, arguments[0]->GetDoubleValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (n == "notifyMetadata") { sendMsgStr("notifyMetadata", 0); return true; }
    if (n == "notifyPlaybackState") { sendMsgStr("notifyPlaybackState", 0); return true; }
    if (n == "notifyArtwork") { sendMsgStr("notifyArtwork", 0); return true; }
    if (n == "saveServerUrl") { sendMsgStr("saveServerUrl", 0); return true; }
    if (n == "loadServer") { sendMsgStr("loadServer", 0); return true; }
    if (n == "checkServerConnectivity") { sendMsgStr("checkServerConnectivity", 0); return true; }
    if (n == "themeColor") { sendMsgStr("themeColor", 0); return true; }

    if (n == "notifyPosition" || n == "notifySeek") {
        sendMsgInt(n.c_str(), 0, 0);
        return true;
    }

    if (n == "notifyRateChange") {
        if (arguments.size() >= 1 && arguments[0]->IsDouble()) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyRateChange");
            msg->GetArgumentList()->SetDouble(0, arguments[0]->GetDoubleValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (n == "notifyQueueChange") {
        if (arguments.size() >= 2 && arguments[0]->IsBool() && arguments[1]->IsBool()) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyQueueChange");
            msg->GetArgumentList()->SetBool(0, arguments[0]->GetBoolValue());
            msg->GetArgumentList()->SetBool(1, arguments[1]->GetBoolValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (n == "setSettingValue") {
        if (arguments.size() >= 3 && arguments[0]->IsString() && arguments[1]->IsString() && arguments[2]->IsString()) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("setSettingValue");
            msg->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
            msg->GetArgumentList()->SetString(1, arguments[1]->GetStringValue());
            msg->GetArgumentList()->SetString(2, arguments[2]->GetStringValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (n == "setClipboard") {
        if (arguments.size() >= 2 && arguments[0]->IsString() && arguments[1]->IsString()) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("setClipboard");
            msg->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
            msg->GetArgumentList()->SetString(1, arguments[1]->GetStringValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (n == "getClipboard") {
        std::string mimeType = arguments.size() >= 1 && arguments[0]->IsString()
            ? arguments[0]->GetStringValue().ToString() : "text/plain";
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("getClipboard");
        msg->GetArgumentList()->SetString(0, mimeType);
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (n == "appExit") { sendMsg("appExit"); return true; }

    return false;
}

// --- Subprocess CefApp (render process only) ---

class SubprocessApp : public CefApp,
                      public CefRenderProcessHandler {
public:
    SubprocessApp() = default;

    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
        registrar->AddCustomScheme("app",
            CEF_SCHEME_OPTION_STANDARD |
            CEF_SCHEME_OPTION_SECURE |
            CEF_SCHEME_OPTION_LOCAL |
            CEF_SCHEME_OPTION_CORS_ENABLED);
    }

    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        Settings::instance().load();

        CefRefPtr<CefV8Value> window = context->GetGlobal();
        CefRefPtr<HelperV8Handler> handler = new HelperV8Handler(browser);

        CefRefPtr<CefV8Value> jmpNative = CefV8Value::CreateObject(nullptr, nullptr);
        const char* methods[] = {
            "playerLoad", "playerStop", "playerPause", "playerPlay", "playerSeek",
            "playerSetVolume", "playerSetMuted", "playerSetSpeed", "playerSetSubtitle",
            "playerSetAudio", "playerSetAudioDelay", "saveServerUrl", "loadServer",
            "checkServerConnectivity", "notifyMetadata", "notifyPosition", "notifySeek",
            "notifyPlaybackState", "notifyArtwork", "notifyQueueChange", "notifyRateChange",
            "setSettingValue", "setClipboard", "getClipboard", "appExit", "themeColor",
        };
        for (const char* m : methods) {
            jmpNative->SetValue(m, CefV8Value::CreateFunction(m, handler), V8_PROPERTY_ATTRIBUTE_READONLY);
        }
        window->SetValue("jmpNative", jmpNative, V8_PROPERTY_ATTRIBUTE_READONLY);

        // Inject JS shims
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
        frame->ExecuteJavaScript(embedded_js.at("mpv-player-core.js"), frame->GetURL(), 0);
        frame->ExecuteJavaScript(embedded_js.at("mpv-video-player.js"), frame->GetURL(), 0);
        frame->ExecuteJavaScript(embedded_js.at("mpv-audio-player.js"), frame->GetURL(), 0);
        frame->ExecuteJavaScript(embedded_js.at("input-plugin.js"), frame->GetURL(), 0);
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override {
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

private:
    IMPLEMENT_REFCOUNTING(SubprocessApp);
    DISALLOW_COPY_AND_ASSIGN(SubprocessApp);
};

// --- Entry point ---

int main(int argc, char* argv[]) {
    // Resolve CEF framework path relative to this helper executable.
    // Helper is at: Contents/Frameworks/<H>.app/Contents/MacOS/<exe>
    // Framework is at: Contents/Frameworks/Chromium Embedded Framework.framework/
    char exe_buf[PATH_MAX];
    uint32_t exe_size = sizeof(exe_buf);
    std::filesystem::path framework_path;
    if (_NSGetExecutablePath(exe_buf, &exe_size) == 0) {
        auto exe_dir = std::filesystem::canonical(exe_buf).parent_path();
        // Go up: MacOS -> Contents -> Helper.app -> Frameworks (main bundle)
        framework_path = exe_dir.parent_path().parent_path().parent_path();
    }
    if (framework_path.empty()) {
        return 1;
    }

    auto framework_lib = (framework_path /
        "Chromium Embedded Framework.framework" /
        "Chromium Embedded Framework").string();

    if (!cef_load_library(framework_lib.c_str())) {
        return 1;
    }

    CefMainArgs main_args(argc, argv);
    CefRefPtr<SubprocessApp> app(new SubprocessApp());
    int exit_code = CefExecuteProcess(main_args, app, nullptr);

    cef_unload_library();
    return exit_code;
}
