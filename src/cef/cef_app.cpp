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

#ifdef __linux__
    // Only the browser process sets ozone platform; CEF propagates to subprocesses.
    if (process_type.empty()) {
        command_line->AppendSwitchWithValue("ozone-platform", ozone_platform_);

        // Disable fractional scale protocol when using ozone-platform=wayland.
        // CEF's OSR has no native window, so Chromium's per-window scaling override
        // in UpdateScreenInfo resolves to 1.0 and clobbers our device_scale_factor
        // from GetScreenInfo. Without this, HiDPI content scaling breaks in OSR.
        if (ozone_platform_ == "wayland") {
            command_line->AppendSwitchWithValue(
                "disable-features", "WaylandFractionalScaleV1");
        }
    }
#endif

    if (disable_gpu_compositing_) {
        command_line->AppendSwitch("disable-gpu-compositing");
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
    jmpNative->SetValue("playerAddSubtitle", CefV8Value::CreateFunction("playerAddSubtitle", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
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
               name == "themeColor" || name == "playerAddSubtitle") {
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
        LOG_INFO(LOG_CEF, "[PUMP] drain(%s) skipped (shutdown)", trigger);
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
        LOG_INFO(LOG_CEF, "[PUMP] OnSched(%lld) SKIP(shutdown) tid=%llu",
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
    LOG_INFO(LOG_CEF, "[PUMP] ShutdownPump: sched_imm=%llu sched_delayed=%llu "
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
