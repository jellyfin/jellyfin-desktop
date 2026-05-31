//! Process entry point. Forwards into [`jfn_rust::app::jfn_app_main`],
//! which owns the full boot/run/shutdown sequence (CEF subprocess
//! dispatch, settings load, platform install, mpv boot, browser run
//! loop, teardown).

use std::ffi::CString;
use std::os::raw::{c_char, c_int};

fn main() {
    // Panic hook: route panics through tracing so they land in the same log
    // file as everything else (stderr is not captured by `just run` on Windows).
    let default_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let bt = std::backtrace::Backtrace::force_capture();
        tracing::error!(target: "panic", "PANIC: {info}\n{bt}");
        eprintln!("PANIC: {info}\n{bt}");
        default_hook(info);
    }));

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
