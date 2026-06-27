use std::ffi::{c_int, c_void};
use std::sync::OnceLock;

type ConnectToFdFn = unsafe extern "C" fn(c_int) -> *mut c_void;

// The resolved fn takes ownership of `fd` — libwayland closes it even on failure.
fn wl_display_connect_to_fd() -> Option<ConnectToFdFn> {
    let addr = unsafe { libc::dlsym(libc::RTLD_DEFAULT, c"wl_display_connect_to_fd".as_ptr()) };
    (!addr.is_null()).then(|| unsafe { std::mem::transmute::<*mut c_void, ConnectToFdFn>(addr) })
}

static APP_DISPLAY: OnceLock<usize> = OnceLock::new();

pub(crate) fn app_display() -> *mut c_void {
    *APP_DISPLAY.get_or_init(|| {
        let fd = crate::mpv_proxy::app_client_fd();
        if fd < 0 {
            tracing::error!(target: "Main", "app_display: no app client fd available");
            return 0;
        }
        let Some(connect) = wl_display_connect_to_fd() else {
            tracing::error!(target: "Main", "app_display: wl_display_connect_to_fd unavailable");
            return 0;
        };
        let d = unsafe { connect(fd) };
        if d.is_null() {
            tracing::error!(target: "Main", "app_display: wl_display_connect_to_fd failed");
            return 0;
        }
        tracing::info!(target: "Main", "app_display: connected on fd={fd} -> {d:p}");
        d as usize
    }) as *mut c_void
}
