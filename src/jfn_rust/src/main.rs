//! Process entry point. Forwards into [`jfn_rust::app::jfn_app_main`],
//! which owns the full boot/run/shutdown sequence (CEF subprocess
//! dispatch, settings load, platform install, mpv boot, browser run
//! loop, teardown). Replaces the historical `src/main.cpp` shim.

use std::ffi::CString;
use std::os::raw::{c_char, c_int};

fn main() {
    // Collect argv into NUL-terminated C strings so the existing
    // `jfn_app_main(argc, argv)` ABI doesn't need to change. We hold
    // the CStrings for the lifetime of the call so the borrowed
    // pointers remain valid.
    let args: Vec<CString> = std::env::args()
        .map(|a| CString::new(a).unwrap_or_else(|_| CString::new("").unwrap()))
        .collect();
    let argv: Vec<*const c_char> = args.iter().map(|c| c.as_ptr()).collect();
    let argc = argv.len() as c_int;

    let rc = unsafe { jfn_rust::app::jfn_app_main(argc, argv.as_ptr()) };
    std::process::exit(rc);
}
