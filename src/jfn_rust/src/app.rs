//! Process entry point. C++ main.cpp shrinks to a forwarder that calls
//! [`jfn_app_main`]; new logic moves out of main.cpp into this module
//! incrementally. Until the port completes, `jfn_app_main` returns -1 to
//! signal "continue in C++", or a >= 0 exit code when the subprocess
//! dispatch / CLI short-circuits should terminate the process.

use std::ffi::c_int;
use std::os::raw::c_char;

/// Subprocess dispatch. In a Chromium subprocess (renderer / GPU /
/// utility), `jfn_cef_start` returns the subprocess exit code (>= 0) and
/// we pass it straight through. In the browser process it returns -1 and
/// we continue startup.
///
/// # Safety
/// `argv` must point to `argc` valid NUL-terminated C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_app_main(argc: c_int, argv: *const *const c_char) -> c_int {
    jfn_cef::ffi::jfn_cef_start(argc, argv)
}
