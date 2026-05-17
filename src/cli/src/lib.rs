//! Argv parser for jellyfin-desktop. Replaces the hand-rolled C++ parser in
//! `src/cli.cpp` with clap. Help/version short-circuits return distinct kinds
//! so the C++ side can run its own printers (which need `mpv_handle` and
//! version macros not available here).

use clap::error::ContextKind;
use clap::{ArgAction, Parser};
use std::ffi::{CStr, CString, c_char, c_int};
use std::ptr;

#[repr(C)]
pub enum JfnCliResultKind {
    Continue = 0,
    Help = 1,
    Version = 2,
    Error = 3,
}

#[repr(C)]
pub struct JfnCliResult {
    kind: JfnCliResultKind,
    unknown_arg: *mut c_char,

    hwdec: *mut c_char,
    audio_passthrough: *mut c_char,
    audio_channels: *mut c_char,
    log_level: *mut c_char,
    log_file: *mut c_char,
    ozone_platform: *mut c_char,
    platform_override: *mut c_char,

    log_file_set: bool,
    audio_exclusive_set: bool,
    disable_gpu_compositing_set: bool,

    remote_debugging_port: i32,
}

#[derive(Parser, Debug)]
#[command(disable_help_flag = true, disable_version_flag = true)]
struct Cli {
    #[arg(short = 'h', long, action = ArgAction::SetTrue)]
    help: bool,
    #[arg(short = 'v', long, action = ArgAction::SetTrue)]
    version: bool,
    #[arg(long)]
    log_level: Option<String>,
    #[arg(long)]
    log_file: Option<String>,
    #[arg(long)]
    hwdec: Option<String>,
    #[arg(long)]
    audio_passthrough: Option<String>,
    // Count so we can tell "absent" (0) from "present" (>=1) without losing
    // the bool semantics.
    #[arg(long, action = ArgAction::Count)]
    audio_exclusive: u8,
    #[arg(long)]
    audio_channels: Option<String>,
    #[arg(long)]
    remote_debug_port: Option<i32>,
    #[arg(long, action = ArgAction::Count)]
    disable_gpu_compositing: u8,
    #[arg(long)]
    ozone_platform: Option<String>,
    #[arg(long)]
    platform: Option<String>,
}

fn empty_result() -> JfnCliResult {
    JfnCliResult {
        kind: JfnCliResultKind::Continue,
        unknown_arg: ptr::null_mut(),
        hwdec: ptr::null_mut(),
        audio_passthrough: ptr::null_mut(),
        audio_channels: ptr::null_mut(),
        log_level: ptr::null_mut(),
        log_file: ptr::null_mut(),
        ozone_platform: ptr::null_mut(),
        platform_override: ptr::null_mut(),
        log_file_set: false,
        audio_exclusive_set: false,
        disable_gpu_compositing_set: false,
        remote_debugging_port: -1,
    }
}

fn cstring(s: &str) -> *mut c_char {
    CString::new(s).unwrap_or_default().into_raw()
}

fn opt_cstring(s: Option<String>) -> *mut c_char {
    match s {
        Some(v) => cstring(&v),
        None => ptr::null_mut(),
    }
}

fn drop_cstr(p: *mut c_char) {
    if !p.is_null() {
        unsafe { drop(CString::from_raw(p)) };
    }
}

fn parse_inner(args: Vec<String>, have_x11: bool) -> JfnCliResult {
    let mut r = empty_result();
    match Cli::try_parse_from(args) {
        Ok(cli) => {
            if cli.help {
                r.kind = JfnCliResultKind::Help;
                return r;
            }
            if cli.version {
                r.kind = JfnCliResultKind::Version;
                return r;
            }
            if cli.platform.is_some() && !have_x11 {
                r.kind = JfnCliResultKind::Error;
                r.unknown_arg = cstring("--platform");
                return r;
            }
            r.hwdec = opt_cstring(cli.hwdec);
            r.log_level = opt_cstring(cli.log_level);
            if let Some(v) = cli.log_file {
                r.log_file = cstring(&v);
                r.log_file_set = true;
            }
            r.audio_passthrough = opt_cstring(cli.audio_passthrough);
            r.audio_channels = opt_cstring(cli.audio_channels);
            r.ozone_platform = opt_cstring(cli.ozone_platform);
            r.platform_override = opt_cstring(cli.platform);
            r.audio_exclusive_set = cli.audio_exclusive > 0;
            r.disable_gpu_compositing_set = cli.disable_gpu_compositing > 0;
            r.remote_debugging_port = cli.remote_debug_port.unwrap_or(-1);
        }
        Err(err) => {
            r.kind = JfnCliResultKind::Error;
            let bad = err.context().find_map(|(k, v)| match k {
                ContextKind::InvalidArg => Some(v.to_string()),
                _ => None,
            });
            // clap renders missing-value errors as "--flag <VALUE_NAME>";
            // strip the placeholder so the reported arg is just the flag.
            let bad = bad
                .as_deref()
                .map(|s| s.split_whitespace().next().unwrap_or(s).to_string());
            r.unknown_arg = cstring(bad.as_deref().unwrap_or(""));
        }
    }
    r
}

/// # Safety
/// `argv` must point to `argc` valid NUL-terminated C strings (or null
/// entries, which are treated as empty). The returned pointer is heap-owned
/// by Rust; free with [`jfn_cli_result_free`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cli_parse(
    argc: c_int,
    argv: *const *const c_char,
    have_x11: bool,
) -> *mut JfnCliResult {
    if argv.is_null() || argc <= 0 {
        return Box::into_raw(Box::new(empty_result()));
    }
    let mut args = Vec::with_capacity(argc as usize);
    for i in 0..argc as isize {
        let p = unsafe { *argv.offset(i) };
        if p.is_null() {
            args.push(String::new());
        } else {
            args.push(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned());
        }
    }
    Box::into_raw(Box::new(parse_inner(args, have_x11)))
}

/// # Safety
/// `r` must either be null or a pointer returned by [`jfn_cli_parse`]. Each
/// pointer may only be freed once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cli_result_free(r: *mut JfnCliResult) {
    if r.is_null() {
        return;
    }
    let boxed = unsafe { Box::from_raw(r) };
    drop_cstr(boxed.unknown_arg);
    drop_cstr(boxed.hwdec);
    drop_cstr(boxed.audio_passthrough);
    drop_cstr(boxed.audio_channels);
    drop_cstr(boxed.log_level);
    drop_cstr(boxed.log_file);
    drop_cstr(boxed.ozone_platform);
    drop_cstr(boxed.platform_override);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(args: &[&str]) -> JfnCliResult {
        parse_inner(args.iter().map(|s| s.to_string()).collect(), true)
    }

    #[test]
    fn help_short() {
        let r = parse(&["app", "-h"]);
        assert!(matches!(r.kind, JfnCliResultKind::Help));
    }

    #[test]
    fn version_long() {
        let r = parse(&["app", "--version"]);
        assert!(matches!(r.kind, JfnCliResultKind::Version));
    }

    #[test]
    fn unknown_flag() {
        let r = parse(&["app", "--nope"]);
        assert!(matches!(r.kind, JfnCliResultKind::Error));
        assert!(!r.unknown_arg.is_null());
        let s = unsafe { CStr::from_ptr(r.unknown_arg) }.to_str().unwrap();
        assert!(s.contains("--nope"));
        unsafe { jfn_cli_result_free(Box::into_raw(Box::new(r))) };
    }

    #[test]
    fn log_file_explicit_empty() {
        let r = parse(&["app", "--log-file", ""]);
        assert!(matches!(r.kind, JfnCliResultKind::Continue));
        assert!(r.log_file_set);
        let s = unsafe { CStr::from_ptr(r.log_file) }.to_str().unwrap();
        assert_eq!(s, "");
    }

    #[test]
    fn equals_form() {
        let r = parse(&["app", "--hwdec=vaapi", "--remote-debug-port=9222"]);
        assert!(matches!(r.kind, JfnCliResultKind::Continue));
        let s = unsafe { CStr::from_ptr(r.hwdec) }.to_str().unwrap();
        assert_eq!(s, "vaapi");
        assert_eq!(r.remote_debugging_port, 9222);
    }

    #[test]
    fn bool_flags() {
        let r = parse(&["app", "--audio-exclusive", "--disable-gpu-compositing"]);
        assert!(r.audio_exclusive_set);
        assert!(r.disable_gpu_compositing_set);
    }

    #[test]
    fn platform_requires_x11() {
        let args: Vec<String> = ["app", "--platform", "x11"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let r = parse_inner(args, false);
        assert!(matches!(r.kind, JfnCliResultKind::Error));
    }

    #[test]
    fn no_args_continue_all_unset() {
        let r = parse(&["app"]);
        assert!(matches!(r.kind, JfnCliResultKind::Continue));
        assert!(r.hwdec.is_null());
        assert!(r.audio_passthrough.is_null());
        assert!(r.audio_channels.is_null());
        assert!(r.log_level.is_null());
        assert!(r.log_file.is_null());
        assert!(r.ozone_platform.is_null());
        assert!(r.platform_override.is_null());
        assert!(!r.log_file_set);
        assert!(!r.audio_exclusive_set);
        assert!(!r.disable_gpu_compositing_set);
        assert_eq!(r.remote_debugging_port, -1);
    }

    #[test]
    fn help_long() {
        let r = parse(&["app", "--help"]);
        assert!(matches!(r.kind, JfnCliResultKind::Help));
    }

    #[test]
    fn version_short() {
        let r = parse(&["app", "-v"]);
        assert!(matches!(r.kind, JfnCliResultKind::Version));
    }

    #[test]
    fn unknown_short_flag() {
        let r = parse(&["app", "-x"]);
        assert!(matches!(r.kind, JfnCliResultKind::Error));
        let s = unsafe { CStr::from_ptr(r.unknown_arg) }.to_str().unwrap();
        assert!(s.contains("-x"));
    }

    #[test]
    fn positional_is_error() {
        let r = parse(&["app", "positional"]);
        assert!(matches!(r.kind, JfnCliResultKind::Error));
    }

    #[test]
    fn missing_trailing_value_is_error() {
        let r = parse(&["app", "--log-level"]);
        assert!(matches!(r.kind, JfnCliResultKind::Error));
        let s = unsafe { CStr::from_ptr(r.unknown_arg) }.to_str().unwrap();
        assert_eq!(s, "--log-level");
    }

    #[test]
    fn space_form_all_flags() {
        let r = parse(&[
            "app",
            "--hwdec", "vaapi",
            "--log-level", "debug",
            "--log-file", "/tmp/x.log",
            "--audio-passthrough", "ac3,dts-hd",
            "--audio-channels", "5.1",
            "--remote-debug-port", "9222",
            "--ozone-platform", "wayland",
            "--platform", "x11",
        ]);
        assert!(matches!(r.kind, JfnCliResultKind::Continue));
        let cs = |p| unsafe { CStr::from_ptr(p) }.to_str().unwrap();
        assert_eq!(cs(r.hwdec), "vaapi");
        assert_eq!(cs(r.log_level), "debug");
        assert!(r.log_file_set);
        assert_eq!(cs(r.log_file), "/tmp/x.log");
        assert_eq!(cs(r.audio_passthrough), "ac3,dts-hd");
        assert_eq!(cs(r.audio_channels), "5.1");
        assert_eq!(r.remote_debugging_port, 9222);
        assert_eq!(cs(r.ozone_platform), "wayland");
        assert_eq!(cs(r.platform_override), "x11");
    }

    #[test]
    fn log_file_unset_vs_explicit_empty() {
        let r = parse(&["app"]);
        assert!(!r.log_file_set);
        let r = parse(&["app", "--log-file="]);
        assert!(r.log_file_set);
        assert_eq!(unsafe { CStr::from_ptr(r.log_file) }.to_str().unwrap(), "");
    }

    #[test]
    fn remote_debug_port_non_numeric_error() {
        let r = parse(&["app", "--remote-debug-port=bogus"]);
        assert!(matches!(r.kind, JfnCliResultKind::Error));
    }

    #[test]
    fn prefix_collision_log_level_vs_log_file() {
        let r = parse(&["app", "--log-file=path", "--log-level=trace"]);
        assert!(matches!(r.kind, JfnCliResultKind::Continue));
        let cs = |p| unsafe { CStr::from_ptr(p) }.to_str().unwrap();
        assert_eq!(cs(r.log_file), "path");
        assert_eq!(cs(r.log_level), "trace");
    }

    #[test]
    fn error_leaves_value_fields_null() {
        let r = parse(&["app", "--hwdec", "vaapi", "--garbage", "--log-level", "debug"]);
        assert!(matches!(r.kind, JfnCliResultKind::Error));
        assert!(r.hwdec.is_null());
        assert!(r.log_level.is_null());
    }
}
