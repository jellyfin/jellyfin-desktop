//! MPRIS direct sink. Owns its own thread that runs a zbus blocking
//! Connection. Receives PlaybackEvents through a channel, updates the
//! content/view state, and emits PropertiesChanged + Seeked signals.
//!
//! Method/property handlers run inline on zbus's reactor thread; outbound
//! transport (Play/Pause/Stop/etc.) calls jfn_mpv_* directly. Next/
//! Previous/Seek/SetPosition route to the JS UI via the registered exec_js
//! callback.

use parking_lot::Mutex;
use std::collections::HashMap;
use std::ffi::c_char;
use std::sync::mpsc::{Receiver, Sender, channel};
use std::sync::{Arc, OnceLock};
use std::thread::{self, JoinHandle};

use zbus::blocking::Connection;
use zbus::interface;
use zbus::names::InterfaceName;
use zbus::zvariant::{ObjectPath, OwnedValue, Value};

use crate::mpris;
use crate::types::{MediaMetadata, PlaybackEvent, PlaybackEventKind, PlaybackSnapshot};

const MPRIS_PATH: &str = "/org/mpris/MediaPlayer2";
const BASE_SERVICE_NAME: &str = "org.mpris.MediaPlayer2.JellyfinDesktop";

// ============================================================================
// Content + projected view
// ============================================================================

#[derive(Clone, Debug, Default)]
struct Content {
    metadata: MediaMetadata,
    pending_rate: f64,
    volume: f64,
    can_go_next: bool,
    can_go_previous: bool,
}

impl Content {
    fn fresh() -> Self {
        Self {
            metadata: MediaMetadata::default(),
            pending_rate: 1.0,
            volume: 1.0,
            can_go_next: false,
            can_go_previous: false,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
struct View {
    playback_status: &'static str,
    can_play: bool,
    can_pause: bool,
    can_seek: bool,
    can_control: bool,
    metadata: MediaMetadata,
    rate: f64,
    volume: f64,
    can_go_next: bool,
    can_go_previous: bool,
}

impl Default for View {
    fn default() -> Self {
        Self {
            playback_status: "Stopped",
            can_play: false,
            can_pause: false,
            can_seek: false,
            can_control: false,
            metadata: MediaMetadata::default(),
            rate: 1.0,
            volume: 1.0,
            can_go_next: false,
            can_go_previous: false,
        }
    }
}

fn status_name(s: mpris::MprisStatus) -> &'static str {
    match s {
        mpris::MprisStatus::Playing => "Playing",
        mpris::MprisStatus::Paused => "Paused",
        mpris::MprisStatus::Stopped => "Stopped",
    }
}

fn project_view(snap: &PlaybackSnapshot, content: &Content) -> View {
    let d = mpris::project(&mpris::ProjectInput {
        phase: snap.phase,
        seeking: snap.seeking,
        buffering: snap.buffering,
        metadata_duration_us: content.metadata.duration_us,
        pending_rate: content.pending_rate,
    });
    View {
        playback_status: status_name(d.status),
        can_play: d.can_play,
        can_pause: d.can_pause,
        can_seek: d.can_seek,
        can_control: d.can_control,
        // metadata_active=false -> clean transport while nothing is loaded
        metadata: if d.metadata_active {
            content.metadata.clone()
        } else {
            MediaMetadata::default()
        },
        rate: d.rate,
        volume: content.volume,
        can_go_next: content.can_go_next,
        can_go_previous: content.can_go_previous,
    }
}

/// Returns the MPRIS property names that differ between two views.
fn diff_view(prev: &View, next: &View) -> Vec<&'static str> {
    let mut out = Vec::new();
    if prev.playback_status != next.playback_status {
        out.push("PlaybackStatus");
    }
    if prev.can_play != next.can_play {
        out.push("CanPlay");
    }
    if prev.can_pause != next.can_pause {
        out.push("CanPause");
    }
    if prev.can_seek != next.can_seek {
        out.push("CanSeek");
    }
    if prev.can_control != next.can_control {
        out.push("CanControl");
    }
    if prev.metadata != next.metadata {
        out.push("Metadata");
    }
    if prev.rate != next.rate {
        out.push("Rate");
    }
    if prev.volume != next.volume {
        out.push("Volume");
    }
    if prev.can_go_next != next.can_go_next {
        out.push("CanGoNext");
    }
    if prev.can_go_previous != next.can_go_previous {
        out.push("CanGoPrevious");
    }
    out
}

fn metadata_to_dict(meta: &MediaMetadata) -> HashMap<String, OwnedValue> {
    let mut m = HashMap::new();
    // mpris:trackid is required by spec.
    let track_id = ObjectPath::try_from("/org/jellyfin/track/1").unwrap();
    m.insert(
        "mpris:trackid".into(),
        OwnedValue::try_from(Value::from(track_id)).unwrap(),
    );
    if meta.duration_us > 0 {
        m.insert(
            "mpris:length".into(),
            OwnedValue::try_from(Value::from(meta.duration_us)).unwrap(),
        );
    }
    if !meta.title.is_empty() {
        m.insert(
            "xesam:title".into(),
            OwnedValue::try_from(Value::from(meta.title.as_str())).unwrap(),
        );
    }
    if !meta.artist.is_empty() {
        m.insert(
            "xesam:artist".into(),
            OwnedValue::try_from(Value::from(vec![meta.artist.as_str()])).unwrap(),
        );
    }
    if !meta.album.is_empty() {
        m.insert(
            "xesam:album".into(),
            OwnedValue::try_from(Value::from(meta.album.as_str())).unwrap(),
        );
    }
    if meta.track_number > 0 {
        m.insert(
            "xesam:trackNumber".into(),
            OwnedValue::try_from(Value::from(meta.track_number)).unwrap(),
        );
    }
    if !meta.art_data_uri.is_empty() {
        m.insert(
            "mpris:artUrl".into(),
            OwnedValue::try_from(Value::from(meta.art_data_uri.as_str())).unwrap(),
        );
    }
    m
}

// ============================================================================
// Shared state — accessed by zbus reactor thread (interface impls) and the
// event-pump thread (worker). Single Mutex; getters are read-only fast paths.
// ============================================================================

struct State {
    content: Content,
    view: View,
    snapshot: PlaybackSnapshot,
}

impl State {
    fn fresh() -> Self {
        Self {
            content: Content::fresh(),
            view: View::default(),
            snapshot: PlaybackSnapshot::default(),
        }
    }
}

use crate::exec_js::call as call_exec_js;

// ============================================================================
// D-Bus interface impls
// ============================================================================

struct Root;

#[interface(name = "org.mpris.MediaPlayer2")]
impl Root {
    fn raise(&self) {}
    fn quit(&self) {}

    #[zbus(property)]
    fn identity(&self) -> &str {
        "Jellyfin Desktop"
    }
    #[zbus(property)]
    fn can_quit(&self) -> bool {
        false
    }
    #[zbus(property)]
    fn can_raise(&self) -> bool {
        true
    }
    #[zbus(property)]
    fn can_set_fullscreen(&self) -> bool {
        true
    }
    #[zbus(property)]
    fn fullscreen(&self) -> bool {
        false
    }
    #[zbus(property)]
    fn has_track_list(&self) -> bool {
        false
    }
    #[zbus(property)]
    fn supported_uri_schemes(&self) -> Vec<String> {
        Vec::new()
    }
    #[zbus(property)]
    fn supported_mime_types(&self) -> Vec<String> {
        Vec::new()
    }
}

struct Player {
    state: Arc<Mutex<State>>,
}

#[interface(name = "org.mpris.MediaPlayer2.Player")]
impl Player {
    fn play(&self) {
        jfn_mpv::api::jfn_mpv_play()
    }
    fn pause(&self) {
        jfn_mpv::api::jfn_mpv_pause()
    }
    fn play_pause(&self) {
        jfn_mpv::api::jfn_mpv_toggle_pause()
    }
    fn stop(&self) {
        jfn_mpv::api::jfn_mpv_stop()
    }
    fn next(&self) {
        call_exec_js("if(window._nativeHostInput) window._nativeHostInput(['next']);");
    }
    fn previous(&self) {
        call_exec_js("if(window._nativeHostInput) window._nativeHostInput(['previous']);");
    }
    fn seek(&self, offset: i64) {
        let cur = self.state.lock().snapshot.position_us;
        let new_pos = (cur + offset).max(0);
        let ms = new_pos / 1000;
        call_exec_js(&format!(
            "if(window._nativeSeek) window._nativeSeek({});",
            ms
        ));
    }
    fn set_position(&self, _track: ObjectPath<'_>, position_us: i64) {
        let ms = position_us / 1000;
        call_exec_js(&format!(
            "if(window._nativeSeek) window._nativeSeek({});",
            ms
        ));
    }

    #[zbus(property)]
    fn playback_status(&self) -> String {
        self.state.lock().view.playback_status.to_string()
    }
    #[zbus(property)]
    fn rate(&self) -> f64 {
        self.state.lock().view.rate
    }
    #[zbus(property)]
    fn set_rate(&self, value: f64) {
        let clamped = value.clamp(0.25, 2.0);
        jfn_mpv::api::jfn_mpv_set_speed(clamped);
    }
    #[zbus(property)]
    fn minimum_rate(&self) -> f64 {
        0.25
    }
    #[zbus(property)]
    fn maximum_rate(&self) -> f64 {
        2.0
    }
    #[zbus(property)]
    fn metadata(&self) -> HashMap<String, OwnedValue> {
        let s = self.state.lock();
        metadata_to_dict(&s.view.metadata)
    }
    #[zbus(property)]
    fn volume(&self) -> f64 {
        self.state.lock().view.volume
    }
    #[zbus(property)]
    fn position(&self) -> i64 {
        self.state.lock().snapshot.position_us
    }
    #[zbus(property)]
    fn can_go_next(&self) -> bool {
        self.state.lock().view.can_go_next
    }
    #[zbus(property)]
    fn can_go_previous(&self) -> bool {
        self.state.lock().view.can_go_previous
    }
    #[zbus(property)]
    fn can_play(&self) -> bool {
        self.state.lock().view.can_play
    }
    #[zbus(property)]
    fn can_pause(&self) -> bool {
        self.state.lock().view.can_pause
    }
    #[zbus(property)]
    fn can_seek(&self) -> bool {
        self.state.lock().view.can_seek
    }
    #[zbus(property)]
    fn can_control(&self) -> bool {
        self.state.lock().view.can_control
    }
}

// ============================================================================
// Worker thread + global sink registry
// ============================================================================

struct Sink {
    tx: Sender<Msg>,
    join: Option<JoinHandle<()>>,
}

enum Msg {
    Event(Box<PlaybackEvent>),
    Stop,
}

fn sink_slot() -> &'static Mutex<Option<Sink>> {
    static SLOT: OnceLock<Mutex<Option<Sink>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

/// Push a PlaybackEvent into the running MPRIS sink. No-op if not started.
/// Called by the playback coordinator's builtin event-sink closure.
pub(crate) fn deliver(ev: PlaybackEvent) {
    if let Some(s) = sink_slot().lock().as_ref() {
        let _ = s.tx.send(Msg::Event(Box::new(ev)));
    }
}

fn worker(rx: Receiver<Msg>, service_suffix: String) {
    let service_name = format!("{}{}", BASE_SERVICE_NAME, service_suffix);

    let conn = match Connection::session() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("mpris: session bus connect failed: {}", e);
            return;
        }
    };

    if let Err(e) = conn.object_server().at(MPRIS_PATH, Root) {
        eprintln!("mpris: register root iface: {}", e);
        return;
    }

    let state = Arc::new(Mutex::new(State::fresh()));
    let player = Player {
        state: Arc::clone(&state),
    };
    if let Err(e) = conn.object_server().at(MPRIS_PATH, player) {
        eprintln!("mpris: register player iface: {}", e);
        return;
    }

    if let Err(e) = conn.request_name(service_name.as_str()) {
        eprintln!("mpris: request name {}: {}", service_name, e);
        return;
    }
    eprintln!("mpris: registered as {}", service_name);

    while let Ok(msg) = rx.recv() {
        match msg {
            Msg::Stop => break,
            Msg::Event(ev) => handle_event(*ev, &state, &conn),
        }
    }

    let _ = conn.release_name(service_name.as_str());
}

fn handle_event(ev: PlaybackEvent, state: &Arc<Mutex<State>>, conn: &Connection) {
    let snap = ev.snapshot.clone();

    // last_snap_ tracks every snapshot so getPosition() reads the latest.
    state.lock().snapshot = snap.clone();

    let mut do_recompute = false;
    let mut emit_seeked = false;
    {
        let mut s = state.lock();
        match ev.kind {
            PlaybackEventKind::MetadataChanged => {
                // Same-Id dedup: same-Id setMetadata is a semantic no-op
                // (identical item). Otherwise empty art fields in the
                // incoming meta would clobber cached art from notifyArtwork
                // on every variant switch.
                if ev.metadata.id.is_empty() || ev.metadata.id != s.content.metadata.id {
                    s.content.metadata = ev.metadata.clone();
                    do_recompute = true;
                }
            }
            PlaybackEventKind::ArtworkChanged => {
                s.content.metadata.art_data_uri = ev.artwork_uri.clone();
                do_recompute = true;
            }
            PlaybackEventKind::QueueCapsChanged => {
                s.content.can_go_next = ev.can_go_next;
                s.content.can_go_previous = ev.can_go_prev;
                do_recompute = true;
            }
            PlaybackEventKind::Started => {
                do_recompute = true;
                emit_seeked = true;
            }
            PlaybackEventKind::Seeked => {
                emit_seeked = true;
            }
            PlaybackEventKind::Paused
            | PlaybackEventKind::Finished
            | PlaybackEventKind::Canceled
            | PlaybackEventKind::Error
            | PlaybackEventKind::SeekingChanged
            | PlaybackEventKind::BufferingChanged
            | PlaybackEventKind::TrackLoaded
            | PlaybackEventKind::RateChanged => {
                do_recompute = true;
            }
            // MPRIS Position is polled, not signaled. Snapshot already
            // refreshed above so the property getter returns latest value.
            PlaybackEventKind::PositionChanged => {}
            // Duration ships inside metadata; bare DurationChanged from mpv
            // isn't surfaced to MPRIS.
            PlaybackEventKind::DurationChanged => {}
            PlaybackEventKind::MediaTypeChanged
            | PlaybackEventKind::FullscreenChanged
            | PlaybackEventKind::OsdDimsChanged
            | PlaybackEventKind::BufferedRangesChanged
            | PlaybackEventKind::DisplayHzChanged => {}
        }
    }

    if do_recompute {
        recompute_and_emit(state, conn);
    }

    if emit_seeked
        && let Err(e) = conn.emit_signal(
            None::<&str>,
            MPRIS_PATH,
            "org.mpris.MediaPlayer2.Player",
            "Seeked",
            &snap.position_us,
        )
    {
        eprintln!("mpris: emit Seeked: {}", e);
    }
}

fn recompute_and_emit(state: &Arc<Mutex<State>>, conn: &Connection) {
    let (changed, new_view) = {
        let mut s = state.lock();
        let next = project_view(&s.snapshot, &s.content);
        let names = diff_view(&s.view, &next);
        s.view = next.clone();
        (names, next)
    };
    if changed.is_empty() {
        return;
    }
    emit_properties_changed(conn, &changed, &new_view);
}

/// Build the PropertiesChanged signal body and emit it directly. Mirrors
/// the legacy `sd_bus_emit_properties_changed_strv` call but ships values
/// in the changed dict rather than via the invalidated list, which is
/// what zbus's auto-generated property-change helpers also do.
fn emit_properties_changed(conn: &Connection, names: &[&str], view: &View) {
    let mut changed: HashMap<&str, Value> = HashMap::new();
    for name in names {
        let v = match *name {
            "PlaybackStatus" => Value::from(view.playback_status.to_string()),
            "Rate" => Value::from(view.rate),
            "Metadata" => {
                let dict = metadata_to_dict(&view.metadata);
                // HashMap<String, OwnedValue> -> a{sv} via Value::from
                Value::from(dict)
            }
            "Volume" => Value::from(view.volume),
            "CanGoNext" => Value::from(view.can_go_next),
            "CanGoPrevious" => Value::from(view.can_go_previous),
            "CanPlay" => Value::from(view.can_play),
            "CanPause" => Value::from(view.can_pause),
            "CanSeek" => Value::from(view.can_seek),
            "CanControl" => Value::from(view.can_control),
            _ => continue,
        };
        changed.insert(*name, v);
    }
    if changed.is_empty() {
        return;
    }
    let invalidated: Vec<&str> = Vec::new();
    let iface_name = InterfaceName::try_from("org.mpris.MediaPlayer2.Player").unwrap();
    if let Err(e) = conn.emit_signal(
        None::<&str>,
        MPRIS_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        &(iface_name.as_str(), changed, invalidated),
    ) {
        eprintln!("mpris: emit PropertiesChanged: {}", e);
    }
}

// ============================================================================
// FFI: start / stop
// ============================================================================

/// Spawn the MPRIS sink thread. `service_suffix` is appended to the base
/// service name (`org.mpris.MediaPlayer2.JellyfinDesktop<suffix>`) — pass
/// an empty NUL-terminated string for none. No-op if already running.
///
/// # Safety
/// `service_suffix` must be NUL-terminated or null.
pub unsafe fn jfn_mpris_sink_start(service_suffix: *const c_char) {
    let mut slot = sink_slot().lock();
    if slot.is_some() {
        return;
    }
    let suffix = if service_suffix.is_null() {
        String::new()
    } else {
        unsafe { std::ffi::CStr::from_ptr(service_suffix) }
            .to_string_lossy()
            .into_owned()
    };
    let (tx, rx) = channel::<Msg>();
    let join = thread::Builder::new()
        .name("mpris-sink".into())
        .spawn(move || worker(rx, suffix))
        .expect("spawn mpris-sink");
    *slot = Some(Sink {
        tx,
        join: Some(join),
    });
}

pub fn jfn_mpris_sink_stop() {
    let mut slot = sink_slot().lock();
    let Some(mut s) = slot.take() else {
        return;
    };
    let _ = s.tx.send(Msg::Stop);
    if let Some(h) = s.join.take() {
        let _ = h.join();
    }
}
