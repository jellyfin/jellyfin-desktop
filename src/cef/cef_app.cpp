#include "cef_app.h"
#include "resource_handler.h"
#include "../settings.h"
#include "../paths/paths.h"
#include "embedded_js.h"
#include "logging.h"
#include "version.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_frame.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"

#include <cassert>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include "include/wrapper/cef_library_loader.h"
#endif

// App and NativeV8Handler are implementation details of this TU. Declaring
// them here (not in the public header) keeps CEF types off main.cpp's public
// surface.

class App : public CefApp,
            public CefBrowserProcessHandler,
            public CefRenderProcessHandler {
public:
    App() = default;

    // CefApp
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override;
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

    // CefBrowserProcessHandler
    void OnContextInitialized() override;
    void OnScheduleMessagePumpWork(int64_t delay_ms) override;
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

#ifdef __APPLE__
    // external_message_pump support (macOS only). InitPump() installs a
    // CFRunLoopSource and CFRunLoopTimer in the main runloop's common modes;
    // OnScheduleMessagePumpWork signals the source (immediate) or sets the
    // timer's next fire date (delayed). Both are serviced by [NSApp run]'s
    // CFRunLoopRun loop. Must be called once after [NSApplication
    // sharedApplication] and before CefInitialize. Call ShutdownPump() after
    // the post-run CEF drain completes (and before CefShutdown) to invalidate
    // the source/timer and gate any racing wakes.
    static void InitPump();
    static void ShutdownPump();
#endif

    // CefRenderProcessHandler
    void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefDictionaryValue> extra_info) override;
    void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override;
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;

private:
    // Renderer-local map of browser identifier → injection profile passed
    // through extra_info at CreateBrowser time. Populated in OnBrowserCreated,
    // consumed in OnContextCreated, erased in OnBrowserDestroyed.
    std::mutex profiles_mtx_;
    std::unordered_map<int, CefRefPtr<CefDictionaryValue>> profiles_;

    IMPLEMENT_REFCOUNTING(App);
    DISALLOW_COPY_AND_ASSIGN(App);
};

class NativeV8Handler : public CefV8Handler {
public:
    NativeV8Handler(CefRefPtr<CefBrowser> browser) : browser_(browser) {}

    bool Execute(const CefString& name,
                CefRefPtr<CefV8Value> object,
                const CefV8ValueList& arguments,
                CefRefPtr<CefV8Value>& retval,
                CefString& exception) override;

private:
    CefRefPtr<CefBrowser> browser_;
    IMPLEMENT_REFCOUNTING(NativeV8Handler);
};

namespace CefRuntime {

// Process-lifetime state for the main/browser process.
CefMainArgs g_main_args;
CefRefPtr<App> g_app;

// Populated by the Set*() configuration functions. CefSettings fields get
// written directly; command-line switches get queued here and appended in
// App::OnBeforeCommandLineProcessing when CEF invokes it.
CefSettings g_settings;
struct PendingSwitch { std::string name; std::string value; };  // value="" → flag
std::vector<PendingSwitch> g_pending_switches;

namespace {

constexpr const char kSubprocessEnvVar[] = "JELLYFIN_CEF_SUBPROCESS";

#ifndef _WIN32
// Backing storage for the filtered argv we hand to CEF in the browser
// process. Must outlive g_main_args, hence file-scope.
char* g_argv0_only[2];
#endif

#ifdef __APPLE__
// macOS loads libcef dynamically via the helper wrapper. The loader must
// live for the rest of the process, so store it here.
CefScopedLibraryLoader g_library_loader;
#endif

CefMainArgs BuildMainArgs(int argc, char* argv[]) {
#ifdef _WIN32
    (void)argc; (void)argv;
    // Windows: argv is unused; Chromium reads subprocess switches from
    // GetCommandLine() itself.
    SetEnvironmentVariableA(kSubprocessEnvVar, "1");
    return CefMainArgs(GetModuleHandle(NULL));
#else
    // Children inherit this env var from the parent that spawned them.
    // Presence == "I am a CEF-spawned subprocess, pass argv through".
    if (std::getenv(kSubprocessEnvVar)) {
        return CefMainArgs(argc, argv);
    }
    setenv(kSubprocessEnvVar, "1", 1);
    // Initial/browser process: strip argv so the user's shell flags don't
    // reach Chromium's command-line parser.
    g_argv0_only[0] = argv[0];
    g_argv0_only[1] = nullptr;
    return CefMainArgs(1, g_argv0_only);
#endif
}

}  // namespace

int Start(int argc, char* argv[]) {
#ifdef __APPLE__
    if (!g_library_loader.LoadInMain()) {
        fprintf(stderr, "Failed to load CEF library\n");
        return 1;
    }
#endif
    g_main_args = BuildMainArgs(argc, argv);
    g_app = new App();
    // CefExecuteProcess returns >= 0 in subprocesses (GPU/renderer/...) after
    // they've run their course; returns -1 in the main/browser process.
    return CefExecuteProcess(g_main_args, g_app, nullptr);
}

void SetLogSeverity(cef_log_severity_t severity) {
    g_settings.log_severity = severity;
}

void SetRemoteDebuggingPort(int port) {
    g_settings.remote_debugging_port = port;
}

void SetDisableGpuCompositing(bool disable) {
    if (disable) g_pending_switches.push_back({"disable-gpu-compositing", ""});
}

#ifdef __linux__
void SetOzonePlatform(const std::string& platform) {
    if (platform.empty()) return;
    g_pending_switches.push_back({"ozone-platform", platform});
    // CEF's OSR has no native window, so Chromium's per-window scaling override
    // in UpdateScreenInfo resolves to 1.0 and clobbers our device_scale_factor
    // from GetScreenInfo. Disabling the fractional scale protocol avoids that
    // path and keeps HiDPI OSR content scaling correct.
    if (platform == "wayland")
        g_pending_switches.push_back({"disable-features", "WaylandFractionalScaleV1"});
}
#endif

bool Initialize() {
    assert(g_app && "CefRuntime::Start() must be called first");
    CefSettings& settings = g_settings;
    settings.windowless_rendering_enabled = true;
#ifdef __APPLE__
    settings.external_message_pump = true;
#else
    settings.multi_threaded_message_loop = true;
#endif
    settings.no_sandbox = true;
    CefString(&settings.locale).FromASCII("en-US");
    CefString(&settings.user_agent).FromASCII(APP_USER_AGENT);

#ifdef __APPLE__
    char exe_buf[4096];
    uint32_t exe_size = sizeof(exe_buf);
    _NSGetExecutablePath(exe_buf, &exe_size);
    auto exe = std::filesystem::canonical(exe_buf);
    auto app_contents = exe.parent_path().parent_path();
    auto fw_path = (app_contents / "Frameworks" / "Chromium Embedded Framework.framework").string();
    CefString(&settings.framework_dir_path).FromString(fw_path);
    CefString(&settings.browser_subprocess_path).FromString(exe.string());
#elif defined(_WIN32)
    char exe_buf[MAX_PATH];
    GetModuleFileNameA(NULL, exe_buf, MAX_PATH);
    auto exe_path = std::filesystem::canonical(exe_buf);
    auto exe_dir = exe_path.parent_path();
    CefString(&settings.browser_subprocess_path).FromString(exe_path.string());
    CefString(&settings.resources_dir_path).FromString(exe_dir.string());
    CefString(&settings.locales_dir_path).FromString((exe_dir / "locales").string());
#else
    auto exe_path = std::filesystem::canonical("/proc/self/exe");
    CefString(&settings.browser_subprocess_path).FromString(exe_path.string());
#ifdef CEF_RESOURCES_DIR
    std::string res_dir = CEF_RESOURCES_DIR;
    CefString(&settings.resources_dir_path).FromString(res_dir);
    CefString(&settings.locales_dir_path).FromString(res_dir + "/locales");
#else
    auto exe_dir = exe_path.parent_path();
    CefString(&settings.resources_dir_path).FromString(exe_dir.string());
    CefString(&settings.locales_dir_path).FromString((exe_dir / "locales").string());
#endif
#endif
    CefString(&settings.root_cache_path).FromString(paths::getCacheDir());

#ifdef __APPLE__
    // Install the CFRunLoopSource + CFRunLoopTimer that drive the external
    // message pump. Must happen before CefInitialize so the very first
    // OnScheduleMessagePumpWork callback (which fires synchronously during
    // CefInitialize on the calling thread) finds the source/timer ready.
    App::InitPump();
#endif

    return CefInitialize(g_main_args, settings, g_app, nullptr);
}

void Shutdown() {
#ifdef __APPLE__
    // Gate further external-pump dispatches so any blocks that race into the
    // main queue after this point become no-ops instead of calling into CEF
    // state that's about to be torn down.
    App::ShutdownPump();
#endif
    CefShutdown();
}

}  // namespace CefRuntime


#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <limits>
#include <pthread.h>
#include <mach/mach_time.h>
static inline uint64_t tid_u64() {
    uint64_t t = 0;
    pthread_threadid_np(nullptr, &t);
    return t;
}
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

    // Switches queued by the CefRuntime::Set*() configuration functions.
    // Browser process only; CEF propagates to subprocesses as needed.
    if (process_type.empty()) {
        for (const auto& s : CefRuntime::g_pending_switches) {
            if (s.value.empty())
                command_line->AppendSwitch(s.name);
            else
                command_line->AppendSwitchWithValue(s.name, s.value);
        }
    }

#ifdef __APPLE__
    // macOS 26: CEF renderer/GPU subprocesses fail to bootstrap due to a
    // MachPortRendezvous incompatibility, leaving the browser process stuck
    // waiting for a child that never finishes handshaking. Run everything
    // in-process instead. Matches third_party/cef-mpv.
    command_line->AppendSwitch("single-process");

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

void App::OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefDictionaryValue> extra_info) {
    if (!extra_info) return;
    std::lock_guard<std::mutex> lock(profiles_mtx_);
    profiles_[browser->GetIdentifier()] = extra_info;
}

void App::OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) {
    std::lock_guard<std::mutex> lock(profiles_mtx_);
    profiles_.erase(browser->GetIdentifier());
}

void App::OnContextCreated(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) {
    // Injection is a top-frame concern: binding window.jmpNative and running
    // the player shim inside every iframe would duplicate state and pollute
    // unrelated contexts. Sub-frames get nothing.
    if (!frame->IsMain()) return;

    // Look up the browser's injection profile (passed via extra_info at
    // CreateBrowser time, stashed in OnBrowserCreated). No profile → this
    // browser opted out of native-shim injection entirely.
    CefRefPtr<CefDictionaryValue> profile;
    {
        std::lock_guard<std::mutex> lock(profiles_mtx_);
        auto it = profiles_.find(browser->GetIdentifier());
        if (it == profiles_.end()) return;
        profile = it->second;
    }

    CefRefPtr<CefListValue> functions = profile->GetList("functions");
    CefRefPtr<CefListValue> scripts = profile->GetList("scripts");

    // Bind jmpNative with only the functions this browser declared.
    CefRefPtr<CefV8Value> window = context->GetGlobal();
    CefRefPtr<NativeV8Handler> handler = new NativeV8Handler(browser);
    CefRefPtr<CefV8Value> jmpNative = CefV8Value::CreateObject(nullptr, nullptr);
    if (functions) {
        for (size_t i = 0; i < functions->GetSize(); i++) {
            std::string fn = functions->GetString(i).ToString();
            jmpNative->SetValue(fn, CefV8Value::CreateFunction(fn, handler),
                                V8_PROPERTY_ATTRIBUTE_READONLY);
        }
    }
    window->SetValue("jmpNative", jmpNative, V8_PROPERTY_ATTRIBUTE_READONLY);

    if (!scripts || scripts->GetSize() == 0) return;

    // Renderer process is separate from browser process; settings need to be
    // loaded here for placeholder substitution below.
    Settings::instance().load();

    // Concatenate the declared scripts and execute in one call.
    std::string code;
    for (size_t i = 0; i < scripts->GetSize(); i++) {
        if (i > 0) code += '\n';
        code += embedded_js.at(scripts->GetString(i).ToString());
    }

    // Placeholder substitution. No-op if the placeholder isn't present, so
    // profiles that don't include native-shim.js pay nothing here.
    auto replace_first = [&](const std::string& ph, const std::string& value) {
        size_t pos = code.find(ph);
        if (pos != std::string::npos) code.replace(pos, ph.length(), value);
    };
    replace_first("__SERVER_URL__", Settings::instance().serverUrl());
    replace_first("__SETTINGS_JSON__", Settings::instance().cliSettingsJson());
    replace_first("__APP_VERSION__", APP_VERSION);
    if (profile->HasKey("device_profile_json"))
        replace_first("__DEVICE_PROFILE_JSON__",
                      profile->GetString("device_profile_json").ToString());

    frame->ExecuteJavaScript(code, frame->GetURL(), 0);
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

    if (name == "savedServerUrl") {
        CefV8ValueList v8args;
        v8args.push_back(CefV8Value::CreateString(args->GetString(0)));
        callJsGlobal(frame, "_onSavedServerUrl", v8args);
        return true;
    }

    if (name == "serverConnectivityResult") {
        CefV8ValueList v8args;
        v8args.push_back(CefV8Value::CreateString(args->GetString(0)));
        v8args.push_back(CefV8Value::CreateBool(args->GetBool(1)));
        v8args.push_back(CefV8Value::CreateString(args->GetString(2)));
        callJsGlobal(frame, "_onServerConnectivityResult", v8args);
        return true;
    }

    if (name == "getPopupOptions") {
        CefRefPtr<CefListValue> options = CefListValue::Create();
        int selectedIdx = -1;
        CefRefPtr<CefV8Context> ctx = frame->GetV8Context();
        if (ctx && ctx->Enter()) {
            CefRefPtr<CefV8Value> doc = ctx->GetGlobal()->GetValue("document");
            CefRefPtr<CefV8Value> el = doc ? doc->GetValue("activeElement") : nullptr;
            if (el && el->IsObject()) {
                CefRefPtr<CefV8Value> tag = el->GetValue("tagName");
                if (tag && tag->IsString() && tag->GetStringValue() == "SELECT") {
                    CefRefPtr<CefV8Value> opts = el->GetValue("options");
                    CefRefPtr<CefV8Value> lenVal = opts ? opts->GetValue("length") : nullptr;
                    if (opts && opts->IsObject() && lenVal && lenVal->IsInt()) {
                        int len = lenVal->GetIntValue();
                        for (int i = 0; i < len; i++) {
                            CefRefPtr<CefV8Value> opt = opts->GetValue(i);
                            CefString s;
                            if (opt && opt->IsObject()) {
                                CefRefPtr<CefV8Value> t = opt->GetValue("text");
                                if (t && t->IsString()) s = t->GetStringValue();
                            }
                            options->SetString(i, s);
                        }
                        CefRefPtr<CefV8Value> sel = el->GetValue("selectedIndex");
                        if (sel && sel->IsInt()) selectedIdx = sel->GetIntValue();
                    }
                }
            }
            ctx->Exit();
        }
        auto reply = CefProcessMessage::Create("popupOptions");
        reply->GetArgumentList()->SetList(0, options);
        reply->GetArgumentList()->SetInt(1, selectedIdx);
        frame->SendProcessMessage(PID_BROWSER, reply);
        return true;
    }

    // Apply matches what a real click would do: set selectedIndex and
    // fire input + change events so Jellyfin's onchange handlers run.
    if (name == "applyPopupSelection") {
        int idx = args->GetInt(0);
        if (idx >= 0) {
            std::string js = "(function(){var el=document.activeElement;"
                "if(el&&el.tagName==='SELECT'){"
                "el.selectedIndex=" + std::to_string(idx) + ";"
                "el.dispatchEvent(new Event('input',{bubbles:true}));"
                "el.dispatchEvent(new Event('change',{bubbles:true}));"
                "}})();";
            frame->ExecuteJavaScript(js, frame->GetURL(), 0);
        }
        return true;
    }

    return false;
}

// V8 handler -- generic IPC relay to browser process.
// Serializes V8 arguments by detected type; the receiving side handles
// any coercion (e.g. JS numbers that should be ints).
bool NativeV8Handler::Execute(const CefString& name,
                              CefRefPtr<CefV8Value>,
                              const CefV8ValueList& arguments,
                              CefRefPtr<CefV8Value>&,
                              CefString&) {
    CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(name);
    CefRefPtr<CefListValue> args = msg->GetArgumentList();

    for (size_t i = 0; i < arguments.size(); i++) {
        auto& v = arguments[i];
        if      (v->IsBool())   args->SetBool(i, v->GetBoolValue());
        else if (v->IsInt())    args->SetInt(i, v->GetIntValue());
        else if (v->IsDouble()) args->SetDouble(i, v->GetDoubleValue());
        else if (v->IsString()) args->SetString(i, v->GetStringValue());
    }

    browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
    return true;
}

#ifdef __APPLE__
// external_message_pump integration on macOS, mirroring what
// MessagePumpCFRunLoopBase does internally (which CEF's MessagePumpExternal
// declines to do). The wake mechanism is a CFRunLoopSource for immediate
// work and a CFRunLoopTimer for delayed work, both installed in the main
// runloop's common modes. [NSApp run] services them as part of its normal
// CFRunLoopRun loop — fully event-driven, no fixed tick.
//
// Why we drain CefDoMessageLoopWork in a loop inside the source/timer
// callback:
//
// CEF's MessagePumpExternal::Run (libcef/browser/browser_message_loop.cc)
// breaks out of its drain when wall-clock elapsed exceeds max_time_slice_
// (10ms), even when DoWork's last NextWorkInfo was is_immediate(). That
// violates base::MessagePump::ScheduleWork's documented contract
// (base/message_loop/message_pump.h:247-256: "Once this call is made, DoWork
// is guaranteed to be called repeatedly at least until it returns a
// non-immediate NextWorkInfo"), and it leaves the WorkDeduplicator state at
// kDoWorkPending — see thread_controller_with_message_pump_impl.cc:355-360
// and work_deduplicator.cc:62-64. While state is kDoWorkPending, every
// subsequent ScheduleWork call from another thread is suppressed by
// WorkDeduplicator::OnWorkRequested (work_deduplicator.cc:29-34: only
// returns kScheduleImmediate when previous state was kIdle), so
// OnScheduleMessagePumpWork is never called and the pump wedges.
//
// Calling CefDoMessageLoopWork() again clears the wedge: each new call's
// OnWorkStarted unconditionally resets state to kInDoWork
// (work_deduplicator.cc:44-49), draining any tasks queued during the wedge
// window. We loop until a CefDoMessageLoopWork call returns at or below
// the time slice ceiling, which means MessagePumpExternal::Run did not
// have to break early — there's no wedge.
//
// g_pump_shutdown gates the callbacks. Set it via App::ShutdownPump after
// the post-run CEF drain completes to stop any racing wakes from touching
// torn-down CEF state between then and CefShutdown().

static CFRunLoopSourceRef g_work_source = nullptr;
static CFRunLoopTimerRef  g_delayed_timer = nullptr;
static std::atomic<bool>  g_pump_shutdown{false};

// True between OnSched(imm) calling CFRunLoopSourceSignal and the source
// callback actually running. CFRunLoop has no public API to read the
// signaled bit, so we shadow it ourselves. Diagnostic only.
static std::atomic<bool>  g_work_source_pending{false};

// Counters for pump activity, dumped at shutdown.
static std::atomic<uint64_t> g_pump_sched_imm_calls{0};
static std::atomic<uint64_t> g_pump_sched_delayed_calls{0};
static std::atomic<uint64_t> g_pump_source_fired{0};
static std::atomic<uint64_t> g_pump_timer_fired{0};
static std::atomic<uint64_t> g_pump_dmlw_calls{0};

static double mach_ms(uint64_t t0, uint64_t t1) {
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (double)(t1 - t0) * tb.numer / tb.denom / 1e6;
}

// CEF's MessagePumpExternal::Run (libcef/browser/browser_message_loop.cc)
// caps each Run() at max_time_slice_ = 0.01f (10ms). If DoWork is still
// returning is_immediate at that point, Run breaks with the WorkDeduplicator
// state stuck at kDoWorkPending (see
// base/task/sequence_manager/work_deduplicator.cc:62 and
// thread_controller_with_message_pump_impl.cc:355). In that state,
// WorkDeduplicator::OnWorkRequested silently drops subsequent cross-thread
// ScheduleWork calls, so OnScheduleMessagePumpWork stops firing and the
// pump wedges even though CEF has more work queued.
//
// The way out: re-enter CefDoMessageLoopWork. ThreadController::OnWorkStarted
// unconditionally transitions state to kInDoWork (clearing kPendingDoWorkFlag)
// and drains more work. Once a DoWork call completes within the time slice,
// Run returns naturally with state == kIdle and subsequent cross-thread
// schedule calls will notify us again.
//
// We detect the wedge from outside by measuring wall-clock time. CEF's
// max_time_slice_ is 0.01f = 10.0ms (libcef/browser/browser_message_loop.cc
// hardcodes this at the MessagePumpExternal construction site). Run's break
// condition is `delta.InSecondsF() > max_time_slice_`, strict inequality —
// the smallest elapsed that could possibly produce a break is just over
// 10.0ms. Anything at or below 10.0ms means Run returned naturally (either
// no more immediate work, or a delayed deadline > 10ms in the future) and
// state is *not* wedged. Anything over 10.0ms means Run was cut short and
// state is kDoWorkPending.
//
// The response is to re-signal our CFRunLoopSource and yield to the
// runloop: the next runloop turn re-enters us, we call DoWork again,
// state gets unstuck. Cooperative (runloop services NSEvents and other
// sources between iterations) and self-terminating (when elapsed drops
// to or below the threshold, we stop re-arming).
//
// The Metal present path is now non-blocking (IOSurface pool + completion
// handler, not CAMetalLayer nextDrawable), so typical steady-state dmlw
// calls run in well under 1ms and this heuristic never fires outside of
// startup and heavy-work bursts.
//
// If a future CEF version changes max_time_slice_, update this value to
// match.
constexpr double kCefMaxTimeSliceMs = 10.0;

static void pump_drain(const char* trigger) {
    if (g_pump_shutdown.load(std::memory_order_acquire)) {
        LOG_DEBUG(LOG_CEF, "[PUMP] drain({}) skipped (shutdown)", trigger);
        return;
    }

    // Clear the shadow pending bit. Re-entrant OnSched(imm) from any thread
    // during CefDoMessageLoopWork sets it back to true; we use it to tell
    // whether new work has already signaled the source while we were busy.
    g_work_source_pending.store(false, std::memory_order_release);
    g_pump_dmlw_calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t t0 = mach_absolute_time();
    CefDoMessageLoopWork();
    uint64_t t1 = mach_absolute_time();
    double ms = mach_ms(t0, t1);
    bool pending = g_work_source_pending.load(std::memory_order_acquire);

    bool wedged = ms > kCefMaxTimeSliceMs;
    // Two reasons to come back:
    //  - `pending` was set by a cross-thread OnSched(imm) during the call:
    //    CEF has more work and the source is already signaled by our
    //    OnScheduleMessagePumpWork handler. Nothing to do here — the runloop
    //    will re-fire us on its next turn.
    //  - `wedged`: dmlw was cut short on CEF's internal time-slice. State is
    //    kDoWorkPending and cross-thread schedule calls will be silently
    //    dropped until we re-enter. Signal the source ourselves so the
    //    runloop comes back to us after servicing any other pending work.
    if (wedged && !pending) {
        if (g_work_source) {
            g_work_source_pending.store(true, std::memory_order_release);
            CFRunLoopSourceSignal(g_work_source);
            CFRunLoopWakeUp(CFRunLoopGetMain());
        }
    }
}

static void work_source_perform(void* /*info*/) {
    g_pump_source_fired.fetch_add(1, std::memory_order_relaxed);
    pump_drain("source");
}

static void delayed_timer_fire(CFRunLoopTimerRef /*t*/, void* /*info*/) {
    g_pump_timer_fired.fetch_add(1, std::memory_order_relaxed);
    pump_drain("timer");
}

void App::InitPump() {
    LOG_INFO(LOG_CEF, "[PUMP] InitPump: installing CFRunLoopSource + CFRunLoopTimer");
    CFRunLoopSourceContext src_ctx = {};
    src_ctx.perform = work_source_perform;
    g_work_source = CFRunLoopSourceCreate(kCFAllocatorDefault, /*order=*/1, &src_ctx);
    CFRunLoopAddSource(CFRunLoopGetMain(), g_work_source, kCFRunLoopCommonModes);

    // Initial fire date in the far future; armed by OnScheduleMessagePumpWork.
    g_delayed_timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        /*fireDate=*/CFAbsoluteTimeGetCurrent() + 1e10,
        /*interval=*/0, /*flags=*/0, /*order=*/0,
        delayed_timer_fire, /*context=*/nullptr);
    CFRunLoopAddTimer(CFRunLoopGetMain(), g_delayed_timer, kCFRunLoopCommonModes);
}

void App::OnScheduleMessagePumpWork(int64_t delay_ms) {
    if (g_pump_shutdown.load(std::memory_order_acquire)) {
        LOG_DEBUG(LOG_CEF, "[PUMP] OnSched({}) SKIP(shutdown) tid={}",
                 (long long)delay_ms, (unsigned long long)tid_u64());
        return;
    }
    if (delay_ms <= 0) {
        g_pump_sched_imm_calls.fetch_add(1, std::memory_order_relaxed);
        if (g_work_source) {
            g_work_source_pending.store(true, std::memory_order_release);
            // Both calls are documented thread-safe.
            CFRunLoopSourceSignal(g_work_source);
            CFRunLoopWakeUp(CFRunLoopGetMain());
        }
    } else {
        g_pump_sched_delayed_calls.fetch_add(1, std::memory_order_relaxed);
        if (g_delayed_timer) {
            // CFRunLoopTimerSetNextFireDate is thread-safe and *replaces*
            // the previous fire date — exactly the "any currently pending
            // scheduled call should be cancelled" semantics CEF specifies
            // in cef_browser_process_handler.h:135.
            CFRunLoopTimerSetNextFireDate(
                g_delayed_timer,
                CFAbsoluteTimeGetCurrent() + delay_ms / 1000.0);
        }
    }
}


void App::ShutdownPump() {
    LOG_INFO(LOG_CEF, "[PUMP] ShutdownPump: sched_imm={} sched_delayed={} "
             "source_fired=%llu timer_fired=%llu dmlw_calls=%llu",
             (unsigned long long)g_pump_sched_imm_calls.load(),
             (unsigned long long)g_pump_sched_delayed_calls.load(),
             (unsigned long long)g_pump_source_fired.load(),
             (unsigned long long)g_pump_timer_fired.load(),
             (unsigned long long)g_pump_dmlw_calls.load());
    g_pump_shutdown.store(true, std::memory_order_release);
    if (g_delayed_timer) {
        CFRunLoopTimerInvalidate(g_delayed_timer);
        CFRelease(g_delayed_timer);
        g_delayed_timer = nullptr;
    }
    if (g_work_source) {
        CFRunLoopSourceInvalidate(g_work_source);
        CFRelease(g_work_source);
        g_work_source = nullptr;
    }
}
#else
void App::OnScheduleMessagePumpWork(int64_t) {
    // multi_threaded_message_loop on Linux — no pump work needed
}
#endif
