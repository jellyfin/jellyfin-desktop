//! How the platform drives CEF's message loop.
//!
//! Present (`Platform::cef_host` returns `Some`) only on backends where
//! the platform must pump CEF itself (macOS: external message pump on the
//! main CFRunLoop, CADisplayLink-driven BeginFrame, framework loader).
//! Backends returning `None` run CEF's own multi-threaded message loop.

pub trait CefHost: Send + Sync {
    /// Runs before the FIRST CEF API call in any process — including
    /// `CefExecuteProcess` — e.g. to load the CEF framework so calls
    /// don't dispatch through a NULL thunk table.
    fn before_start(&self);

    /// Install the pump's run-loop hooks. Runs before `CefInitialize` so
    /// the first `OnScheduleMessagePumpWork` (fired synchronously during
    /// init) finds them ready.
    fn pump_init(&self);

    /// CEF's `OnScheduleMessagePumpWork` — schedule a pump after
    /// `delay_ms` (immediately when <= 0). May fire from any thread.
    fn pump_schedule(&self, delay_ms: i64);

    /// Gate further pump dispatches before CEF state is torn down.
    fn pump_shutdown(&self);

    /// Whether browsers are created with external BeginFrame enabled —
    /// the platform drives frame production (e.g. via CADisplayLink).
    fn external_begin_frame(&self) -> bool;
}
