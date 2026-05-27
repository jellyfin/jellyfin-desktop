//! Post-init mpv handle accessors used by sibling crates.
//!
//! All entry points borrow the global handle published by
//! [`crate::boot::jfn_mpv_handle_init`]. They no-op silently if the
//! handle has not yet been initialized or has already been terminated.
//!
//! Property writes and commands go through libmpv's async API
//! (`reply_userdata == 0`, fire-and-forget). Property reads are
//! synchronous and must only be issued from non-event contexts; observed
//! properties should be read from the `jfn_playback_*` atomics instead.
//!
//! Stateful helpers — `LoadFile` / `ApplyPendingTrackSelectionAndPlay`
//! / `SetAspectMode` — live here too. The pending-track state is
//! single-threaded by usage but guarded by a Mutex so callers from any
//! thread stay safe.
//!
//! # Safety
//!
//! Every `pub unsafe fn` in this module accepts raw C-string / raw struct
//! pointers preserved from the original FFI surface. Callers must ensure
//! all `*const c_char` arguments point to NUL-terminated UTF-8 (or are
//! null where the function documents tolerance), and that struct
//! pointers reference live values for the duration of the call.

#![allow(clippy::missing_safety_doc)]

use parking_lot::Mutex;
use std::ffi::{CStr, CString, c_char};
use std::os::raw::c_void;

use crate::sys;

// =============================================================================
// Internal helpers
// =============================================================================

fn raw() -> *mut sys::mpv_handle {
    crate::boot::current_raw_handle().unwrap_or(std::ptr::null_mut())
}

unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a CStr> {
    if p.is_null() {
        None
    } else {
        Some(unsafe { CStr::from_ptr(p) })
    }
}

// =============================================================================
// Generic property R/W + command
// =============================================================================

/// Async (`reply_userdata == 0`) flag write. No-op if the handle is
/// missing or `name` is NULL.
pub unsafe fn jfn_mpv_set_property_flag_async(name: *const c_char, value: bool) {
    let h = raw();
    if h.is_null() {
        return;
    }
    let Some(n) = (unsafe { cstr(name) }) else {
        return;
    };
    let mut flag: i32 = if value { 1 } else { 0 };
    unsafe {
        sys::mpv_set_property_async(
            h,
            0,
            n.as_ptr(),
            sys::mpv_format::MPV_FORMAT_FLAG,
            &mut flag as *mut _ as *mut c_void,
        );
    }
}

pub unsafe fn jfn_mpv_set_property_double_async(name: *const c_char, value: f64) {
    let h = raw();
    if h.is_null() {
        return;
    }
    let Some(n) = (unsafe { cstr(name) }) else {
        return;
    };
    let mut v = value;
    unsafe {
        sys::mpv_set_property_async(
            h,
            0,
            n.as_ptr(),
            sys::mpv_format::MPV_FORMAT_DOUBLE,
            &mut v as *mut _ as *mut c_void,
        );
    }
}

pub unsafe fn jfn_mpv_set_property_int_async(name: *const c_char, value: i64) {
    let h = raw();
    if h.is_null() {
        return;
    }
    let Some(n) = (unsafe { cstr(name) }) else {
        return;
    };
    let mut v = value;
    unsafe {
        sys::mpv_set_property_async(
            h,
            0,
            n.as_ptr(),
            sys::mpv_format::MPV_FORMAT_INT64,
            &mut v as *mut _ as *mut c_void,
        );
    }
}

pub unsafe fn jfn_mpv_set_property_string_async(name: *const c_char, value: *const c_char) {
    let h = raw();
    if h.is_null() {
        return;
    }
    let Some(n) = (unsafe { cstr(name) }) else {
        return;
    };
    let Some(v) = (unsafe { cstr(value) }) else {
        return;
    };
    let mut ptr = v.as_ptr();
    unsafe {
        sys::mpv_set_property_async(
            h,
            0,
            n.as_ptr(),
            sys::mpv_format::MPV_FORMAT_STRING,
            &mut ptr as *mut _ as *mut c_void,
        );
    }
}

/// Sync int property read. Writes the value into `*out` and returns
/// libmpv's error code (0 on success, negative on failure). NULL `out`
/// or missing handle returns `MPV_ERROR_INVALID_PARAMETER` (-4).
pub unsafe fn jfn_mpv_get_property_int(name: *const c_char, out: *mut i64) -> i32 {
    let h = raw();
    if h.is_null() || out.is_null() {
        return -4;
    }
    let Some(n) = (unsafe { cstr(name) }) else {
        return -4;
    };
    unsafe {
        sys::mpv_get_property(
            h,
            n.as_ptr(),
            sys::mpv_format::MPV_FORMAT_INT64,
            out as *mut c_void,
        )
    }
}

/// Sync string property read. Returns a malloc'd UTF-8 C string the
/// caller must free with [`jfn_mpv_free_string`], or NULL on failure.
pub unsafe fn jfn_mpv_get_property_string(name: *const c_char) -> *mut c_char {
    let h = raw();
    if h.is_null() {
        return std::ptr::null_mut();
    }
    let Some(n) = (unsafe { cstr(name) }) else {
        return std::ptr::null_mut();
    };
    let p = unsafe { sys::mpv_get_property_string(h, n.as_ptr()) };
    if p.is_null() {
        return std::ptr::null_mut();
    }
    // libmpv owns p (mpv_free required). Copy into a Rust-allocated
    // CString so the caller's free pairs with `jfn_mpv_free_string`.
    let out = unsafe { CStr::from_ptr(p) }.to_owned();
    unsafe { sys::mpv_free(p as *mut c_void) };
    out.into_raw()
}

pub unsafe fn jfn_mpv_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

/// Async command. `args` is a `const char* const*` table of length `n`
/// (no NULL terminator required — the wrapper appends one). No-op on
/// missing handle, empty argv, or NULL entries.
pub unsafe fn jfn_mpv_command_async(args: *const *const c_char, n: usize) {
    let h = raw();
    if h.is_null() || args.is_null() || n == 0 {
        return;
    }
    let slice = unsafe { std::slice::from_raw_parts(args, n) };
    if slice.iter().any(|p| p.is_null()) {
        return;
    }
    let mut argv: Vec<*const c_char> = slice.to_vec();
    argv.push(std::ptr::null());
    unsafe { sys::mpv_command_async(h, 0, argv.as_ptr() as *mut _) };
}

// =============================================================================
// Event drain (wait_event / wakeup).
// =============================================================================

/// Pumps libmpv's event queue. Returns the raw `mpv_event*` libmpv owns;
/// valid only until the next call on the same handle. NULL if the handle
/// is missing.
pub fn jfn_mpv_wait_event(timeout: f64) -> *mut sys::mpv_event {
    let h = raw();
    if h.is_null() {
        return std::ptr::null_mut();
    }
    unsafe { sys::mpv_wait_event(h, timeout) }
}

pub fn jfn_mpv_wakeup() {
    let h = raw();
    if !h.is_null() {
        unsafe { sys::mpv_wakeup(h) };
    }
}

// =============================================================================
// Player API — convenience wrappers over property writes / commands.
// =============================================================================

unsafe fn set_flag(name: &CStr, v: bool) {
    unsafe { jfn_mpv_set_property_flag_async(name.as_ptr(), v) };
}
unsafe fn set_double(name: &CStr, v: f64) {
    unsafe { jfn_mpv_set_property_double_async(name.as_ptr(), v) };
}
unsafe fn set_str(name: &CStr, v: &CStr) {
    unsafe { jfn_mpv_set_property_string_async(name.as_ptr(), v.as_ptr()) };
}
fn cmd(args: &[&CStr]) {
    let ptrs: Vec<*const c_char> = args.iter().map(|s| s.as_ptr()).collect();
    unsafe { jfn_mpv_command_async(ptrs.as_ptr(), ptrs.len()) };
}

pub fn jfn_mpv_play() {
    unsafe { set_flag(c"pause", false) };
}
pub fn jfn_mpv_pause() {
    unsafe { set_flag(c"pause", true) };
}
pub fn jfn_mpv_toggle_pause() {
    cmd(&[c"cycle", c"pause"]);
}
pub fn jfn_mpv_stop() {
    cmd(&[c"stop"]);
}
pub fn jfn_mpv_seek_absolute(secs: f64) {
    let s = CString::new(format!("{}", secs)).unwrap();
    cmd(&[c"seek", &s, c"absolute"]);
}
pub fn jfn_mpv_set_volume(v: f64) {
    unsafe { set_double(c"volume", v) };
}
pub fn jfn_mpv_set_muted(v: bool) {
    unsafe { set_flag(c"mute", v) };
}
pub fn jfn_mpv_set_speed(v: f64) {
    unsafe { set_double(c"speed", v) };
}
pub fn jfn_mpv_set_audio_delay(s: f64) {
    unsafe { set_double(c"audio-delay", s) };
}
pub fn jfn_mpv_set_subtitle_delay(s: f64) {
    unsafe { set_double(c"sub-delay", s) };
}
pub fn jfn_mpv_set_start_position(s: f64) {
    unsafe { set_double(c"start", s) };
}

/// Track id sentinel: 0 = disabled. >=1 = explicit mpv track id.
/// Mpv's auto-track-selection is globally disabled (boot applies
/// `track-auto-selection=no`); jellyfin-web is the authority.
const TRACK_DISABLE: i64 = 0;

fn track_to_mpv_str(id: i64) -> CString {
    if id == TRACK_DISABLE {
        CString::new("no").unwrap()
    } else {
        CString::new(id.to_string()).unwrap()
    }
}

pub fn jfn_mpv_set_audio_track(id: i64) {
    let s = track_to_mpv_str(id);
    unsafe { set_str(c"aid", &s) };
}

pub fn jfn_mpv_set_subtitle_track(id: i64) {
    let s = track_to_mpv_str(id);
    unsafe { set_str(c"sid", &s) };
}

pub unsafe fn jfn_mpv_sub_add(url: *const c_char) {
    let Some(u) = (unsafe { cstr(url) }) else {
        return;
    };
    cmd(&[c"sub-add", u, c"select"]);
}

pub unsafe fn jfn_mpv_audio_add(url: *const c_char) {
    let Some(u) = (unsafe { cstr(url) }) else {
        return;
    };
    cmd(&[c"audio-add", u, c"select"]);
}

// =============================================================================
// LoadFile + deferred track selection (stateful)
// =============================================================================

/// Load options for `LoadFile`. NULL string pointers are treated as empty.
#[repr(C)]
pub struct JfnMpvLoadOptions {
    pub start_secs: f64,
    pub video_track: i64,
    pub audio_track: i64,
    pub sub_track: i64,
    pub external_audio_url: *const c_char,
    pub external_sub_url: *const c_char,
    pub is_infinite_stream: bool,
    pub is_dolby_vision: bool,
}

struct PendingTrack {
    vid: i64,
    aid: i64,
    sid: i64,
    external_audio_url: String,
    external_sub_url: String,
    defer_audio_to_mpv: bool,
    valid: bool,
}

fn pending_slot() -> &'static Mutex<PendingTrack> {
    use std::sync::OnceLock;
    static SLOT: OnceLock<Mutex<PendingTrack>> = OnceLock::new();
    SLOT.get_or_init(|| {
        Mutex::new(PendingTrack {
            vid: 1,
            aid: TRACK_DISABLE,
            sid: TRACK_DISABLE,
            external_audio_url: String::new(),
            external_sub_url: String::new(),
            defer_audio_to_mpv: false,
            valid: false,
        })
    })
}

unsafe fn cstr_to_string(p: *const c_char) -> String {
    unsafe { cstr(p) }
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default()
}

pub unsafe fn jfn_mpv_load_file(path: *const c_char, opts: *const JfnMpvLoadOptions) {
    let Some(path_c) = (unsafe { cstr(path) }) else {
        return;
    };
    let Some(o) = (unsafe { opts.as_ref() }) else {
        return;
    };

    let ext_audio = unsafe { cstr_to_string(o.external_audio_url) };
    let ext_sub = unsafe { cstr_to_string(o.external_sub_url) };
    let defer_audio =
        o.is_infinite_stream && o.audio_track == TRACK_DISABLE && ext_audio.is_empty();

    // Track selection is owned by Jellyfin. With track-auto-selection=no,
    // mpv silently drops aid/vid/sid in loadfile options (loadfile.c
    // skips select_default_track entirely). Load the file *paused* with
    // no selectors, stash the intended ids, and apply them via property
    // writes after FILE_LOADED. The async writes + final pause=false are
    // FIFO-ordered on mpv's core thread, so playback only begins after
    // track-switch reinits land.
    {
        let mut s = pending_slot().lock();
        s.vid = o.video_track;
        s.aid = o.audio_track;
        s.sid = o.sub_track;
        s.external_audio_url = ext_audio;
        s.external_sub_url = ext_sub;
        s.defer_audio_to_mpv = defer_audio;
        s.valid = true;
    }

    #[cfg(target_os = "windows")]
{
    let hint = if o.is_dolby_vision { c"no" } else { c"yes" };
    unsafe { set_str(c"target-colorspace-hint", hint) };
}
    
    let mut opts_str = format!("start={},pause=yes", o.start_secs);
    if defer_audio {
        // Per-file enable so mpv's demuxer picks the format-correct
        // audio track (HLS DEFAULT=YES, MPEG-TS first PMT, etc.). We
        // explicitly write `sid=no` after FILE_LOADED to keep subs off.
        opts_str.push_str(",track-auto-selection=yes");
    }
    let opts_c = CString::new(opts_str).unwrap();
    cmd(&[c"loadfile", path_c, c"replace", c"-1", &opts_c]);
}

pub fn jfn_mpv_apply_pending_track_selection_and_play() {
    let snapshot = {
        let mut s = pending_slot().lock();
        if !s.valid {
            return;
        }
        let snap = (
            s.vid,
            s.aid,
            s.sid,
            std::mem::take(&mut s.external_audio_url),
            std::mem::take(&mut s.external_sub_url),
            s.defer_audio_to_mpv,
        );
        s.valid = false;
        s.defer_audio_to_mpv = false;
        snap
    };
    let (vid, aid, sid, ext_audio, ext_sub, defer_audio) = snapshot;

    let vid_s = track_to_mpv_str(vid);
    unsafe { set_str(c"vid", &vid_s) };
    if !defer_audio {
        // Normal path: jellyfin-web is authoritative. Skipped only for
        // the unprobed-live case (track-auto-selection=yes was set
        // per-file in load_file so mpv's demuxer already picked).
        let aid_s = track_to_mpv_str(aid);
        unsafe { set_str(c"aid", &aid_s) };
    }
    let sid_s = track_to_mpv_str(sid);
    unsafe { set_str(c"sid", &sid_s) };
    if !ext_audio.is_empty() {
        let u = CString::new(ext_audio).unwrap();
        cmd(&[c"audio-add", &u, c"select"]);
    }
    if !ext_sub.is_empty() {
        let u = CString::new(ext_sub).unwrap();
        cmd(&[c"sub-add", &u, c"select"]);
    }
    unsafe { set_flag(c"pause", false) };
}

// =============================================================================
// Aspect-mode helper
// =============================================================================

pub unsafe fn jfn_mpv_set_aspect_mode(mode: *const c_char) {
    let Some(m) = (unsafe { cstr(mode) }) else {
        return;
    };
    let (keepaspect, panscan) = match m.to_bytes() {
        b"auto" => (true, 0.0),
        b"cover" => (true, 1.0),
        b"fill" => (false, 0.0),
        _ => {
            // Unknown mode — silently ignore (matches legacy log-and-skip).
            return;
        }
    };
    unsafe { set_flag(c"keepaspect", keepaspect) };
    unsafe { set_double(c"panscan", panscan) };
}

// =============================================================================
// Window / display
// =============================================================================

pub fn jfn_mpv_set_fullscreen(v: bool) {
    unsafe { set_flag(c"fullscreen", v) };
}
pub fn jfn_mpv_toggle_fullscreen() {
    cmd(&[c"cycle", c"fullscreen"]);
}
pub fn jfn_mpv_set_window_minimized(v: bool) {
    unsafe { set_flag(c"window-minimized", v) };
}
pub fn jfn_mpv_set_window_maximized(v: bool) {
    unsafe { set_flag(c"window-maximized", v) };
}
pub fn jfn_mpv_set_force_window_position(v: bool) {
    unsafe { set_flag(c"force-window-position", v) };
}
pub unsafe fn jfn_mpv_set_geometry(g: *const c_char) {
    let Some(g) = (unsafe { cstr(g) }) else {
        return;
    };
    unsafe { set_str(c"geometry", g) };
}

/// Returns the parsed packed RGB color of mpv's `background-color`
/// property (0x00RRGGBB), or 0 if the property is unavailable or
/// malformed.
pub fn jfn_mpv_get_background_color() -> u32 {
    let p = unsafe { jfn_mpv_get_property_string(c"background-color".as_ptr()) };
    if p.is_null() {
        return 0;
    }
    let s = unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned();
    let rgb = crate::color::parse(&s);
    unsafe { jfn_mpv_free_string(p) };
    rgb
}

pub unsafe fn jfn_mpv_set_background_color_hex(hex: *const c_char) {
    let Some(h) = (unsafe { cstr(hex) }) else {
        return;
    };
    unsafe { set_str(c"background-color", h) };
}
