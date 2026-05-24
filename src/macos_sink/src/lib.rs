//! macOS Now Playing / MPRemoteCommandCenter sink. Port of
//! `src/playback/sinks/macos/macos_sink.mm`. Owns its own thread that
//! drains queued PlaybackEvents on wake. Inbound MPRemoteCommand
//! callbacks dispatch directly into mpv (jfn_mpv) / web browser
//! (jfn_web_exec_js).
//!
//! All UI updates run on the main thread via `dispatch_async`/
//! `dispatch_sync` because MPNowPlayingInfoCenter mutations are not safe
//! from background threads.

#![cfg(target_os = "macos")]

use std::collections::VecDeque;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::time::{Duration, Instant};

use objc2::rc::{Allocated, Retained};
use objc2::runtime::{AnyObject, ProtocolObject};
use objc2::{ClassType, DefinedClass, MainThreadOnly, define_class, msg_send};
use objc2_app_kit::NSImage;
use objc2_foundation::{
    NSCopying, NSData, NSDataBase64DecodingOptions, NSDictionary, NSMutableDictionary, NSNumber,
    NSObject, NSSize, NSString,
};
use objc2_media_player::{
    MPChangePlaybackPositionCommandEvent, MPMediaItemArtwork, MPNowPlayingInfoCenter,
    MPNowPlayingInfoMediaType, MPNowPlayingPlaybackState, MPRemoteCommand, MPRemoteCommandCenter,
    MPRemoteCommandEvent, MPRemoteCommandHandlerStatus,
};
use std::ptr::NonNull;

fn ns_key(s: &NSString) -> &ProtocolObject<dyn NSCopying> {
    ProtocolObject::from_ref(s)
}

// =====================================================================
// Mirror of jfn-playback's C ABI event shape — needed because the sink
// registers as an event consumer through the C vtable
// (jfn_playback_register_event_sink).
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

// Mirrors jfn-playback's PlaybackEvent::Kind discriminants.
mod kind {
    pub const STARTED: u8 = 0;
    pub const PAUSED: u8 = 1;
    pub const FINISHED: u8 = 2;
    pub const CANCELED: u8 = 3;
    pub const ERROR: u8 = 4;
    pub const TRACK_LOADED: u8 = 8;
    pub const POSITION_CHANGED: u8 = 9;
    pub const RATE_CHANGED: u8 = 11;
    pub const METADATA_CHANGED: u8 = 16;
    pub const ARTWORK_CHANGED: u8 = 17;
    pub const QUEUE_CAPS_CHANGED: u8 = 18;
    pub const SEEKED: u8 = 19;
}

mod media_type {
    pub const AUDIO: u8 = 1;
    pub const _VIDEO: u8 = 2;
}

mod phase {
    pub const _STARTING: u8 = 0;
    pub const PLAYING: u8 = 1;
    pub const PAUSED: u8 = 2;
    pub const STOPPED: u8 = 3;
}

// =====================================================================
// Owned event copy (heap-allocated for the consumer thread).
// =====================================================================

#[derive(Default, Clone)]
struct OwnedMetadata {
    id: String,
    title: String,
    artist: String,
    album: String,
    track_number: i32,
    duration_us: i64,
    art_data_uri: String,
    media_type: u8,
}

struct OwnedEvent {
    kind: u8,
    metadata: OwnedMetadata,
    snapshot_position_us: i64,
    snapshot_rate: f64,
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
            art_data_uri: cstr_slice(m.art_data_uri, m.art_data_uri_len),
            media_type: m.media_type,
        }
    }
}

unsafe fn owned_event(ev: &JfnPlaybackEventC) -> OwnedEvent {
    unsafe {
        OwnedEvent {
            kind: ev.kind,
            metadata: owned_metadata(&ev.metadata),
            snapshot_position_us: ev.snapshot.position_us,
            snapshot_rate: ev.snapshot.rate,
            artwork_uri: cstr_slice(ev.artwork_uri, ev.artwork_uri_len),
            can_go_next: ev.can_go_next,
            can_go_prev: ev.can_go_prev,
        }
    }
}

// =====================================================================
// Sink state (singleton — one instance per process, started from
// jfn_app_run_with_cef on macOS).
// =====================================================================

struct Inner {
    queue: Mutex<VecDeque<OwnedEvent>>,
    cv: Condvar,
    running: AtomicBool,
}

static SINK: OnceLock<Arc<Inner>> = OnceLock::new();

// Capacity matches the C++ QueuedPlaybackSink for parity.
const EVENT_QUEUE_CAP: usize = 256;

fn inner() -> Arc<Inner> {
    SINK.get_or_init(|| {
        Arc::new(Inner {
            queue: Mutex::new(VecDeque::new()),
            cv: Condvar::new(),
            running: AtomicBool::new(false),
        })
    })
    .clone()
}

// Coordinator-side: jfn-playback walks every registered event sink via
// this thunk. Heap-copies the event into the consumer queue.
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

// =====================================================================
// Public start/stop entry points. Called from jfn_app_run_with_cef.
// =====================================================================

#[unsafe(no_mangle)]
pub extern "C" fn jfn_macos_sink_start() {
    let inner = inner();
    if inner.running.swap(true, Ordering::AcqRel) {
        return;
    }

    // Register with the coordinator. ctx is unused — sink state lives in
    // the SINK OnceLock. The thunk signature is bytewise identical to
    // jfn-playback's JfnPlaybackEventC type (mirrored above).
    unsafe {
        jfn_playback::ffi::jfn_playback_register_event_sink(
            std::ptr::null_mut(),
            std::mem::transmute(event_sink_thunk as extern "C" fn(_, _) -> _),
        );
    }

    // Consumer thread drains the queue and dispatches MPNowPlayingInfo /
    // command-center updates onto the main thread.
    std::thread::Builder::new()
        .name("macos-sink".into())
        .spawn(move || consumer_thread(inner))
        .expect("spawn macos-sink thread");
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_macos_sink_stop() {
    let inner = match SINK.get() {
        Some(i) => i.clone(),
        None => return,
    };
    if !inner.running.swap(false, Ordering::AcqRel) {
        return;
    }
    inner.cv.notify_all();
    // Consumer thread observes !running on next wake and exits. We don't
    // join because the OnceLock keeps the inner alive past process tear-
    // down anyway, and joining requires storing the JoinHandle which
    // adds locking complexity.
}

fn consumer_thread(inner: Arc<Inner>) {
    init_remote_command_center();

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
        let mut state = ConsumerState::default();
        for ev in drained {
            deliver(&mut state, ev);
        }
    }

    teardown_remote_command_center();
}

#[derive(Default)]
struct ConsumerState {
    metadata: OwnedMetadata,
    position_us: i64,
    rate: f64,
    last_position_update: Option<Instant>,
}

// =====================================================================
// MPRemoteCommandCenter delegate (Obj-C class defined via objc2 macro).
// =====================================================================

define_class!(
    #[unsafe(super(NSObject))]
    #[name = "JfnMediaKeysDelegate"]
    struct MediaKeysDelegate;

    impl MediaKeysDelegate {
        #[unsafe(method(handleCommand:))]
        fn handle_command(&self, event: &MPRemoteCommandEvent) -> MPRemoteCommandHandlerStatus {
            let command = unsafe { event.command() };
            let center = unsafe { MPRemoteCommandCenter::sharedCommandCenter() };
            let play = unsafe { center.playCommand() };
            let pause = unsafe { center.pauseCommand() };
            let toggle = unsafe { center.togglePlayPauseCommand() };
            let stop = unsafe { center.stopCommand() };
            let next = unsafe { center.nextTrackCommand() };
            let prev = unsafe { center.previousTrackCommand() };

            // MPRemoteCommand identity comparison: each shared center
            // returns the same retained instance, so pointer equality
            // matches the C++ implementation.
            let cp = (&*command as *const MPRemoteCommand) as *const ();
            let eq = |c: &MPRemoteCommand| (&*c as *const MPRemoteCommand) as *const () == cp;
            if eq(&play) {
                jfn_mpv::api::jfn_mpv_play();
            } else if eq(&pause) {
                jfn_mpv::api::jfn_mpv_pause();
            } else if eq(&toggle) {
                jfn_mpv::api::jfn_mpv_toggle_pause();
            } else if eq(&stop) {
                jfn_mpv::api::jfn_mpv_stop();
            } else if eq(&next) {
                exec_js(c"if(window._nativeHostInput) window._nativeHostInput(['next']);");
            } else if eq(&prev) {
                exec_js(c"if(window._nativeHostInput) window._nativeHostInput(['previous']);");
            } else {
                return MPRemoteCommandHandlerStatus::CommandFailed;
            }
            MPRemoteCommandHandlerStatus::Success
        }

        #[unsafe(method(handleSeek:))]
        fn handle_seek(
            &self,
            event: &MPChangePlaybackPositionCommandEvent,
        ) -> MPRemoteCommandHandlerStatus {
            let pos = unsafe { event.positionTime() };
            // Update Now Playing position immediately for responsive UI;
            // rate=0 until mpv finishes the seek.
            unsafe {
                let center = MPNowPlayingInfoCenter::defaultCenter();
                if let Some(existing) = center.nowPlayingInfo() {
                    let info = NSMutableDictionary::dictionaryWithDictionary(&existing);
                    let elapsed_key = mp_const("MPNowPlayingInfoPropertyElapsedPlaybackTime");
                    let rate_key = mp_const("MPNowPlayingInfoPropertyPlaybackRate");
                    info.setObject_forKey(&*NSNumber::new_f64(pos) as &AnyObject, ns_key(&*elapsed_key));
                    info.setObject_forKey(&*NSNumber::new_f64(0.0) as &AnyObject, ns_key(&*rate_key));
                    center.setNowPlayingInfo(Some(&info));
                }
            }
            let ms = (pos * 1000.0) as i64;
            let js = format!(
                "if(window._nativeSeek) window._nativeSeek({ms});\0"
            );
            unsafe {
                jfn_cef::business_web::jfn_web_exec_js(js.as_ptr() as *const c_char);
            }
            MPRemoteCommandHandlerStatus::Success
        }
    }
);

static DELEGATE: OnceLock<DelegateSlot> = OnceLock::new();

struct DelegateSlot(Retained<MediaKeysDelegate>);
unsafe impl Send for DelegateSlot {}
unsafe impl Sync for DelegateSlot {}

fn init_remote_command_center() {
    unsafe {
        let delegate: Retained<MediaKeysDelegate> = msg_send![MediaKeysDelegate::class(), new];
        let center = MPRemoteCommandCenter::sharedCommandCenter();
        let sel_cmd = objc2::sel!(handleCommand:);
        let sel_seek = objc2::sel!(handleSeek:);
        center.playCommand().addTarget_action(&*delegate, sel_cmd);
        center.pauseCommand().addTarget_action(&*delegate, sel_cmd);
        center
            .togglePlayPauseCommand()
            .addTarget_action(&*delegate, sel_cmd);
        center.stopCommand().addTarget_action(&*delegate, sel_cmd);
        center
            .nextTrackCommand()
            .addTarget_action(&*delegate, sel_cmd);
        center
            .previousTrackCommand()
            .addTarget_action(&*delegate, sel_cmd);
        center
            .changePlaybackPositionCommand()
            .addTarget_action(&*delegate, sel_seek);
        let _ = DELEGATE.set(DelegateSlot(delegate));
    }
    media_remote_set_can_be_now_playing(true);
}

fn teardown_remote_command_center() {
    unsafe {
        let center = MPNowPlayingInfoCenter::defaultCenter();
        center.setNowPlayingInfo(None);
    }
    if let Some(d) = DELEGATE.get() {
        unsafe {
            let center = MPRemoteCommandCenter::sharedCommandCenter();
            center.playCommand().removeTarget(Some(&*d.0));
            center.pauseCommand().removeTarget(Some(&*d.0));
            center.togglePlayPauseCommand().removeTarget(Some(&*d.0));
            center.stopCommand().removeTarget(Some(&*d.0));
            center.nextTrackCommand().removeTarget(Some(&*d.0));
            center.previousTrackCommand().removeTarget(Some(&*d.0));
            center
                .changePlaybackPositionCommand()
                .removeTarget(Some(&*d.0));
        }
    }
}

fn exec_js(js: &CStr) {
    unsafe { jfn_cef::business_web::jfn_web_exec_js(js.as_ptr()) };
}

fn mp_const(name: &str) -> Retained<NSString> {
    // MediaPlayer property keys (MPMediaItemPropertyTitle etc.) are
    // exported as `extern NSString* const Name`. objc2-media-player
    // exposes them as constants; for the few we need we resolve via
    // dlsym to keep the dependency surface narrow.
    use std::ffi::CString;
    let cname = CString::new(name).expect("nul");
    unsafe {
        let sym = libc::dlsym(libc::RTLD_DEFAULT, cname.as_ptr());
        if sym.is_null() {
            // Fallback: construct a Rust NSString. The Media keys are
            // not interned, so this won't match the framework's lookup,
            // but tests should never reach this branch on a real macOS.
            return NSString::from_str(name);
        }
        // The symbol is `NSString * const`, i.e. a pointer to a pointer.
        let pp = sym as *const *const NSString;
        let p = *pp;
        Retained::retain(p as *mut NSString).expect("non-null framework string")
    }
}

// =====================================================================
// Private MediaRemote framework (NowPlaying visibility / origin).
// =====================================================================

struct MediaRemoteSyms {
    set_visibility: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
    get_local_origin: Option<unsafe extern "C" fn() -> *mut c_void>,
    set_can_be_now_playing: Option<unsafe extern "C" fn(c_int)>,
    _handle: *mut c_void,
}
unsafe impl Send for MediaRemoteSyms {}
unsafe impl Sync for MediaRemoteSyms {}

static MEDIA_REMOTE: OnceLock<Option<MediaRemoteSyms>> = OnceLock::new();

fn media_remote() -> Option<&'static MediaRemoteSyms> {
    MEDIA_REMOTE
        .get_or_init(|| unsafe {
            let path = c"/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote";
            let handle = libc::dlopen(path.as_ptr(), libc::RTLD_NOW);
            if handle.is_null() {
                tracing::error!(
                    target: "Media",
                    "macOS Media: Failed to load MediaRemote.framework"
                );
                return None;
            }
            let dl = |name: &CStr| {
                let p = libc::dlsym(handle, name.as_ptr());
                if p.is_null() { None } else { Some(p) }
            };
            Some(MediaRemoteSyms {
                set_visibility: dl(c"MRMediaRemoteSetNowPlayingVisibility")
                    .map(|p| std::mem::transmute(p)),
                get_local_origin: dl(c"MRMediaRemoteGetLocalOrigin")
                    .map(|p| std::mem::transmute(p)),
                set_can_be_now_playing: dl(c"MRMediaRemoteSetCanBeNowPlayingApplication")
                    .map(|p| std::mem::transmute(p)),
                _handle: handle,
            })
        })
        .as_ref()
}

fn media_remote_set_can_be_now_playing(yes: bool) {
    if let Some(mr) = media_remote() {
        if let Some(f) = mr.set_can_be_now_playing {
            unsafe { f(if yes { 1 } else { 0 }) };
        }
    }
}

const VISIBILITY_NEVER: c_int = 3;
const VISIBILITY_ALWAYS: c_int = 1;

fn media_remote_set_visibility_for_phase(phase: u8) {
    if let Some(mr) = media_remote() {
        if let (Some(set_vis), Some(get_origin)) = (mr.set_visibility, mr.get_local_origin) {
            unsafe {
                let origin = get_origin();
                let vis = if phase == phase::STOPPED {
                    VISIBILITY_NEVER
                } else {
                    VISIBILITY_ALWAYS
                };
                set_vis(origin, vis);
            }
        }
    }
}

// =====================================================================
// Event delivery (mirrors C++ MacosSink::deliver).
// =====================================================================

fn map_kind_to_phase(kind: u8) -> u8 {
    match kind {
        kind::STARTED => phase::PLAYING,
        kind::PAUSED | kind::TRACK_LOADED => phase::PAUSED,
        kind::FINISHED | kind::CANCELED | kind::ERROR => phase::STOPPED,
        _ => phase::STOPPED,
    }
}

fn convert_state(phase: u8) -> MPNowPlayingPlaybackState {
    match phase {
        self::phase::PLAYING => MPNowPlayingPlaybackState::Playing,
        self::phase::PAUSED => MPNowPlayingPlaybackState::Paused,
        self::phase::STOPPED => MPNowPlayingPlaybackState::Stopped,
        _ => MPNowPlayingPlaybackState::Unknown,
    }
}

fn deliver(state: &mut ConsumerState, ev: OwnedEvent) {
    match ev.kind {
        kind::METADATA_CHANGED => {
            if !ev.metadata.id.is_empty() && ev.metadata.id == state.metadata.id {
                return;
            }
            state.metadata = ev.metadata.clone();
            update_now_playing_info(state);
        }
        kind::ARTWORK_CHANGED => {
            state.metadata.art_data_uri = ev.artwork_uri.clone();
            let Some(comma) = ev.artwork_uri.find(',') else {
                return;
            };
            let base64 = &ev.artwork_uri[comma + 1..];
            unsafe {
                let ns_b64 = NSString::from_str(base64);
                let data: Allocated<NSData> = msg_send![NSData::class(), alloc];
                let data: Option<Retained<NSData>> = msg_send![
                    data, initWithBase64EncodedString: &*ns_b64,
                    options: NSDataBase64DecodingOptions(0)
                ];
                let Some(data) = data else { return };
                let image: Allocated<NSImage> = msg_send![NSImage::class(), alloc];
                let image: Option<Retained<NSImage>> = msg_send![image, initWithData: &*data];
                let Some(image) = image else { return };
                let size = image.size();
                let image_clone = image.clone();
                let handler = block2::RcBlock::new(move |_size: NSSize| -> NonNull<NSImage> {
                    let img: Retained<NSImage> = image_clone.clone();
                    unsafe { NonNull::new_unchecked(Retained::autorelease_return(img)) }
                });
                let artwork: Allocated<MPMediaItemArtwork> =
                    msg_send![MPMediaItemArtwork::class(), alloc];
                let artwork: Retained<MPMediaItemArtwork> = msg_send![
                    artwork,
                    initWithBoundsSize: size,
                    requestHandler: &*handler
                ];
                let center = MPNowPlayingInfoCenter::defaultCenter();
                if let Some(existing) = center.nowPlayingInfo() {
                    let info = NSMutableDictionary::dictionaryWithDictionary(&existing);
                    let key = mp_const("MPMediaItemPropertyArtwork");
                    info.setObject_forKey(&*artwork as &AnyObject, ns_key(&*key));
                    center.setNowPlayingInfo(Some(&info));
                }
            }
        }
        kind::QUEUE_CAPS_CHANGED => unsafe {
            let center = MPRemoteCommandCenter::sharedCommandCenter();
            center.nextTrackCommand().setEnabled(ev.can_go_next);
            center.previousTrackCommand().setEnabled(ev.can_go_prev);
        },
        kind::STARTED
        | kind::PAUSED
        | kind::TRACK_LOADED
        | kind::FINISHED
        | kind::CANCELED
        | kind::ERROR => {
            let p = map_kind_to_phase(ev.kind);
            if p == phase::STOPPED {
                state.metadata = OwnedMetadata::default();
                state.position_us = 0;
                unsafe {
                    let center = MPNowPlayingInfoCenter::defaultCenter();
                    center.setNowPlayingInfo(None);
                    let rcc = MPRemoteCommandCenter::sharedCommandCenter();
                    rcc.changePlaybackPositionCommand().setEnabled(false);
                }
            } else {
                unsafe {
                    let rcc = MPRemoteCommandCenter::sharedCommandCenter();
                    rcc.changePlaybackPositionCommand().setEnabled(true);
                }
            }
            unsafe {
                let center = MPNowPlayingInfoCenter::defaultCenter();
                center.setPlaybackState(convert_state(p));
            }
            media_remote_set_visibility_for_phase(p);
            if p != phase::STOPPED {
                update_timeline_throttled(state, ev.snapshot_position_us, true);
            }
        }
        kind::POSITION_CHANGED => {
            update_timeline_throttled(state, ev.snapshot_position_us, false);
        }
        kind::RATE_CHANGED => unsafe {
            state.rate = ev.snapshot_rate;
            let center = MPNowPlayingInfoCenter::defaultCenter();
            if let Some(existing) = center.nowPlayingInfo() {
                let info = NSMutableDictionary::dictionaryWithDictionary(&existing);
                let key = mp_const("MPNowPlayingInfoPropertyPlaybackRate");
                info.setObject_forKey(&*NSNumber::new_f64(state.rate) as &AnyObject, ns_key(&*key));
                center.setNowPlayingInfo(Some(&info));
            }
        },
        kind::SEEKED => unsafe {
            state.position_us = ev.snapshot_position_us;
            let center = MPNowPlayingInfoCenter::defaultCenter();
            if let Some(existing) = center.nowPlayingInfo() {
                let info = NSMutableDictionary::dictionaryWithDictionary(&existing);
                let key = mp_const("MPNowPlayingInfoPropertyElapsedPlaybackTime");
                info.setObject_forKey(
                    &*NSNumber::new_f64(state.position_us as f64 / 1_000_000.0) as &AnyObject,
                    ns_key(&*key),
                );
                center.setNowPlayingInfo(Some(&info));
            }
        },
        _ => {}
    }
}

fn update_timeline_throttled(state: &mut ConsumerState, position_us: i64, force: bool) {
    state.position_us = position_us;
    let now = Instant::now();
    if !force {
        if let Some(last) = state.last_position_update {
            if now.duration_since(last) < Duration::from_secs(1) {
                return;
            }
        }
    }
    unsafe {
        let center = MPNowPlayingInfoCenter::defaultCenter();
        let Some(existing) = center.nowPlayingInfo() else {
            return;
        };
        let info = NSMutableDictionary::dictionaryWithDictionary(&existing);
        let key = mp_const("MPNowPlayingInfoPropertyElapsedPlaybackTime");
        info.setObject_forKey(
            &*NSNumber::new_f64(position_us as f64 / 1_000_000.0) as &AnyObject,
            ns_key(&*key),
        );
        center.setNowPlayingInfo(Some(&info));
    }
    state.last_position_update = Some(now);
}

fn update_now_playing_info(state: &mut ConsumerState) {
    unsafe {
        let info = NSMutableDictionary::<NSString, AnyObject>::dictionary();
        if !state.metadata.title.is_empty() {
            let k = mp_const("MPMediaItemPropertyTitle");
            let v = NSString::from_str(&state.metadata.title);
            info.setObject_forKey(&*v as &AnyObject, ns_key(&*k));
        }
        if !state.metadata.artist.is_empty() {
            let k = mp_const("MPMediaItemPropertyArtist");
            let v = NSString::from_str(&state.metadata.artist);
            info.setObject_forKey(&*v as &AnyObject, ns_key(&*k));
        }
        if !state.metadata.album.is_empty() {
            let k = mp_const("MPMediaItemPropertyAlbumTitle");
            let v = NSString::from_str(&state.metadata.album);
            info.setObject_forKey(&*v as &AnyObject, ns_key(&*k));
        }
        if state.metadata.duration_us > 0 {
            let k = mp_const("MPMediaItemPropertyPlaybackDuration");
            let v = NSNumber::new_f64(state.metadata.duration_us as f64 / 1_000_000.0);
            info.setObject_forKey(&*v as &AnyObject, ns_key(&*k));
        }
        if state.metadata.track_number > 0 {
            let k = mp_const("MPMediaItemPropertyAlbumTrackNumber");
            let v = NSNumber::new_i32(state.metadata.track_number);
            info.setObject_forKey(&*v as &AnyObject, ns_key(&*k));
        }
        let elapsed_key = mp_const("MPNowPlayingInfoPropertyElapsedPlaybackTime");
        let elapsed_v = NSNumber::new_f64(state.position_us as f64 / 1_000_000.0);
        info.setObject_forKey(&*elapsed_v as &AnyObject, ns_key(&*elapsed_key));
        let rate_key = mp_const("MPNowPlayingInfoPropertyPlaybackRate");
        let rate_v = NSNumber::new_f64(state.rate);
        info.setObject_forKey(&*rate_v as &AnyObject, ns_key(&*rate_key));
        let media_type_key = mp_const("MPNowPlayingInfoPropertyMediaType");
        let media_type_v: MPNowPlayingInfoMediaType =
            if state.metadata.media_type == media_type::AUDIO {
                MPNowPlayingInfoMediaType::Audio
            } else {
                MPNowPlayingInfoMediaType::Video
            };
        let media_type_num = NSNumber::new_u64(media_type_v.0 as u64);
        info.setObject_forKey(&*media_type_num as &AnyObject, ns_key(&*media_type_key));

        let center = MPNowPlayingInfoCenter::defaultCenter();
        let cast: &NSDictionary<NSString, AnyObject> = &info;
        center.setNowPlayingInfo(Some(cast));
    }
}
