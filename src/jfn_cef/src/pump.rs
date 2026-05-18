//! macOS external message pump bridge.
//!
//! The pump itself (CFRunLoopSource + CFRunLoopTimer + wedge-recovery
//! heuristic) stays in C++ this slice. CEF's `MessagePumpExternal::Run`
//! contract is tightly coupled to a specific Chromium version's internals
//! and the wedge heuristic depends on a tested time-slice constant. The
//! Rust App forwards the three lifecycle calls into the existing C++
//! implementation, which is exported from `src/cef/macos_pump.cpp` (the
//! macOS pump section of the old `cef_app.cpp`, lifted out unchanged).
//!
//! Follow-up slice will port the pump to Rust once the rest of jfn-cef is
//! stable on macOS.

unsafe extern "C" {
    pub fn jfn_cef_macos_pump_init();
    pub fn jfn_cef_macos_pump_shutdown();
    pub fn jfn_cef_macos_pump_on_schedule(delay_ms: i64);
}

pub(crate) fn init() {
    unsafe { jfn_cef_macos_pump_init() };
}

pub(crate) fn shutdown() {
    unsafe { jfn_cef_macos_pump_shutdown() };
}

pub(crate) fn on_schedule(delay_ms: i64) {
    unsafe { jfn_cef_macos_pump_on_schedule(delay_ms) };
}
