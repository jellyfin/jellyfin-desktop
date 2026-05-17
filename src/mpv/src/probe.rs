//! Static-version queries that need a transient mpv handle. Mirrors the
//! prior C++ `cli::print_version` mpv block: create a handle, initialize
//! it, read `mpv-version` and `ffmpeg-version`, tear it down.

use crate::handle::Handle;
use std::io::{self, Write};

const PROPS: &[&str] = &["mpv-version", "ffmpeg-version"];

/// Spin up a throwaway handle and return `(property_name, value)` pairs
/// for the static-version properties libmpv exposes. Returns an empty
/// vec if handle creation or initialization fails.
pub fn version_info() -> Vec<(String, String)> {
    let Ok(handle) = Handle::create() else {
        return Vec::new();
    };
    if handle.initialize().is_err() {
        return Vec::new();
    }
    PROPS
        .iter()
        .filter_map(|name| {
            handle
                .get_property_string(name)
                .ok()
                .map(|v| ((*name).to_string(), v))
        })
        .collect()
}

/// C ABI for the C++ `cli::print_version` entry point. Prints each
/// `"<name> <value>\n"` line to stdout.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_mpv_print_version_info() {
    let stdout = io::stdout();
    let mut out = stdout.lock();
    for (name, value) in version_info() {
        let _ = writeln!(out, "{} {}", name, value);
    }
    let _ = out.flush();
}
