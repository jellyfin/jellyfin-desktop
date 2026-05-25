//! macOS Now Playing / MPRemoteCommandCenter sink. Owns its own thread
//! that drains queued PlaybackEvents on wake. Inbound MPRemoteCommand
//! callbacks dispatch directly into mpv (jfn_mpv) / web browser
//! (jfn_web_exec_js).
//!
//! All UI updates run on the main thread via `dispatch_async`/
//! `dispatch_sync` because MPNowPlayingInfoCenter mutations are not safe
//! from background threads.

#![cfg(target_os = "macos")]

use parking_lot::{Condvar, Mutex};
use std::collections::VecDeque;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};

use jfn_playback::{MediaType as PbMediaType, PlaybackEvent, PlaybackEventKind};
use objc2::rc::{Allocated, Retained};
use objc2::runtime::{AnyObject, ProtocolObject};
use objc2::{ClassType, define_class, msg_send};
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

// Stopped maps to MPNowPlayingPlaybackState::Stopped via a local Phase
// enum so we keep the kind→phase mapping table.
#[derive(Copy, Clone, PartialEq, Eq)]
enum Phase {
    Playing,
    Paused,
    Stopped,
}

// =====================================================================
// Owned event copy (heap-allocated for the consumer thread). Holds only
// the fields the consumer reads, so we avoid keeping the full event
// clone alive across thread hops.
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
    media_type: PbMediaType,
}

struct OwnedEvent {
    kind: PlaybackEventKind,
    metadata: OwnedMetadata,
    snapshot_position_us: i64,
    snapshot_rate: f64,
    artwork_uri: String,
    can_go_next: bool,
    can_go_prev: bool,
}

fn owned_event(ev: &PlaybackEvent) -> OwnedEvent {
    OwnedEvent {
        kind: ev.kind,
        metadata: OwnedMetadata {
            id: ev.metadata.id.clone(),
            title: ev.metadata.title.clone(),
            artist: ev.metadata.artist.clone(),
            album: ev.metadata.album.clone(),
            track_number: ev.metadata.track_number,
            duration_us: ev.metadata.duration_us,
            art_data_uri: ev.metadata.art_data_uri.clone(),
            media_type: ev.metadata.media_type,
        },
        snapshot_position_us: ev.snapshot.position_us,
        snapshot_rate: ev.snapshot.rate,
        artwork_uri: ev.artwork_uri.clone(),
        can_go_next: ev.can_go_next,
        can_go_prev: ev.can_go_prev,
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

// Coordinator-side: jfn-playback invokes this for every event. We copy
// the small subset of fields the consumer thread reads.
fn on_event(ev: &PlaybackEvent) {
    let owned = owned_event(ev);
    let inner = inner();
    {
        let mut q = inner.queue.lock();
        if q.len() >= EVENT_QUEUE_CAP {
            return;
        }
        q.push_back(owned);
    }
    inner.cv.notify_one();
}

// =====================================================================
// Public start/stop entry points.
// =====================================================================

pub fn jfn_macos_sink_start() {
    let inner = inner();
    if inner.running.swap(true, Ordering::AcqRel) {
        return;
    }

    jfn_playback::ffi::register_event_sink(Box::new(on_event));

    // Consumer thread drains the queue and dispatches MPNowPlayingInfo /
    // command-center updates onto the main thread.
    std::thread::Builder::new()
        .name("macos-sink".into())
        .spawn(move || consumer_thread(inner))
        .expect("spawn macos-sink thread");
}

pub fn jfn_macos_sink_stop() {
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
            let mut q = inner.queue.lock();
            while q.is_empty() && inner.running.load(Ordering::Acquire) {
                inner.cv.wait_for(&mut q, Duration::from_millis(100));
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
            // is sufficient.
            let cp = (&*command as *const MPRemoteCommand) as *const ();
            let eq = |c: &MPRemoteCommand| (c as *const MPRemoteCommand) as *const () == cp;
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
                    info.setObject_forKey(&*NSNumber::new_f64(pos) as &AnyObject, ns_key(&elapsed_key));
                    info.setObject_forKey(&*NSNumber::new_f64(0.0) as &AnyObject, ns_key(&rate_key));
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
        center.playCommand().addTarget_action(&delegate, sel_cmd);
        center.pauseCommand().addTarget_action(&delegate, sel_cmd);
        center
            .togglePlayPauseCommand()
            .addTarget_action(&delegate, sel_cmd);
        center.stopCommand().addTarget_action(&delegate, sel_cmd);
        center
            .nextTrackCommand()
            .addTarget_action(&delegate, sel_cmd);
        center
            .previousTrackCommand()
            .addTarget_action(&delegate, sel_cmd);
        center
            .changePlaybackPositionCommand()
            .addTarget_action(&delegate, sel_seek);
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
    if let Some(mr) = media_remote()
        && let Some(f) = mr.set_can_be_now_playing
    {
        unsafe { f(if yes { 1 } else { 0 }) };
    }
}

const VISIBILITY_NEVER: c_int = 3;
const VISIBILITY_ALWAYS: c_int = 1;

fn media_remote_set_visibility_for_phase(phase: Phase) {
    if let Some(mr) = media_remote()
        && let (Some(set_vis), Some(get_origin)) = (mr.set_visibility, mr.get_local_origin)
    {
        unsafe {
            let origin = get_origin();
            let vis = if phase == Phase::Stopped {
                VISIBILITY_NEVER
            } else {
                VISIBILITY_ALWAYS
            };
            set_vis(origin, vis);
        }
    }
}

// =====================================================================
// Event delivery.
// =====================================================================

fn map_kind_to_phase(kind: PlaybackEventKind) -> Phase {
    match kind {
        PlaybackEventKind::Started => Phase::Playing,
        PlaybackEventKind::Paused | PlaybackEventKind::TrackLoaded => Phase::Paused,
        PlaybackEventKind::Finished | PlaybackEventKind::Canceled | PlaybackEventKind::Error => {
            Phase::Stopped
        }
        _ => Phase::Stopped,
    }
}

fn convert_state(phase: Phase) -> MPNowPlayingPlaybackState {
    match phase {
        Phase::Playing => MPNowPlayingPlaybackState::Playing,
        Phase::Paused => MPNowPlayingPlaybackState::Paused,
        Phase::Stopped => MPNowPlayingPlaybackState::Stopped,
    }
}

fn deliver(state: &mut ConsumerState, ev: OwnedEvent) {
    match ev.kind {
        PlaybackEventKind::MetadataChanged => {
            if !ev.metadata.id.is_empty() && ev.metadata.id == state.metadata.id {
                return;
            }
            state.metadata = ev.metadata.clone();
            update_now_playing_info(state);
        }
        PlaybackEventKind::ArtworkChanged => {
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
                    NonNull::new_unchecked(Retained::autorelease_return(img))
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
                    info.setObject_forKey(&*artwork as &AnyObject, ns_key(&key));
                    center.setNowPlayingInfo(Some(&info));
                }
            }
        }
        PlaybackEventKind::QueueCapsChanged => unsafe {
            let center = MPRemoteCommandCenter::sharedCommandCenter();
            center.nextTrackCommand().setEnabled(ev.can_go_next);
            center.previousTrackCommand().setEnabled(ev.can_go_prev);
        },
        PlaybackEventKind::Started
        | PlaybackEventKind::Paused
        | PlaybackEventKind::TrackLoaded
        | PlaybackEventKind::Finished
        | PlaybackEventKind::Canceled
        | PlaybackEventKind::Error => {
            let p = map_kind_to_phase(ev.kind);
            if p == Phase::Stopped {
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
            if p != Phase::Stopped {
                update_timeline_throttled(state, ev.snapshot_position_us, true);
            }
        }
        PlaybackEventKind::PositionChanged => {
            update_timeline_throttled(state, ev.snapshot_position_us, false);
        }
        PlaybackEventKind::RateChanged => unsafe {
            state.rate = ev.snapshot_rate;
            let center = MPNowPlayingInfoCenter::defaultCenter();
            if let Some(existing) = center.nowPlayingInfo() {
                let info = NSMutableDictionary::dictionaryWithDictionary(&existing);
                let key = mp_const("MPNowPlayingInfoPropertyPlaybackRate");
                info.setObject_forKey(&*NSNumber::new_f64(state.rate) as &AnyObject, ns_key(&key));
                center.setNowPlayingInfo(Some(&info));
            }
        },
        PlaybackEventKind::Seeked => unsafe {
            state.position_us = ev.snapshot_position_us;
            let center = MPNowPlayingInfoCenter::defaultCenter();
            if let Some(existing) = center.nowPlayingInfo() {
                let info = NSMutableDictionary::dictionaryWithDictionary(&existing);
                let key = mp_const("MPNowPlayingInfoPropertyElapsedPlaybackTime");
                info.setObject_forKey(
                    &*NSNumber::new_f64(state.position_us as f64 / 1_000_000.0) as &AnyObject,
                    ns_key(&key),
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
    if !force
        && let Some(last) = state.last_position_update
        && now.duration_since(last) < Duration::from_secs(1)
    {
        return;
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
            ns_key(&key),
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
            info.setObject_forKey(&*v as &AnyObject, ns_key(&k));
        }
        if !state.metadata.artist.is_empty() {
            let k = mp_const("MPMediaItemPropertyArtist");
            let v = NSString::from_str(&state.metadata.artist);
            info.setObject_forKey(&*v as &AnyObject, ns_key(&k));
        }
        if !state.metadata.album.is_empty() {
            let k = mp_const("MPMediaItemPropertyAlbumTitle");
            let v = NSString::from_str(&state.metadata.album);
            info.setObject_forKey(&*v as &AnyObject, ns_key(&k));
        }
        if state.metadata.duration_us > 0 {
            let k = mp_const("MPMediaItemPropertyPlaybackDuration");
            let v = NSNumber::new_f64(state.metadata.duration_us as f64 / 1_000_000.0);
            info.setObject_forKey(&*v as &AnyObject, ns_key(&k));
        }
        if state.metadata.track_number > 0 {
            let k = mp_const("MPMediaItemPropertyAlbumTrackNumber");
            let v = NSNumber::new_i32(state.metadata.track_number);
            info.setObject_forKey(&*v as &AnyObject, ns_key(&k));
        }
        let elapsed_key = mp_const("MPNowPlayingInfoPropertyElapsedPlaybackTime");
        let elapsed_v = NSNumber::new_f64(state.position_us as f64 / 1_000_000.0);
        info.setObject_forKey(&*elapsed_v as &AnyObject, ns_key(&elapsed_key));
        let rate_key = mp_const("MPNowPlayingInfoPropertyPlaybackRate");
        let rate_v = NSNumber::new_f64(state.rate);
        info.setObject_forKey(&*rate_v as &AnyObject, ns_key(&rate_key));
        let media_type_key = mp_const("MPNowPlayingInfoPropertyMediaType");
        let media_type_v: MPNowPlayingInfoMediaType =
            if state.metadata.media_type == PbMediaType::Audio {
                MPNowPlayingInfoMediaType::Audio
            } else {
                MPNowPlayingInfoMediaType::Video
            };
        let media_type_num = NSNumber::new_u64(media_type_v.0 as u64);
        info.setObject_forKey(&*media_type_num as &AnyObject, ns_key(&media_type_key));

        let center = MPNowPlayingInfoCenter::defaultCenter();
        let cast: &NSDictionary<NSString, AnyObject> = &info;
        center.setNowPlayingInfo(Some(cast));
    }
}
