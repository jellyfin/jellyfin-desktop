//! macOS external message pump.
//!
//! Mirrors what `MessagePumpCFRunLoopBase` does internally (which CEF's
//! `MessagePumpExternal` declines to do). A `CFRunLoopSource` services
//! immediate work; a `CFRunLoopTimer` services delayed work; both are
//! installed in the main runloop's common modes.
//!
//! The wedge-recovery heuristic is preserved verbatim because it's tied to
//! a specific CEF version's `WorkDeduplicator` internals.

use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU64, Ordering};
use std::time::Instant;

// ----- CoreFoundation FFI ---------------------------------------------------

#[allow(non_camel_case_types)]
type CFIndex = isize;
#[allow(non_camel_case_types)]
type CFAbsoluteTime = f64;
#[allow(non_camel_case_types)]
type CFTimeInterval = f64;
#[allow(non_camel_case_types)]
type CFOptionFlags = usize;
#[allow(non_camel_case_types)]
type CFHashCode = usize;
#[allow(non_camel_case_types)]
type Boolean = u8;

type CFAllocatorRef = *const c_void;
type CFRunLoopRef = *mut c_void;
type CFRunLoopSourceRef = *mut c_void;
type CFRunLoopTimerRef = *mut c_void;
type CFStringRef = *const c_void;
type CFTypeRef = *const c_void;

#[repr(C)]
struct CFRunLoopSourceContext {
    version: CFIndex,
    info: *mut c_void,
    retain: Option<unsafe extern "C" fn(*const c_void) -> *const c_void>,
    release: Option<unsafe extern "C" fn(*const c_void)>,
    copy_description: Option<unsafe extern "C" fn(*const c_void) -> CFStringRef>,
    equal: Option<unsafe extern "C" fn(*const c_void, *const c_void) -> Boolean>,
    hash: Option<unsafe extern "C" fn(*const c_void) -> CFHashCode>,
    schedule: Option<unsafe extern "C" fn(*mut c_void, CFRunLoopRef, CFStringRef)>,
    cancel: Option<unsafe extern "C" fn(*mut c_void, CFRunLoopRef, CFStringRef)>,
    perform: Option<unsafe extern "C" fn(*mut c_void)>,
}

#[repr(C)]
struct CFRunLoopTimerContext {
    version: CFIndex,
    info: *mut c_void,
    retain: Option<unsafe extern "C" fn(*const c_void) -> *const c_void>,
    release: Option<unsafe extern "C" fn(*const c_void)>,
    copy_description: Option<unsafe extern "C" fn(*const c_void) -> CFStringRef>,
}

#[link(name = "CoreFoundation", kind = "framework")]
unsafe extern "C" {
    static kCFRunLoopCommonModes: CFStringRef;

    fn CFRunLoopGetMain() -> CFRunLoopRef;
    fn CFRunLoopWakeUp(rl: CFRunLoopRef);
    fn CFRunLoopAddSource(rl: CFRunLoopRef, source: CFRunLoopSourceRef, mode: CFStringRef);
    fn CFRunLoopAddTimer(rl: CFRunLoopRef, timer: CFRunLoopTimerRef, mode: CFStringRef);
    fn CFRunLoopSourceCreate(
        allocator: CFAllocatorRef,
        order: CFIndex,
        context: *mut CFRunLoopSourceContext,
    ) -> CFRunLoopSourceRef;
    fn CFRunLoopSourceSignal(source: CFRunLoopSourceRef);
    fn CFRunLoopSourceInvalidate(source: CFRunLoopSourceRef);
    fn CFRunLoopTimerCreate(
        allocator: CFAllocatorRef,
        fire_date: CFAbsoluteTime,
        interval: CFTimeInterval,
        flags: CFOptionFlags,
        order: CFIndex,
        callout: Option<unsafe extern "C" fn(CFRunLoopTimerRef, *mut c_void)>,
        context: *mut CFRunLoopTimerContext,
    ) -> CFRunLoopTimerRef;
    fn CFRunLoopTimerSetNextFireDate(timer: CFRunLoopTimerRef, fire_date: CFAbsoluteTime);
    fn CFRunLoopTimerInvalidate(timer: CFRunLoopTimerRef);
    fn CFAbsoluteTimeGetCurrent() -> CFAbsoluteTime;
    fn CFRelease(cf: CFTypeRef);
}

// ----- State ----------------------------------------------------------------

static WORK_SOURCE: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
static DELAYED_TIMER: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
static PUMP_SHUTDOWN: AtomicBool = AtomicBool::new(false);
// True between on_schedule(imm) signalling the source and the source
// callback actually running. CFRunLoop has no public API to read the
// signaled bit, so we shadow it ourselves. Diagnostic only.
static WORK_SOURCE_PENDING: AtomicBool = AtomicBool::new(false);

static SCHED_IMM_CALLS: AtomicU64 = AtomicU64::new(0);
static SCHED_DELAYED_CALLS: AtomicU64 = AtomicU64::new(0);
static SOURCE_FIRED: AtomicU64 = AtomicU64::new(0);
static TIMER_FIRED: AtomicU64 = AtomicU64::new(0);
static DMLW_CALLS: AtomicU64 = AtomicU64::new(0);

// CEF's MessagePumpExternal::Run caps each Run() at 0.01f (10ms). If DoWork
// is still returning is_immediate at that point, Run breaks with the
// WorkDeduplicator state stuck at kDoWorkPending. In that state,
// WorkDeduplicator::OnWorkRequested silently drops subsequent cross-thread
// ScheduleWork calls, so OnScheduleMessagePumpWork stops firing and the
// pump wedges.
//
// The way out: re-enter cef::do_message_loop_work. ThreadController::OnWorkStarted
// unconditionally transitions state to kInDoWork. We detect the wedge by
// measuring wall-clock time. CEF's break condition is strict inequality on
// 10.0ms — anything > 10.0ms means Run was cut short.
const CEF_MAX_TIME_SLICE_MS: f64 = 10.0;

fn pump_drain(trigger: &str) {
    if PUMP_SHUTDOWN.load(Ordering::Acquire) {
        if jfn_logging::log_enabled(jfn_logging::CATEGORY_CEF, jfn_logging::LEVEL_DEBUG) {
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_DEBUG,
                &format!("[PUMP] drain({trigger}) skipped (shutdown)"),
            );
        }
        return;
    }

    WORK_SOURCE_PENDING.store(false, Ordering::Release);
    DMLW_CALLS.fetch_add(1, Ordering::Relaxed);
    let t0 = Instant::now();
    cef::do_message_loop_work();
    let ms = t0.elapsed().as_secs_f64() * 1e3;
    let pending = WORK_SOURCE_PENDING.load(Ordering::Acquire);

    let wedged = ms > CEF_MAX_TIME_SLICE_MS;
    if wedged && !pending {
        let src = WORK_SOURCE.load(Ordering::Acquire);
        if !src.is_null() {
            WORK_SOURCE_PENDING.store(true, Ordering::Release);
            unsafe {
                CFRunLoopSourceSignal(src);
                CFRunLoopWakeUp(CFRunLoopGetMain());
            }
        }
    }
}

unsafe extern "C" fn work_source_perform(_info: *mut c_void) {
    SOURCE_FIRED.fetch_add(1, Ordering::Relaxed);
    pump_drain("source");
}

unsafe extern "C" fn delayed_timer_fire(_timer: CFRunLoopTimerRef, _info: *mut c_void) {
    TIMER_FIRED.fetch_add(1, Ordering::Relaxed);
    pump_drain("timer");
}

// ----- Public API -----------------------------------------------------------

pub(crate) fn init() {
    jfn_logging::log(
        jfn_logging::CATEGORY_CEF,
        jfn_logging::LEVEL_INFO,
        "[PUMP] init: installing CFRunLoopSource + CFRunLoopTimer",
    );

    let mut src_ctx = CFRunLoopSourceContext {
        version: 0,
        info: std::ptr::null_mut(),
        retain: None,
        release: None,
        copy_description: None,
        equal: None,
        hash: None,
        schedule: None,
        cancel: None,
        perform: Some(work_source_perform),
    };
    let source = unsafe { CFRunLoopSourceCreate(std::ptr::null(), 1, &mut src_ctx) };
    WORK_SOURCE.store(source, Ordering::Release);
    unsafe {
        CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    }

    let timer = unsafe {
        CFRunLoopTimerCreate(
            std::ptr::null(),
            CFAbsoluteTimeGetCurrent() + 1e10,
            0.0,
            0,
            0,
            Some(delayed_timer_fire),
            std::ptr::null_mut(),
        )
    };
    DELAYED_TIMER.store(timer, Ordering::Release);
    unsafe {
        CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    }
}

pub(crate) fn on_schedule(delay_ms: i64) {
    if PUMP_SHUTDOWN.load(Ordering::Acquire) {
        if jfn_logging::log_enabled(jfn_logging::CATEGORY_CEF, jfn_logging::LEVEL_DEBUG) {
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_DEBUG,
                &format!("[PUMP] on_schedule({delay_ms}) SKIP(shutdown)"),
            );
        }
        return;
    }
    if delay_ms <= 0 {
        SCHED_IMM_CALLS.fetch_add(1, Ordering::Relaxed);
        let src = WORK_SOURCE.load(Ordering::Acquire);
        if !src.is_null() {
            WORK_SOURCE_PENDING.store(true, Ordering::Release);
            unsafe {
                CFRunLoopSourceSignal(src);
                CFRunLoopWakeUp(CFRunLoopGetMain());
            }
        }
    } else {
        SCHED_DELAYED_CALLS.fetch_add(1, Ordering::Relaxed);
        let timer = DELAYED_TIMER.load(Ordering::Acquire);
        if !timer.is_null() {
            unsafe {
                CFRunLoopTimerSetNextFireDate(
                    timer,
                    CFAbsoluteTimeGetCurrent() + delay_ms as f64 / 1000.0,
                );
            }
        }
    }
}

pub(crate) fn shutdown() {
    jfn_logging::log(
        jfn_logging::CATEGORY_CEF,
        jfn_logging::LEVEL_INFO,
        &format!(
            "[PUMP] shutdown: sched_imm={} sched_delayed={} source_fired={} timer_fired={} dmlw_calls={}",
            SCHED_IMM_CALLS.load(Ordering::Relaxed),
            SCHED_DELAYED_CALLS.load(Ordering::Relaxed),
            SOURCE_FIRED.load(Ordering::Relaxed),
            TIMER_FIRED.load(Ordering::Relaxed),
            DMLW_CALLS.load(Ordering::Relaxed),
        ),
    );
    PUMP_SHUTDOWN.store(true, Ordering::Release);

    let timer = DELAYED_TIMER.swap(std::ptr::null_mut(), Ordering::AcqRel);
    if !timer.is_null() {
        unsafe {
            CFRunLoopTimerInvalidate(timer);
            CFRelease(timer);
        }
    }
    let source = WORK_SOURCE.swap(std::ptr::null_mut(), Ordering::AcqRel);
    if !source.is_null() {
        unsafe {
            CFRunLoopSourceInvalidate(source);
            CFRelease(source);
        }
    }
}
