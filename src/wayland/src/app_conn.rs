use std::ffi::{c_int, c_void};
use std::ptr::NonNull;

use parking_lot::Mutex;

type ConnectToFdFn = unsafe extern "C" fn(c_int) -> *mut c_void;

// The resolved fn takes ownership of `fd` — libwayland closes it even on failure.
fn wl_display_connect_to_fd() -> Option<ConnectToFdFn> {
    let addr = unsafe { libc::dlsym(libc::RTLD_DEFAULT, c"wl_display_connect_to_fd".as_ptr()) };
    (!addr.is_null()).then(|| unsafe { std::mem::transmute::<*mut c_void, ConnectToFdFn>(addr) })
}

/// The app-side `wl_display`. Sharing the raw pointer across threads is sound:
/// libwayland's display is internally synchronized and the app already drives it
/// from the root thread. Callers take the raw pointer only at the FFI boundary
/// via [`AppDisplay::as_ptr`], keeping the ownership contract in the type.
#[derive(Clone, Copy)]
pub(crate) struct AppDisplay(NonNull<c_void>);
unsafe impl Send for AppDisplay {}
unsafe impl Sync for AppDisplay {}

impl AppDisplay {
    pub(crate) fn as_ptr(self) -> *mut c_void {
        self.0.as_ptr()
    }
}

/// `Unattempted` retries on the next call; `Failed` does not. Only a `fd < 0`
/// (proxy hasn't published the client fd yet) stays `Unattempted` — once
/// `connect` runs it has consumed the fd, so its result is terminal either way.
enum DisplayState {
    Unattempted,
    Connected(AppDisplay),
    Failed,
}

static APP_DISPLAY: Mutex<DisplayState> = Mutex::new(DisplayState::Unattempted);

pub(crate) fn app_display() -> Option<AppDisplay> {
    let mut state = APP_DISPLAY.lock();
    match &*state {
        DisplayState::Connected(a) => return Some(*a),
        DisplayState::Failed => return None,
        DisplayState::Unattempted => {}
    }
    let Some(fd) = crate::mpv_proxy::app_client_fd() else {
        tracing::error!(target: "Main", "app_display: no app client fd available");
        return None;
    };
    let Some(connect) = wl_display_connect_to_fd() else {
        tracing::error!(target: "Main", "app_display: wl_display_connect_to_fd unavailable");
        *state = DisplayState::Failed;
        return None;
    };
    let Some(d) = NonNull::new(unsafe { connect(fd) }) else {
        tracing::error!(target: "Main", "app_display: wl_display_connect_to_fd failed");
        *state = DisplayState::Failed;
        return None;
    };
    tracing::info!(target: "Main", "app_display: connected on fd={fd} -> {:p}", d.as_ptr());
    let display = AppDisplay(d);
    *state = DisplayState::Connected(display);
    Some(display)
}
