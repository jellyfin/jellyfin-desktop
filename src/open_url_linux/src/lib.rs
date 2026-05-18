//! Spawn `xdg-open <url>` detached. Caller ensures the URL is non-empty and
//! doesn't start with '-'.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::process::{Command, Stdio};
use std::thread;

/// # Safety
/// `url` must be a valid NUL-terminated C string or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_open_url(url: *const c_char) {
    if url.is_null() {
        return;
    }
    let s = match unsafe { CStr::from_ptr(url) }.to_str() {
        Ok(s) => s.to_owned(),
        Err(e) => {
            log::error!("open_url: non-utf8 url: {}", e);
            return;
        }
    };

    let child = Command::new("xdg-open")
        .arg(&s)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn();

    match child {
        Ok(mut child) => {
            // xdg-open exits quickly after daemonizing the real handler; reap it.
            thread::spawn(move || {
                let _ = child.wait();
            });
        }
        Err(e) => {
            log::error!("spawn(xdg-open) failed: {}", e);
        }
    }
}
