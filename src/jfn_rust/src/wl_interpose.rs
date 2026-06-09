//! Must live in the executable: ELF symbol preemption requires the definition
//! in the dynamic symbol table (`-Wl,--export-dynamic`) to shadow libwayland's
//! for libmpv.

#![cfg(target_os = "linux")]

use std::ffi::{c_char, c_int, c_void};
use std::sync::OnceLock;

type ConnectFn = unsafe extern "C" fn(*const c_char) -> *mut c_void;
type ConnectToFdFn = unsafe extern "C" fn(c_int) -> *mut c_void;

fn real_sym<T: Copy>(slot: &OnceLock<usize>, name: &std::ffi::CStr) -> Option<T> {
    let addr =
        *slot.get_or_init(|| unsafe { libc::dlsym(libc::RTLD_NEXT, name.as_ptr()) } as usize);
    (addr != 0).then(|| unsafe { std::mem::transmute_copy::<usize, T>(&addr) })
}

// Skip the proxy's own upstream connections: recording them would desync the
// registry index from the proxy's same-process client index.
fn record(disp: *mut c_void, via: &str) {
    if disp.is_null() {
        return;
    }
    if jfn_wlproxy::jfn_wlproxy_in_upstream_connect() {
        tracing::debug!(target: "WlInterpose", "{via} -> {disp:p} (proxy upstream, skipped)");
        return;
    }
    let idx = jfn_linux_util::wl_display_registry::record_connect(disp);
    tracing::info!(target: "WlInterpose", "{via} -> {disp:p} (downstream #{idx})");
}

static REAL_CONNECT: OnceLock<usize> = OnceLock::new();
static REAL_CONNECT_TO_FD: OnceLock<usize> = OnceLock::new();

/// # Safety
/// Matches libwayland's `wl_display_connect` ABI; `name` is null or a valid C
/// string per the caller (libmpv / libwayland conventions).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wl_display_connect(name: *const c_char) -> *mut c_void {
    let Some(real) = real_sym::<ConnectFn>(&REAL_CONNECT, c"wl_display_connect") else {
        tracing::error!(target: "WlInterpose", "no real wl_display_connect via RTLD_NEXT");
        return std::ptr::null_mut();
    };
    let disp = unsafe { real(name) };
    record(disp, "wl_display_connect");
    disp
}

/// # Safety
/// Matches libwayland's `wl_display_connect_to_fd` ABI; `fd` is an open socket
/// fd ownership-transferred to libwayland (Chromium/Ozone's connect path).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wl_display_connect_to_fd(fd: c_int) -> *mut c_void {
    let Some(real) = real_sym::<ConnectToFdFn>(&REAL_CONNECT_TO_FD, c"wl_display_connect_to_fd")
    else {
        tracing::error!(target: "WlInterpose", "no real wl_display_connect_to_fd via RTLD_NEXT");
        return std::ptr::null_mut();
    };
    let disp = unsafe { real(fd) };
    record(disp, "wl_display_connect_to_fd");
    disp
}

/// Force-reference the exports so LTO/gc-sections can't strip them from the
/// dynamic symbol table. Called once from `main`.
pub fn ensure_linked() {
    std::hint::black_box(wl_display_connect as *const ());
    std::hint::black_box(wl_display_connect_to_fd as *const ());
}
