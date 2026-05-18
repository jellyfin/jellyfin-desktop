// macOS external_message_pump integration. Mirrors what
// MessagePumpCFRunLoopBase does internally (which CEF's MessagePumpExternal
// declines to do). CFRunLoopSource for immediate work, CFRunLoopTimer for
// delayed work; both installed in the main runloop's common modes.
//
// Lifted verbatim from the original src/cef/cef_app.cpp pump section. The
// rest of the cef_app.cpp logic moved to the jfn-cef Rust crate; this stays
// in C++ because the wedge-recovery heuristic is tied to a specific CEF
// version's internals and was validated here.

#ifdef __APPLE__

#include "include/cef_app.h"
#include "../logging.h"

#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <cstdint>
#include <mach/mach_time.h>
#include <pthread.h>

namespace {

static inline uint64_t tid_u64() {
    uint64_t t = 0;
    pthread_threadid_np(nullptr, &t);
    return t;
}

CFRunLoopSourceRef g_work_source = nullptr;
CFRunLoopTimerRef  g_delayed_timer = nullptr;
std::atomic<bool>  g_pump_shutdown{false};

// True between OnSched(imm) calling CFRunLoopSourceSignal and the source
// callback actually running. CFRunLoop has no public API to read the
// signaled bit, so we shadow it ourselves. Diagnostic only.
std::atomic<bool>  g_work_source_pending{false};

// Counters for pump activity, dumped at shutdown.
std::atomic<uint64_t> g_pump_sched_imm_calls{0};
std::atomic<uint64_t> g_pump_sched_delayed_calls{0};
std::atomic<uint64_t> g_pump_source_fired{0};
std::atomic<uint64_t> g_pump_timer_fired{0};
std::atomic<uint64_t> g_pump_dmlw_calls{0};

double mach_ms(uint64_t t0, uint64_t t1) {
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (double)(t1 - t0) * tb.numer / tb.denom / 1e6;
}

// CEF's MessagePumpExternal::Run caps each Run() at 0.01f (10ms). If DoWork
// is still returning is_immediate at that point, Run breaks with the
// WorkDeduplicator state stuck at kDoWorkPending. In that state,
// WorkDeduplicator::OnWorkRequested silently drops subsequent cross-thread
// ScheduleWork calls, so OnScheduleMessagePumpWork stops firing and the
// pump wedges.
//
// The way out: re-enter CefDoMessageLoopWork. ThreadController::OnWorkStarted
// unconditionally transitions state to kInDoWork. We detect the wedge by
// measuring wall-clock time. CEF's break condition is strict inequality on
// 10.0ms — anything > 10.0ms means Run was cut short.
constexpr double kCefMaxTimeSliceMs = 10.0;

void pump_drain(const char* trigger) {
    if (g_pump_shutdown.load(std::memory_order_acquire)) {
        LOG_DEBUG(LOG_CEF, "[PUMP] drain({}) skipped (shutdown)", trigger);
        return;
    }

    g_work_source_pending.store(false, std::memory_order_release);
    g_pump_dmlw_calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t t0 = mach_absolute_time();
    CefDoMessageLoopWork();
    uint64_t t1 = mach_absolute_time();
    double ms = mach_ms(t0, t1);
    bool pending = g_work_source_pending.load(std::memory_order_acquire);

    bool wedged = ms > kCefMaxTimeSliceMs;
    if (wedged && !pending) {
        if (g_work_source) {
            g_work_source_pending.store(true, std::memory_order_release);
            CFRunLoopSourceSignal(g_work_source);
            CFRunLoopWakeUp(CFRunLoopGetMain());
        }
    }
}

void work_source_perform(void* /*info*/) {
    g_pump_source_fired.fetch_add(1, std::memory_order_relaxed);
    pump_drain("source");
}

void delayed_timer_fire(CFRunLoopTimerRef /*t*/, void* /*info*/) {
    g_pump_timer_fired.fetch_add(1, std::memory_order_relaxed);
    pump_drain("timer");
}

}  // namespace

extern "C" {

void jfn_cef_macos_pump_init() {
    LOG_INFO(LOG_CEF, "[PUMP] init: installing CFRunLoopSource + CFRunLoopTimer");
    CFRunLoopSourceContext src_ctx = {};
    src_ctx.perform = work_source_perform;
    g_work_source = CFRunLoopSourceCreate(kCFAllocatorDefault, /*order=*/1, &src_ctx);
    CFRunLoopAddSource(CFRunLoopGetMain(), g_work_source, kCFRunLoopCommonModes);

    g_delayed_timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        /*fireDate=*/CFAbsoluteTimeGetCurrent() + 1e10,
        /*interval=*/0, /*flags=*/0, /*order=*/0,
        delayed_timer_fire, /*context=*/nullptr);
    CFRunLoopAddTimer(CFRunLoopGetMain(), g_delayed_timer, kCFRunLoopCommonModes);
}

void jfn_cef_macos_pump_on_schedule(int64_t delay_ms) {
    if (g_pump_shutdown.load(std::memory_order_acquire)) {
        LOG_DEBUG(LOG_CEF, "[PUMP] on_schedule({}) SKIP(shutdown) tid={}",
                 (long long)delay_ms, (unsigned long long)tid_u64());
        return;
    }
    if (delay_ms <= 0) {
        g_pump_sched_imm_calls.fetch_add(1, std::memory_order_relaxed);
        if (g_work_source) {
            g_work_source_pending.store(true, std::memory_order_release);
            CFRunLoopSourceSignal(g_work_source);
            CFRunLoopWakeUp(CFRunLoopGetMain());
        }
    } else {
        g_pump_sched_delayed_calls.fetch_add(1, std::memory_order_relaxed);
        if (g_delayed_timer) {
            CFRunLoopTimerSetNextFireDate(
                g_delayed_timer,
                CFAbsoluteTimeGetCurrent() + delay_ms / 1000.0);
        }
    }
}

void jfn_cef_macos_pump_shutdown() {
    LOG_INFO(LOG_CEF, "[PUMP] shutdown: sched_imm={} sched_delayed={} "
             "source_fired={} timer_fired={} dmlw_calls={}",
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

}  // extern "C"

#endif  // __APPLE__
