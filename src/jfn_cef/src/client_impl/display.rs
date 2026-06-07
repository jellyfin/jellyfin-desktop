use cef::*;
use std::os::raw::c_int;
use std::sync::Arc;

use crate::client::Inner;
use jfn_platform_abi::cursor::CursorShape;

#[cfg(target_os = "linux")]
type CursorHandle = std::os::raw::c_ulong;
#[cfg(target_os = "macos")]
type CursorHandle = *mut u8;
#[cfg(target_os = "windows")]
type CursorHandle = sys::HCURSOR;

// ----- DisplayHandler ------------------------------------------------------

wrap_display_handler! {
    pub struct JfnDisplayHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl DisplayHandler {
        fn on_fullscreen_mode_change(
            &self,
            _browser: Option<&mut Browser>,
            fullscreen: c_int,
        ) {
            self.inner.on_fullscreen_mode_change(fullscreen != 0);
        }
        fn on_cursor_change(
            &self,
            _browser: Option<&mut Browser>,
            _cursor: CursorHandle,
            type_: CursorType,
            _custom_cursor_info: Option<&CursorInfo>,
        ) -> c_int {
            let t: sys::cef_cursor_type_t = type_.into();
            if let Some(shape) = CursorShape::from_cef(t as i32) {
                self.inner.emit_cursor(shape);
            }
            1
        }
        fn on_console_message(
            &self,
            _browser: Option<&mut Browser>,
            level: LogSeverity,
            message: Option<&CefString>,
            source: Option<&CefString>,
            line: c_int,
        ) -> c_int {
            let lvl: sys::cef_log_severity_t = level.into();
            let msg = message.map(|s| s.to_string()).unwrap_or_default();
            let src = source.map(|s| s.to_string()).unwrap_or_default();
            self.inner.on_console_message(lvl as c_int, &msg, &src, line);
            1
        }
    }
}
