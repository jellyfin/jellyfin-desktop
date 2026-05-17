//! C ABI for the playback module.
//!
//! The C++ side owns sink objects and registers a vtable (context pointer
//! + function pointers) per sink. The Rust coordinator's worker thread
//! calls those function pointers with a `JfnPlaybackEventC` borrowed from
//! a Rust-owned `PlaybackEvent`. The pointer + length string fields are
//! valid only for the duration of the call.

use std::ffi::{CStr, c_char, c_void};
use std::sync::{Mutex, OnceLock};

use crate::coordinator::{Input, PlaybackCoordinator};
use crate::types::*;

// =====================================================================
// C-friendly mirror types
// =====================================================================

#[repr(C)]
#[derive(Clone, Copy)]
pub struct JfnBufferedRange {
    pub start_ticks: i64,
    pub end_ticks: i64,
}

#[repr(C)]
pub struct JfnPlaybackSnapshotC {
    pub presence: u8, // 0=None 1=Present
    pub phase: u8,    // 0=Starting 1=Playing 2=Paused 3=Stopped
    pub seeking: bool,
    pub buffering: bool,
    pub media_type: u8, // 0=Unknown 1=Audio 2=Video
    pub position_us: i64,
    pub variant_switch_pending: bool,
    pub rate: f64,
    pub duration_us: i64,
    pub fullscreen: bool,
    pub maximized_before_fullscreen: bool,
    pub layout_w: i32,
    pub layout_h: i32,
    pub pixel_w: i32,
    pub pixel_h: i32,
    pub display_hz: f64,
    pub buffered: *const JfnBufferedRange,
    pub buffered_len: usize,
}

#[repr(C)]
pub struct JfnMediaMetadataC {
    pub id: *const c_char,
    pub id_len: usize,
    pub title: *const c_char,
    pub title_len: usize,
    pub artist: *const c_char,
    pub artist_len: usize,
    pub album: *const c_char,
    pub album_len: usize,
    pub track_number: i32,
    pub duration_us: i64,
    pub art_url: *const c_char,
    pub art_url_len: usize,
    pub art_data_uri: *const c_char,
    pub art_data_uri_len: usize,
    pub media_type: u8,
}

#[repr(C)]
pub struct JfnPlaybackEventC {
    pub kind: u8, // PlaybackEventKind
    pub flag: bool,
    pub error_message: *const c_char,
    pub error_message_len: usize,
    pub snapshot: JfnPlaybackSnapshotC,
    pub metadata: JfnMediaMetadataC,
    pub artwork_uri: *const c_char,
    pub artwork_uri_len: usize,
    pub can_go_next: bool,
    pub can_go_prev: bool,
}

#[repr(C)]
pub struct JfnPlaybackActionC {
    pub kind: u8,
}

// =====================================================================
// Sink registry (Rust-side)
// =====================================================================

pub(crate) struct EventSinkEntry {
    pub ctx: *mut c_void,
    pub try_post: extern "C" fn(*mut c_void, *const JfnPlaybackEventC) -> bool,
}
// Safety: function pointer + caller-owned context pointer. C++ sinks live
// at fixed addresses for the program's lifetime; the dispatch path never
// dereferences `ctx` itself.
unsafe impl Send for EventSinkEntry {}
unsafe impl Sync for EventSinkEntry {}

impl EventSinkEntry {
    pub fn dispatch(&self, e: &PlaybackEvent) {
        with_event_c(e, |c| {
            (self.try_post)(self.ctx, c as *const _);
        });
    }
}

pub(crate) struct ActionSinkEntry {
    pub ctx: *mut c_void,
    pub try_post: extern "C" fn(*mut c_void, *const JfnPlaybackActionC) -> bool,
}
unsafe impl Send for ActionSinkEntry {}
unsafe impl Sync for ActionSinkEntry {}

impl ActionSinkEntry {
    pub fn dispatch(&self, a: &PlaybackAction) {
        let c = JfnPlaybackActionC { kind: a.kind as u8 };
        (self.try_post)(self.ctx, &c);
    }
}

// =====================================================================
// Conversion helpers
// =====================================================================

fn snapshot_to_c(s: &PlaybackSnapshot) -> JfnPlaybackSnapshotC {
    JfnPlaybackSnapshotC {
        presence: s.presence as u8,
        phase: s.phase as u8,
        seeking: s.seeking,
        buffering: s.buffering,
        media_type: s.media_type as u8,
        position_us: s.position_us,
        variant_switch_pending: s.variant_switch_pending,
        rate: s.rate,
        duration_us: s.duration_us,
        fullscreen: s.fullscreen,
        maximized_before_fullscreen: s.maximized_before_fullscreen,
        layout_w: s.layout_w,
        layout_h: s.layout_h,
        pixel_w: s.pixel_w,
        pixel_h: s.pixel_h,
        display_hz: s.display_hz,
        buffered: if s.buffered.is_empty() {
            std::ptr::null()
        } else {
            // PlaybackBufferedRange is #[repr(C)] (Default derive plus
            // matching layout); cast is a layout-equivalent reinterpret.
            s.buffered.as_ptr() as *const JfnBufferedRange
        },
        buffered_len: s.buffered.len(),
    }
}

fn metadata_to_c(m: &MediaMetadata) -> JfnMediaMetadataC {
    fn p(s: &str) -> (*const c_char, usize) {
        if s.is_empty() {
            (std::ptr::null(), 0)
        } else {
            (s.as_ptr() as *const c_char, s.len())
        }
    }
    let (id, id_len) = p(&m.id);
    let (title, title_len) = p(&m.title);
    let (artist, artist_len) = p(&m.artist);
    let (album, album_len) = p(&m.album);
    let (art_url, art_url_len) = p(&m.art_url);
    let (art_data_uri, art_data_uri_len) = p(&m.art_data_uri);
    JfnMediaMetadataC {
        id,
        id_len,
        title,
        title_len,
        artist,
        artist_len,
        album,
        album_len,
        track_number: m.track_number,
        duration_us: m.duration_us,
        art_url,
        art_url_len,
        art_data_uri,
        art_data_uri_len,
        media_type: m.media_type as u8,
    }
}

fn with_event_c<F: FnOnce(&JfnPlaybackEventC)>(e: &PlaybackEvent, f: F) {
    fn p(s: &str) -> (*const c_char, usize) {
        if s.is_empty() {
            (std::ptr::null(), 0)
        } else {
            (s.as_ptr() as *const c_char, s.len())
        }
    }
    let (err_ptr, err_len) = p(&e.error_message);
    let (art_ptr, art_len) = p(&e.artwork_uri);
    let c = JfnPlaybackEventC {
        kind: e.kind as u8,
        flag: e.flag,
        error_message: err_ptr,
        error_message_len: err_len,
        snapshot: snapshot_to_c(&e.snapshot),
        metadata: metadata_to_c(&e.metadata),
        artwork_uri: art_ptr,
        artwork_uri_len: art_len,
        can_go_next: e.can_go_next,
        can_go_prev: e.can_go_prev,
    };
    f(&c);
}

fn cstr_to_string(p: *const c_char, len: usize) -> String {
    if p.is_null() || len == 0 {
        String::new()
    } else {
        let slice = unsafe { std::slice::from_raw_parts(p as *const u8, len) };
        String::from_utf8_lossy(slice).into_owned()
    }
}

fn cstr_nul_to_string(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(p) }
            .to_string_lossy()
            .into_owned()
    }
}

// =====================================================================
// Singleton coordinator
// =====================================================================

static COORD: OnceLock<Mutex<Option<PlaybackCoordinator>>> = OnceLock::new();

fn coord_slot() -> &'static Mutex<Option<PlaybackCoordinator>> {
    COORD.get_or_init(|| Mutex::new(None))
}

fn with_coord<F: FnOnce(&PlaybackCoordinator)>(f: F) {
    let guard = coord_slot().lock().unwrap();
    if let Some(c) = guard.as_ref() {
        f(c);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_init() {
    let mut guard = coord_slot().lock().unwrap();
    if guard.is_none() {
        let mut c = PlaybackCoordinator::new();
        c.start();
        *guard = Some(c);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_shutdown() {
    let mut guard = coord_slot().lock().unwrap();
    if let Some(mut c) = guard.take() {
        c.stop();
    }
}

/// Register an event sink. `ctx` is opaque to Rust and passed back to
/// `try_post` on each delivery. Must be called between init and the first
/// post.
///
/// # Safety
/// `try_post` must remain callable for the lifetime of the coordinator,
/// and `ctx` must be valid to pass through unmodified to `try_post`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_register_event_sink(
    ctx: *mut c_void,
    try_post: extern "C" fn(*mut c_void, *const JfnPlaybackEventC) -> bool,
) {
    with_coord(|c| c.add_event_sink(EventSinkEntry { ctx, try_post }));
}

/// Register an action sink.
///
/// # Safety
/// See [`jfn_playback_register_event_sink`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_register_action_sink(
    ctx: *mut c_void,
    try_post: extern "C" fn(*mut c_void, *const JfnPlaybackActionC) -> bool,
) {
    with_coord(|c| c.add_action_sink(ActionSinkEntry { ctx, try_post }));
}

/// Copy the current snapshot into `out`.
///
/// # Safety
/// `out` must point to writable storage for a `JfnPlaybackSnapshotC`.
/// The `buffered` pointer inside the returned snapshot is invalid after
/// the call returns (it borrows from a Rust Vec that goes out of scope);
/// callers that need ranges must copy them while the call is in progress.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_snapshot(out: *mut JfnPlaybackSnapshotC) {
    if out.is_null() {
        return;
    }
    let mut guard = coord_slot().lock().unwrap();
    let Some(c) = guard.as_mut() else {
        return;
    };
    let snap = c.snapshot();
    // Note: buffered slice lifetime is the duration of this function call.
    // Snapshot accessors today (hotkeys) don't read buffered, so this is
    // safe in practice; documented above.
    unsafe { std::ptr::write(out, snapshot_to_c(&snap)) };
    // Snapshot dropped here; out.buffered would be dangling — null it.
    unsafe {
        (*out).buffered = std::ptr::null();
        (*out).buffered_len = 0;
    }
}

// =====================================================================
// Producers
// =====================================================================

fn post(in_: Input) {
    with_coord(|c| c.enqueue(in_));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_file_loaded() {
    post(Input::FileLoaded);
}

/// # Safety
/// `item_id` must be null or a valid NUL-terminated UTF-8 C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_post_load_starting(item_id: *const c_char) {
    post(Input::LoadStarting(cstr_nul_to_string(item_id)));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_pause_changed(paused: bool) {
    post(Input::PauseChanged(paused));
}

/// # Safety
/// `error_message` must be null or a valid NUL-terminated UTF-8 C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_post_end_file(reason: u8, error_message: *const c_char) {
    let reason = match reason {
        0 => EndReason::Eof,
        1 => EndReason::Error,
        _ => EndReason::Canceled,
    };
    post(Input::EndFile {
        reason,
        error_message: cstr_nul_to_string(error_message),
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_seeking_changed(seeking: bool) {
    post(Input::SeekingChanged(seeking));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_paused_for_cache(pfc: bool) {
    post(Input::PausedForCache(pfc));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_core_idle(ci: bool) {
    post(Input::CoreIdle(ci));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_position(position_us: i64) {
    post(Input::Position(position_us));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_media_type(ty: u8) {
    let mt = match ty {
        1 => MediaType::Audio,
        2 => MediaType::Video,
        _ => MediaType::Unknown,
    };
    post(Input::MediaType(mt));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_video_frame_available(available: bool) {
    post(Input::VideoFrameAvailable(available));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_speed(rate: f64) {
    post(Input::Speed(rate));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_duration(duration_us: i64) {
    post(Input::Duration(duration_us));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_fullscreen(fullscreen: bool, was_maximized: bool) {
    post(Input::Fullscreen {
        fullscreen,
        was_maximized,
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_osd_dims(lw: i32, lh: i32, pw: i32, ph: i32) {
    post(Input::OsdDims { lw, lh, pw, ph });
}

/// # Safety
/// `ranges` must point to `len` valid `JfnBufferedRange`s (or be null when
/// `len == 0`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_post_buffered_ranges(
    ranges: *const JfnBufferedRange,
    len: usize,
) {
    let v: Vec<PlaybackBufferedRange> = if len == 0 || ranges.is_null() {
        Vec::new()
    } else {
        let slice = unsafe { std::slice::from_raw_parts(ranges, len) };
        slice
            .iter()
            .map(|r| PlaybackBufferedRange {
                start_ticks: r.start_ticks,
                end_ticks: r.end_ticks,
            })
            .collect()
    };
    post(Input::BufferedRanges(v));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_display_hz(hz: f64) {
    post(Input::DisplayHz(hz));
}

/// # Safety
/// String pointers inside `m` must each be null or point to `*_len` valid
/// bytes of UTF-8 (terminator not required).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_post_metadata(m: *const JfnMediaMetadataC) {
    if m.is_null() {
        return;
    }
    let m = unsafe { &*m };
    let meta = MediaMetadata {
        id: cstr_to_string(m.id, m.id_len),
        title: cstr_to_string(m.title, m.title_len),
        artist: cstr_to_string(m.artist, m.artist_len),
        album: cstr_to_string(m.album, m.album_len),
        track_number: m.track_number,
        duration_us: m.duration_us,
        art_url: cstr_to_string(m.art_url, m.art_url_len),
        art_data_uri: cstr_to_string(m.art_data_uri, m.art_data_uri_len),
        media_type: match m.media_type {
            1 => MediaType::Audio,
            2 => MediaType::Video,
            _ => MediaType::Unknown,
        },
    };
    post(Input::Metadata(meta));
}

/// # Safety
/// `data_uri` must be null or a valid NUL-terminated UTF-8 C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_post_artwork(data_uri: *const c_char) {
    post(Input::Artwork(cstr_nul_to_string(data_uri)));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_queue_caps(can_go_next: bool, can_go_prev: bool) {
    post(Input::QueueCaps {
        can_go_next,
        can_go_prev,
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_seeked(position_us: i64) {
    post(Input::Seeked(position_us));
}
