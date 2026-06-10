//! macOS [`CefHost`]: framework loader + external message pump on the
//! main CFRunLoop + CADisplayLink-driven BeginFrame.

use std::sync::OnceLock;

use jfn_platform_abi::CefHost;

pub struct MacosCefHost;

impl CefHost for MacosCefHost {
    fn before_start(&self) {
        // macOS distributes CEF as a framework loaded at runtime via a thunk
        // table in libcef_dll_wrapper (`cef_load_library` populates it).
        // Without this, every CEF call dispatches through a NULL pointer.
        static LOADER: OnceLock<cef::library_loader::LibraryLoader> = OnceLock::new();
        LOADER.get_or_init(|| {
            #[allow(clippy::expect_used)] // no CEF without an executable path
            let exe = std::env::current_exe().expect("current_exe");
            let loader = cef::library_loader::LibraryLoader::new(&exe, false);
            assert!(loader.load(), "failed to load Chromium Embedded Framework");
            loader
        });
    }

    fn pump_init(&self) {
        crate::cef_pump::init();
    }

    fn pump_schedule(&self, delay_ms: i64) {
        crate::cef_pump::on_schedule(delay_ms);
    }

    fn pump_shutdown(&self) {
        crate::cef_pump::shutdown();
    }

    fn external_begin_frame(&self) -> bool {
        true
    }
}
