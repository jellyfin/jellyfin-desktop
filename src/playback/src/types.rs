//! Pure data types shared between the state machine and coordinator.
//!
//! These are internal Rust types. The FFI-facing shapes live in `ffi.rs`
//! and are populated from these at sink-delivery time.

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, Default)]
pub enum MediaType {
    #[default]
    Unknown = 0,
    Audio = 1,
    Video = 2,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, Default)]
pub enum PlayerPresence {
    #[default]
    None = 0,
    Present = 1,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, Default)]
pub enum PlaybackPhase {
    Starting = 0,
    Playing = 1,
    Paused = 2,
    #[default]
    Stopped = 3,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EndReason {
    Eof = 0,
    Error = 1,
    Canceled = 2,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct MediaMetadata {
    pub id: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub track_number: i32,
    pub duration_us: i64,
    pub art_url: String,
    pub art_data_uri: String,
    pub media_type: MediaType,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PlaybackBufferedRange {
    pub start_ticks: i64,
    pub end_ticks: i64,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct PlaybackSnapshot {
    pub presence: PlayerPresence,
    pub phase: PlaybackPhase,
    pub seeking: bool,
    pub buffering: bool,
    pub media_type: MediaType,
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
    pub buffered: Vec<PlaybackBufferedRange>,
}

impl PlaybackSnapshot {
    pub(crate) fn fresh() -> Self {
        Self {
            rate: 1.0,
            ..Default::default()
        }
    }
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PlaybackEventKind {
    Started = 0,
    Paused = 1,
    Finished = 2,
    Canceled = 3,
    Error = 4,
    SeekingChanged = 5,
    BufferingChanged = 6,
    MediaTypeChanged = 7,
    TrackLoaded = 8,
    PositionChanged = 9,
    DurationChanged = 10,
    RateChanged = 11,
    FullscreenChanged = 12,
    OsdDimsChanged = 13,
    BufferedRangesChanged = 14,
    DisplayHzChanged = 15,
    MetadataChanged = 16,
    ArtworkChanged = 17,
    QueueCapsChanged = 18,
    Seeked = 19,
}

#[derive(Clone, Debug)]
pub struct PlaybackEvent {
    pub kind: PlaybackEventKind,
    pub flag: bool,
    pub error_message: String,
    pub snapshot: PlaybackSnapshot,
    pub metadata: MediaMetadata,
    pub artwork_uri: String,
    pub can_go_next: bool,
    pub can_go_prev: bool,
}

impl PlaybackEvent {
    pub(crate) fn new(kind: PlaybackEventKind) -> Self {
        Self {
            kind,
            flag: false,
            error_message: String::new(),
            snapshot: PlaybackSnapshot::default(),
            metadata: MediaMetadata::default(),
            artwork_uri: String::new(),
            can_go_next: false,
            can_go_prev: false,
        }
    }
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PlaybackActionKind {
    ApplyPendingTrackSelectionAndPlay = 0,
}

#[derive(Clone, Copy, Debug)]
pub struct PlaybackAction {
    pub kind: PlaybackActionKind,
}
