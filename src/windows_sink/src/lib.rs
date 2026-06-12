//! Windows SystemMediaTransportControls (SMTC) sink. The shared
//! [`jfn_playback::sink_core`] harness owns the event queue and consumer
//! thread (MTA-initialised); this crate supplies a [`WindowsSink`] whose
//! `deliver` drives SMTC. ButtonPressed / PlaybackPositionChangeRequested
//! callbacks dispatch via [`sink_core::execute`] / [`sink_core::seek_to_ms`].
//!
//! Public entry points:
//!   * `jfn_windows_sink_start()` / `jfn_windows_sink_start_for(hwnd)` —
//!     start the sink (SMTC binds to the mpv window via GetForWindow).
//!   * `jfn_windows_sink_stop()` — signal the thread to exit at next wake.

#![cfg(target_os = "windows")]

use std::time::Instant;

use jfn_playback::sink_core::{self, MediaCommand, Phase, QueuedSink, map_kind_to_phase};
use jfn_playback::{MediaMetadata, MediaType as PbMediaType, PlaybackEvent, PlaybackEventKind};
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
use windows::Win32::Security::Cryptography::{CRYPT_STRING_BASE64, CryptStringToBinaryA};
use windows::Win32::System::WinRT::{ISystemMediaTransportControlsInterop, RoGetActivationFactory};
use windows::core::HSTRING;

// =====================================================================
// Public start/stop entry points.
// =====================================================================

/// Start the sink. `hwnd_raw` is the HWND of the mpv window — required
/// to bind SMTC via ISystemMediaTransportControlsInterop::GetForWindow.
pub fn jfn_windows_sink_start_for(hwnd_raw: isize) {
    sink_core::run_sink("windows-sink", move || WindowsSink::new(hwnd_raw));
}

/// Convenience entry: queries mpv for the window-id and starts the sink.
pub fn jfn_windows_sink_start() {
    let mut wid: i64 = 0;
    let name = c"window-id";
    let rc = unsafe { jfn_mpv::api::jfn_mpv_get_property_int(name.as_ptr(), &mut wid) };
    if rc < 0 || wid == 0 {
        tracing::error!(target: "Media", "[SMTC] mpv window-id lookup failed");
        return;
    }
    jfn_windows_sink_start_for(wid as isize);
}

pub fn jfn_windows_sink_stop() {
    sink_core::stop();
}

// =====================================================================
// Sink state — lives on the consumer thread for the sink's lifetime.
// =====================================================================

#[derive(Default)]
struct WinState {
    metadata: MediaMetadata,
    phase: Option<Phase>,
    position_us: i64,
    last_position_update: Option<Instant>,
    pending_update: bool,
}

struct WindowsSink {
    hwnd_raw: isize,
    smtc: Option<Smtc>,
    state: WinState,
}

impl WindowsSink {
    fn new(hwnd_raw: isize) -> Self {
        Self {
            hwnd_raw,
            smtc: None,
            state: WinState::default(),
        }
    }
}

impl QueuedSink for WindowsSink {
    fn init(&mut self) {
        self.smtc = init_smtc(self.hwnd_raw);
    }

    fn deliver(&mut self, ev: &PlaybackEvent) {
        deliver(&mut self.state, &mut self.smtc, ev);
    }

    fn teardown(&mut self) {
        if let Some(s) = self.smtc.take() {
            teardown_smtc(s);
        }
    }
}

// =====================================================================
// SMTC bindings.
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
        let interop: ISystemMediaTransportControlsInterop =
            RoGetActivationFactory(&HSTRING::from("Windows.Media.SystemMediaTransportControls"))
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
                if let Some(args) = args.as_ref()
                    && let Ok(button) = args.Button()
                {
                    on_button_pressed(button);
                }
                Ok(())
            },
        );
        smtc.ButtonPressed(&handler).ok()?
    };

    let seek_token = {
        let handler =
            windows::Foundation::TypedEventHandler::new(
                |_sender: windows_core::Ref<SystemMediaTransportControls>,
                 args: windows_core::Ref<
                    windows::Media::PlaybackPositionChangeRequestedEventArgs,
                >| {
                    if let Some(args) = args.as_ref()
                        && let Ok(span) = args.RequestedPlaybackPosition()
                    {
                        // TimeSpan.Duration is 100-ns ticks.
                        let pos_us = span.Duration / 10;
                        sink_core::seek_to_ms(pos_us / 1000);
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

fn on_button_pressed(button: SystemMediaTransportControlsButton) {
    use SystemMediaTransportControlsButton as B;
    let cmd = match button {
        B::Play => MediaCommand::Play,
        B::Pause => MediaCommand::Pause,
        B::Stop => MediaCommand::Stop,
        B::Next => MediaCommand::Next,
        B::Previous => MediaCommand::Previous,
        _ => return,
    };
    sink_core::execute(cmd);
}

fn deliver(state: &mut WinState, smtc: &mut Option<Smtc>, ev: &PlaybackEvent) {
    match ev.kind {
        PlaybackEventKind::MetadataChanged => {
            if !ev.metadata.id.is_empty() && ev.metadata.id == state.metadata.id {
                return;
            }
            // Display refresh happens on the next phase transition
            // (Started/Paused), which reads this cached metadata.
            state.metadata = ev.metadata.clone();
        }
        PlaybackEventKind::ArtworkChanged => {
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
        PlaybackEventKind::QueueCapsChanged => {
            if let Some(s) = smtc.as_ref() {
                let _ = s.smtc.SetIsNextEnabled(ev.can_go_next);
                let _ = s.smtc.SetIsPreviousEnabled(ev.can_go_prev);
            }
        }
        PlaybackEventKind::Started
        | PlaybackEventKind::Paused
        | PlaybackEventKind::TrackLoaded
        | PlaybackEventKind::Finished
        | PlaybackEventKind::Canceled
        | PlaybackEventKind::Error => {
            let p = map_kind_to_phase(ev.kind);
            state.phase = Some(p);
            let Some(s) = smtc.as_mut() else { return };
            match p {
                Phase::Playing => {
                    let _ = s.smtc.SetPlaybackStatus(MediaPlaybackStatus::Playing);
                    update_display_properties(state, s);
                }
                Phase::Paused => {
                    let _ = s.smtc.SetPlaybackStatus(MediaPlaybackStatus::Paused);
                    update_timeline(state, s);
                }
                Phase::Stopped => {
                    state.metadata = MediaMetadata::default();
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
        PlaybackEventKind::PositionChanged => {
            state.position_us = ev.snapshot.position_us;
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
        PlaybackEventKind::Seeked => {
            state.position_us = ev.snapshot.position_us;
            if let Some(s) = smtc.as_mut() {
                update_timeline(state, s);
            }
            state.last_position_update = Some(Instant::now());
            state.pending_update = false;
        }
        _ => {}
    }
}

fn update_display_properties(state: &mut WinState, s: &mut Smtc) {
    if state.phase == Some(Phase::Stopped) {
        return;
    }
    let _ = s.updater.ClearAll();
    if state.metadata.media_type == PbMediaType::Audio {
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

fn update_timeline(state: &WinState, s: &Smtc) {
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
        let ok = CryptStringToBinaryA(bytes, CRYPT_STRING_BASE64, None, &mut needed, None, None);
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
