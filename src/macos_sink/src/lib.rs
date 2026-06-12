//! macOS Now Playing / MPRemoteCommandCenter sink. The shared
//! [`jfn_playback::sink_core`] harness owns the event queue and consumer
//! thread; this crate supplies a [`MacosSink`] whose `deliver` drives
//! MPNowPlayingInfoCenter. Inbound MPRemoteCommand callbacks dispatch via
//! [`sink_core::execute`] / [`sink_core::seek_to_ms`].
//!
//! All UI updates run on the consumer thread; MPNowPlayingInfoCenter
//! mutations are performed there.

#![cfg(target_os = "macos")]

use std::ffi::{CStr, c_int, c_void};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use jfn_playback::sink_core::{self, MediaCommand, Phase, QueuedSink, map_kind_to_phase};
use jfn_playback::{MediaMetadata, MediaType as PbMediaType, PlaybackEvent, PlaybackEventKind};
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

// =====================================================================
// Public start/stop entry points.
// =====================================================================

pub fn jfn_macos_sink_start() {
    sink_core::run_sink("macos-sink", MacosSink::default);
}

pub fn jfn_macos_sink_stop() {
    sink_core::stop();
}

// =====================================================================
// Sink state — lives on the consumer thread for the sink's lifetime.
// =====================================================================

#[derive(Default)]
struct MacosSink {
    metadata: MediaMetadata,
    position_us: i64,
    rate: f64,
    last_position_update: Option<Instant>,
}

impl QueuedSink for MacosSink {
    fn init(&mut self) {
        init_remote_command_center();
    }

    fn deliver(&mut self, ev: &PlaybackEvent) {
        deliver(self, ev);
    }

    fn teardown(&mut self) {
        teardown_remote_command_center();
    }
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
            let cmd = if eq(&play) {
                MediaCommand::Play
            } else if eq(&pause) {
                MediaCommand::Pause
            } else if eq(&toggle) {
                MediaCommand::PlayPause
            } else if eq(&stop) {
                MediaCommand::Stop
            } else if eq(&next) {
                MediaCommand::Next
            } else if eq(&prev) {
                MediaCommand::Previous
            } else {
                return MPRemoteCommandHandlerStatus::CommandFailed;
            };
            sink_core::execute(cmd);
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
            sink_core::seek_to_ms((pos * 1000.0) as i64);
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

fn mp_const(name: &str) -> Retained<NSString> {
    // MediaPlayer property keys (MPMediaItemPropertyTitle etc.) are
    // exported as `extern NSString* const Name`. objc2-media-player
    // exposes them as constants; for the few we need we resolve via
    // dlsym to keep the dependency surface narrow.
    use std::ffi::CString;
    let Ok(cname) = CString::new(name) else {
        return NSString::from_str(name);
    };
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
        match Retained::retain(p as *mut NSString) {
            Some(s) => s,
            None => NSString::from_str(name),
        }
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

fn convert_state(phase: Phase) -> MPNowPlayingPlaybackState {
    match phase {
        Phase::Playing => MPNowPlayingPlaybackState::Playing,
        Phase::Paused => MPNowPlayingPlaybackState::Paused,
        Phase::Stopped => MPNowPlayingPlaybackState::Stopped,
    }
}

fn deliver(state: &mut MacosSink, ev: &PlaybackEvent) {
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
                state.metadata = MediaMetadata::default();
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
                update_timeline_throttled(state, ev.snapshot.position_us, true);
            }
        }
        PlaybackEventKind::PositionChanged => {
            update_timeline_throttled(state, ev.snapshot.position_us, false);
        }
        PlaybackEventKind::RateChanged => unsafe {
            state.rate = ev.snapshot.rate;
            let center = MPNowPlayingInfoCenter::defaultCenter();
            if let Some(existing) = center.nowPlayingInfo() {
                let info = NSMutableDictionary::dictionaryWithDictionary(&existing);
                let key = mp_const("MPNowPlayingInfoPropertyPlaybackRate");
                info.setObject_forKey(&*NSNumber::new_f64(state.rate) as &AnyObject, ns_key(&key));
                center.setNowPlayingInfo(Some(&info));
            }
        },
        PlaybackEventKind::Seeked => unsafe {
            state.position_us = ev.snapshot.position_us;
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

fn update_timeline_throttled(state: &mut MacosSink, position_us: i64, force: bool) {
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

fn update_now_playing_info(state: &mut MacosSink) {
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
