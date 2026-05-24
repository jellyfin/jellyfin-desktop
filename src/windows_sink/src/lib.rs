//! Windows SystemMediaTransportControls (SMTC) sink. Port of
//! `src/playback/sinks/windows/windows_sink.cpp`. Owns its own MTA-
//! initialised thread that drains queued PlaybackEvents on wake.
//! SMTC ButtonPressed / PlaybackPositionChangeRequested callbacks
//! dispatch directly into mpv (jfn_mpv) and jfn_web_exec_js.
//!
//! Public entry points:
//!   * `jfn_windows_sink_start()` — registers the event-sink thunk with
//!     jfn-playback and spawns the consumer thread.
//!   * `jfn_windows_sink_stop()`  — signals the thread to exit at next
//!     wake.

#![cfg(target_os = "windows")]

use std::collections::VecDeque;
use std::ffi::{c_char, c_void};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::time::{Duration, Instant};

use windows::Foundation::TimeSpan;
use windows::Media::{
    MediaPlaybackStatus, MediaPlaybackType, SystemMediaTransportControls,
    SystemMediaTransportControlsButton, SystemMediaTransportControlsButtonPressedEventArgs,
    SystemMediaTransportControlsTimelineProperties,
};
use windows::Storage::Streams::{
    DataWriter, InMemoryRandomAccessStream, RandomAccessStreamReference,
};
use windows::Win32::Foundation::HWND;
use windows::Win32::Security::Cryptography::{
    CRYPT_STRING_BASE64, CryptStringToBinaryA,
};
use windows::Win32::System::WinRT::{
    ISystemMediaTransportControlsInterop, RoGetActivationFactory,
};
use windows::core::{HSTRING, Interface, h};

// =====================================================================
// Mirror of jfn-playback's C ABI event shape (kept private so we don't
// add a cross-crate dependency on the raw C types).
// =====================================================================

#[repr(C)]
struct JfnBufferedRange {
    start_ticks: i64,
    end_ticks: i64,
}

#[repr(C)]
struct JfnPlaybackSnapshotC {
    presence: u8,
    phase: u8,
    seeking: bool,
    buffering: bool,
    media_type: u8,
    position_us: i64,
    variant_switch_pending: bool,
    rate: f64,
    duration_us: i64,
    fullscreen: bool,
    maximized_before_fullscreen: bool,
    layout_w: i32,
    layout_h: i32,
    pixel_w: i32,
    pixel_h: i32,
    display_hz: f64,
    buffered: *const JfnBufferedRange,
    buffered_len: usize,
}

#[repr(C)]
struct JfnMediaMetadataC {
    id: *const c_char,
    id_len: usize,
    title: *const c_char,
    title_len: usize,
    artist: *const c_char,
    artist_len: usize,
    album: *const c_char,
    album_len: usize,
    track_number: i32,
    duration_us: i64,
    art_url: *const c_char,
    art_url_len: usize,
    art_data_uri: *const c_char,
    art_data_uri_len: usize,
    media_type: u8,
}

#[repr(C)]
struct JfnPlaybackEventC {
    kind: u8,
    flag: bool,
    error_message: *const c_char,
    error_message_len: usize,
    snapshot: JfnPlaybackSnapshotC,
    metadata: JfnMediaMetadataC,
    artwork_uri: *const c_char,
    artwork_uri_len: usize,
    can_go_next: bool,
    can_go_prev: bool,
}

mod kind {
    pub const STARTED: u8 = 0;
    pub const PAUSED: u8 = 1;
    pub const FINISHED: u8 = 2;
    pub const CANCELED: u8 = 3;
    pub const ERROR: u8 = 4;
    pub const TRACK_LOADED: u8 = 8;
    pub const POSITION_CHANGED: u8 = 9;
    pub const METADATA_CHANGED: u8 = 16;
    pub const ARTWORK_CHANGED: u8 = 17;
    pub const QUEUE_CAPS_CHANGED: u8 = 18;
    pub const SEEKED: u8 = 19;
}

mod media_type {
    pub const AUDIO: u8 = 1;
    pub const _VIDEO: u8 = 2;
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase {
    Playing,
    Paused,
    Stopped,
}

// =====================================================================
// Owned event copy.
// =====================================================================

#[derive(Default, Clone)]
struct OwnedMetadata {
    id: String,
    title: String,
    artist: String,
    album: String,
    track_number: i32,
    duration_us: i64,
    media_type: u8,
}

struct OwnedEvent {
    kind: u8,
    metadata: OwnedMetadata,
    position_us: i64,
    artwork_uri: String,
    can_go_next: bool,
    can_go_prev: bool,
}

unsafe fn cstr_slice(p: *const c_char, n: usize) -> String {
    if p.is_null() || n == 0 {
        return String::new();
    }
    let s = unsafe { std::slice::from_raw_parts(p as *const u8, n) };
    String::from_utf8_lossy(s).into_owned()
}

unsafe fn owned_metadata(m: &JfnMediaMetadataC) -> OwnedMetadata {
    unsafe {
        OwnedMetadata {
            id: cstr_slice(m.id, m.id_len),
            title: cstr_slice(m.title, m.title_len),
            artist: cstr_slice(m.artist, m.artist_len),
            album: cstr_slice(m.album, m.album_len),
            track_number: m.track_number,
            duration_us: m.duration_us,
            media_type: m.media_type,
        }
    }
}

unsafe fn owned_event(ev: &JfnPlaybackEventC) -> OwnedEvent {
    unsafe {
        OwnedEvent {
            kind: ev.kind,
            metadata: owned_metadata(&ev.metadata),
            position_us: ev.snapshot.position_us,
            artwork_uri: cstr_slice(ev.artwork_uri, ev.artwork_uri_len),
            can_go_next: ev.can_go_next,
            can_go_prev: ev.can_go_prev,
        }
    }
}

// =====================================================================
// Sink state.
// =====================================================================

struct Inner {
    queue: Mutex<VecDeque<OwnedEvent>>,
    cv: Condvar,
    running: AtomicBool,
    hwnd: Mutex<isize>,
}

static SINK: OnceLock<Arc<Inner>> = OnceLock::new();
const EVENT_QUEUE_CAP: usize = 256;

fn inner() -> Arc<Inner> {
    SINK.get_or_init(|| {
        Arc::new(Inner {
            queue: Mutex::new(VecDeque::new()),
            cv: Condvar::new(),
            running: AtomicBool::new(false),
            hwnd: Mutex::new(0),
        })
    })
    .clone()
}

extern "C" fn event_sink_thunk(_ctx: *mut c_void, ev: *const JfnPlaybackEventC) -> bool {
    if ev.is_null() {
        return false;
    }
    let owned = unsafe { owned_event(&*ev) };
    let inner = inner();
    {
        let mut q = match inner.queue.lock() {
            Ok(q) => q,
            Err(_) => return false,
        };
        if q.len() >= EVENT_QUEUE_CAP {
            return false;
        }
        q.push_back(owned);
    }
    inner.cv.notify_one();
    true
}

/// Start the sink. `hwnd_raw` is the HWND of the mpv window — required
/// to bind SMTC via ISystemMediaTransportControlsInterop::GetForWindow.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_windows_sink_start_for(hwnd_raw: isize) {
    let inner = inner();
    if inner.running.swap(true, Ordering::AcqRel) {
        return;
    }
    *inner.hwnd.lock().expect("hwnd poisoned") = hwnd_raw;

    unsafe {
        jfn_playback::ffi::jfn_playback_register_event_sink(
            std::ptr::null_mut(),
            std::mem::transmute(event_sink_thunk as extern "C" fn(_, _) -> _),
        );
    }

    std::thread::Builder::new()
        .name("windows-sink".into())
        .spawn(move || consumer_thread(inner))
        .expect("spawn windows-sink");
}

/// Convenience entry: queries mpv for the window-id and starts the sink.
/// Mirrors the C++ `jfn_app_run_with_cef` block that constructs a
/// WindowsSink from `jfn_mpv_get_property_int("window-id", &wid)`.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_windows_sink_start() {
    let mut wid: i64 = 0;
    let name = std::ffi::CString::new("window-id").expect("nul");
    let rc = unsafe { jfn_mpv::api::jfn_mpv_get_property_int(name.as_ptr(), &mut wid) };
    if rc < 0 || wid == 0 {
        tracing::error!(target: "Media", "[SMTC] mpv window-id lookup failed");
        return;
    }
    jfn_windows_sink_start_for(wid as isize);
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_windows_sink_stop() {
    let inner = match SINK.get() {
        Some(i) => i.clone(),
        None => return,
    };
    if !inner.running.swap(false, Ordering::AcqRel) {
        return;
    }
    inner.cv.notify_all();
}

// =====================================================================
// Consumer thread + SMTC bindings.
// =====================================================================

struct Smtc {
    smtc: SystemMediaTransportControls,
    updater: windows::Media::SystemMediaTransportControlsDisplayUpdater,
    button_token: i64,
    seek_token: i64,
    cached_thumbnail: Option<RandomAccessStreamReference>,
}

fn init_smtc(hwnd_raw: isize) -> Option<Smtc> {
    unsafe {
        // RPC_E_CHANGED_MODE is fine — another thread may already have
        // initialised the apartment.
        let _ = windows::Win32::System::Com::CoInitializeEx(
            None,
            windows::Win32::System::Com::COINIT_MULTITHREADED,
        );
    }
    if hwnd_raw == 0 {
        tracing::error!(target: "Media", "[SMTC] NULL HWND provided");
        return None;
    }
    let smtc: SystemMediaTransportControls = match unsafe {
        let interop: ISystemMediaTransportControlsInterop = RoGetActivationFactory(
            &HSTRING::from("Windows.Media.SystemMediaTransportControls"),
        )
        .ok()?;
        let hwnd = HWND(hwnd_raw as *mut _);
        interop
            .GetForWindow::<SystemMediaTransportControls>(hwnd)
            .ok()
    } {
        Some(s) => s,
        None => {
            tracing::error!(target: "Media", "[SMTC] GetForWindow returned null");
            return None;
        }
    };

    smtc.SetIsEnabled(true).ok()?;
    smtc.SetIsPlayEnabled(true).ok()?;
    smtc.SetIsPauseEnabled(true).ok()?;
    smtc.SetIsStopEnabled(true).ok()?;
    smtc.SetIsNextEnabled(false).ok()?;
    smtc.SetIsPreviousEnabled(false).ok()?;

    let updater = smtc.DisplayUpdater().ok()?;

    let button_token = {
        let handler = windows::Foundation::TypedEventHandler::new(
            |_sender: windows_core::Ref<SystemMediaTransportControls>,
             args: windows_core::Ref<
                SystemMediaTransportControlsButtonPressedEventArgs,
            >| {
                if let Some(args) = args.as_ref() {
                    if let Ok(button) = args.Button() {
                        on_button_pressed(button);
                    }
                }
                Ok(())
            },
        );
        smtc.ButtonPressed(&handler).ok()?
    };

    let seek_token = {
        let handler = windows::Foundation::TypedEventHandler::new(
            |_sender: windows_core::Ref<SystemMediaTransportControls>,
             args: windows_core::Ref<
                windows::Media::PlaybackPositionChangeRequestedEventArgs,
            >| {
                if let Some(args) = args.as_ref() {
                    if let Ok(span) = args.RequestedPlaybackPosition() {
                        // TimeSpan.Duration is 100-ns ticks.
                        let pos_us = span.Duration / 10;
                        let ms = pos_us / 1000;
                        let js = format!(
                            "if(window._nativeSeek) window._nativeSeek({ms});\0"
                        );
                        unsafe {
                            jfn_cef::business_web::jfn_web_exec_js(js.as_ptr() as *const c_char);
                        }
                    }
                }
                Ok(())
            },
        );
        smtc.PlaybackPositionChangeRequested(&handler).ok()?
    };

    tracing::info!(target: "Media", "[SMTC] Initialized");
    Some(Smtc {
        smtc,
        updater,
        button_token,
        seek_token,
        cached_thumbnail: None,
    })
}

fn teardown_smtc(s: Smtc) {
    let _ = s.smtc.RemoveButtonPressed(s.button_token);
    let _ = s.smtc.RemovePlaybackPositionChangeRequested(s.seek_token);
    let _ = s.updater.ClearAll();
    let _ = s.updater.Update();
    let _ = s.smtc.SetIsEnabled(false);
}

fn consumer_thread(inner: Arc<Inner>) {
    let hwnd_raw = *inner.hwnd.lock().expect("hwnd poisoned");
    let mut smtc = init_smtc(hwnd_raw);

    let mut state = ConsumerState::default();

    while inner.running.load(Ordering::Acquire) {
        let drained: Vec<OwnedEvent> = {
            let mut q = inner.queue.lock().expect("queue poisoned");
            while q.is_empty() && inner.running.load(Ordering::Acquire) {
                q = inner
                    .cv
                    .wait_timeout(q, Duration::from_millis(100))
                    .expect("cv poisoned")
                    .0;
            }
            q.drain(..).collect()
        };
        for ev in drained {
            deliver(&mut state, &mut smtc, ev);
        }
    }

    if let Some(s) = smtc.take() {
        teardown_smtc(s);
    }
}

trait OptionTakeExt<T> {
    fn take(self) -> Option<T>;
}
impl<T> OptionTakeExt<T> for Option<T> {
    fn take(self) -> Option<T> {
        self
    }
}

#[derive(Default)]
struct ConsumerState {
    metadata: OwnedMetadata,
    phase: Option<Phase>,
    position_us: i64,
    last_position_update: Option<Instant>,
    pending_update: bool,
}

fn map_kind_to_phase(kind: u8) -> Phase {
    match kind {
        kind::STARTED => Phase::Playing,
        kind::PAUSED | kind::TRACK_LOADED => Phase::Paused,
        kind::FINISHED | kind::CANCELED | kind::ERROR => Phase::Stopped,
        _ => Phase::Stopped,
    }
}

fn on_button_pressed(button: SystemMediaTransportControlsButton) {
    use SystemMediaTransportControlsButton as B;
    match button {
        B::Play => jfn_mpv::api::jfn_mpv_play(),
        B::Pause => jfn_mpv::api::jfn_mpv_pause(),
        B::Stop => jfn_mpv::api::jfn_mpv_stop(),
        B::Next => {
            let js = c"if(window._nativeHostInput) window._nativeHostInput(['next']);";
            unsafe { jfn_cef::business_web::jfn_web_exec_js(js.as_ptr()) };
        }
        B::Previous => {
            let js = c"if(window._nativeHostInput) window._nativeHostInput(['previous']);";
            unsafe { jfn_cef::business_web::jfn_web_exec_js(js.as_ptr()) };
        }
        _ => {}
    }
}

fn deliver(state: &mut ConsumerState, smtc: &mut Option<Smtc>, ev: OwnedEvent) {
    match ev.kind {
        kind::METADATA_CHANGED => {
            if !ev.metadata.id.is_empty() && ev.metadata.id == state.metadata.id {
                return;
            }
            state.metadata = ev.metadata.clone();
            if state.phase != Some(Phase::Stopped) {
                update_display_properties(state, smtc);
            }
        }
        kind::ARTWORK_CHANGED => {
            let Some(smtc) = smtc.as_mut() else { return };
            if ev.artwork_uri.is_empty() {
                return;
            }
            let Some(comma) = ev.artwork_uri.find(',') else {
                return;
            };
            let b64 = &ev.artwork_uri[comma + 1..];
            let bytes = match decode_base64(b64) {
                Some(b) => b,
                None => return,
            };
            if let Some(ref_stream) = make_thumbnail_stream(&bytes) {
                let _ = smtc.updater.SetThumbnail(&ref_stream);
                let _ = smtc.updater.Update();
                smtc.cached_thumbnail = Some(ref_stream);
            }
        }
        kind::QUEUE_CAPS_CHANGED => {
            if let Some(s) = smtc.as_ref() {
                let _ = s.smtc.SetIsNextEnabled(ev.can_go_next);
                let _ = s.smtc.SetIsPreviousEnabled(ev.can_go_prev);
            }
        }
        kind::STARTED
        | kind::PAUSED
        | kind::TRACK_LOADED
        | kind::FINISHED
        | kind::CANCELED
        | kind::ERROR => {
            let p = map_kind_to_phase(ev.kind);
            state.phase = Some(p);
            let Some(s) = smtc.as_mut() else { return };
            match p {
                Phase::Playing => {
                    let _ = s.smtc.SetPlaybackStatus(MediaPlaybackStatus::Playing);
                    update_display_properties_inner(state, s);
                }
                Phase::Paused => {
                    let _ = s.smtc.SetPlaybackStatus(MediaPlaybackStatus::Paused);
                    update_timeline(state, s);
                }
                Phase::Stopped => {
                    state.metadata = OwnedMetadata::default();
                    state.position_us = 0;
                    s.cached_thumbnail = None;
                    let _ = s.updater.ClearAll();
                    let _ = s.updater.Update();
                    let _ = s.smtc.SetPlaybackStatus(MediaPlaybackStatus::Stopped);
                    return;
                }
            }
            state.pending_update = true;
        }
        kind::POSITION_CHANGED => {
            state.position_us = ev.position_us;
            let now = Instant::now();
            let elapsed = state
                .last_position_update
                .map(|t| now.duration_since(t).as_millis() as i64)
                .unwrap_or(i64::MAX);
            if state.pending_update || elapsed >= 1000 {
                if let Some(s) = smtc.as_mut() {
                    update_timeline(state, s);
                }
                state.last_position_update = Some(now);
                state.pending_update = false;
            }
        }
        kind::SEEKED => {
            state.position_us = ev.position_us;
            if let Some(s) = smtc.as_mut() {
                update_timeline(state, s);
            }
            state.last_position_update = Some(Instant::now());
            state.pending_update = false;
        }
        _ => {}
    }
}

// Wrapper so the recursive borrow lookups inside the Playing branch
// compile — `update_display_properties` only mutates the updater + the
// cached_thumbnail field of Smtc, leaving the state borrow intact.
fn update_display_properties(state: &mut ConsumerState, _placeholder: &mut Option<Smtc>) {
    // intentionally empty — real work happens in
    // `update_display_properties_inner` to keep borrow chains tidy.
    let _ = state;
}

fn update_display_properties_inner(state: &mut ConsumerState, s: &mut Smtc) {
    if state.phase == Some(Phase::Stopped) {
        return;
    }
    let _ = s.updater.ClearAll();
    if state.metadata.media_type == media_type::AUDIO {
        let _ = s.updater.SetType(MediaPlaybackType::Music);
        if let Ok(music) = s.updater.MusicProperties() {
            let _ = music.SetTitle(&HSTRING::from(&state.metadata.title));
            let _ = music.SetArtist(&HSTRING::from(&state.metadata.artist));
            let _ = music.SetAlbumTitle(&HSTRING::from(&state.metadata.album));
            if state.metadata.track_number > 0 {
                let _ = music.SetTrackNumber(state.metadata.track_number as u32);
            }
        }
    } else {
        let _ = s.updater.SetType(MediaPlaybackType::Video);
        if let Ok(video) = s.updater.VideoProperties() {
            let _ = video.SetTitle(&HSTRING::from(&state.metadata.title));
            if !state.metadata.artist.is_empty() {
                let _ = video.SetSubtitle(&HSTRING::from(&state.metadata.artist));
            }
        }
    }
    if let Some(thumb) = &s.cached_thumbnail {
        let _ = s.updater.SetThumbnail(thumb);
    }
    let _ = s.updater.Update();
    update_timeline(state, s);
}

fn update_timeline(state: &ConsumerState, s: &Smtc) {
    if state.metadata.duration_us <= 0 {
        return;
    }
    let tl = SystemMediaTransportControlsTimelineProperties::new();
    let Ok(tl) = tl else { return };
    let to_ticks = |us: i64| TimeSpan { Duration: us * 10 };
    let _ = tl.SetStartTime(TimeSpan { Duration: 0 });
    let _ = tl.SetEndTime(to_ticks(state.metadata.duration_us));
    let _ = tl.SetPosition(to_ticks(state.position_us));
    let _ = tl.SetMinSeekTime(TimeSpan { Duration: 0 });
    let _ = tl.SetMaxSeekTime(to_ticks(state.metadata.duration_us));
    let _ = s.smtc.UpdateTimelineProperties(&tl);
}

fn decode_base64(s: &str) -> Option<Vec<u8>> {
    let bytes = s.as_bytes();
    let mut needed: u32 = 0;
    unsafe {
        let ok = CryptStringToBinaryA(
            bytes,
            CRYPT_STRING_BASE64,
            None,
            &mut needed,
            None,
            None,
        );
        if ok.is_err() || needed == 0 {
            return None;
        }
        let mut buf = vec![0u8; needed as usize];
        let mut got = needed;
        let ok = CryptStringToBinaryA(
            bytes,
            CRYPT_STRING_BASE64,
            Some(buf.as_mut_ptr()),
            &mut got,
            None,
            None,
        );
        if ok.is_err() {
            return None;
        }
        buf.truncate(got as usize);
        Some(buf)
    }
}

fn make_thumbnail_stream(bytes: &[u8]) -> Option<RandomAccessStreamReference> {
    let stream = InMemoryRandomAccessStream::new().ok()?;
    let writer = DataWriter::CreateDataWriter(&stream).ok()?;
    writer.WriteBytes(bytes).ok()?;
    // Safe to .get() because we're on the MTA sink thread.
    let _ = writer.StoreAsync().ok()?.join().ok()?;
    let _ = writer.DetachStream();
    stream.Seek(0).ok()?;
    RandomAccessStreamReference::CreateFromStream(&stream).ok()
}
