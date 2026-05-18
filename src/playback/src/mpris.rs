//! MPRIS Player projection rules.
//!
//! Encodes every derived-field rule that today drives MprisBackend setters
//! and prop_get_* getters:
//!   - PlaybackStatus from playback.phase
//!   - CanPlay/CanPause/CanSeek/CanControl from phase + duration
//!   - Metadata cleared while phase==Stopped (caller substitutes empty
//!     metadata when `metadata_active` is false; lets the C++ wrapper hold
//!     onto the MediaMetadata struct so it isn't shipped through FFI).
//!   - Rate locked to 0 while seeking|buffering|Starting
//!
//! Pass-through fields (volume, can_go_next, can_go_previous, metadata
//! itself) are NOT computed here — they live in the caller's MprisContent
//! and copy straight into the view. The diff over those is trivial bool /
//! double / struct equality which the C++ side handles directly.

use crate::types::PlaybackPhase;

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MprisStatus {
    Stopped = 0,
    Playing = 1,
    Paused = 2,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ProjectInput {
    pub phase: PlaybackPhase,
    pub seeking: bool,
    pub buffering: bool,
    /// Duration from MprisContent.metadata, not from PlaybackSnapshot. The
    /// two diverge during track transitions: snapshot reflects mpv's current
    /// stream, MprisContent reflects the metadata the JS UI most recently
    /// pushed.
    pub metadata_duration_us: i64,
    /// MprisContent.pending_rate — applied verbatim when rolling.
    pub pending_rate: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MprisDerived {
    pub status: MprisStatus,
    pub can_play: bool,
    pub can_pause: bool,
    pub can_seek: bool,
    pub can_control: bool,
    /// False -> caller substitutes empty metadata in the projected view so
    /// MPRIS clients see a clean transport when nothing is loaded.
    pub metadata_active: bool,
    pub rate: f64,
}

fn status_for(phase: PlaybackPhase) -> MprisStatus {
    // MPRIS only recognizes Playing/Paused/Stopped. Pre-roll (Starting)
    // reflects user intent: the user pressed play, so PlaybackStatus reads
    // Playing. The fact that frames aren't actually rolling yet is signalled
    // through Rate=0 below.
    match phase {
        PlaybackPhase::Playing | PlaybackPhase::Starting => MprisStatus::Playing,
        PlaybackPhase::Paused => MprisStatus::Paused,
        PlaybackPhase::Stopped => MprisStatus::Stopped,
    }
}

fn is_active(phase: PlaybackPhase) -> bool {
    matches!(
        phase,
        PlaybackPhase::Playing | PlaybackPhase::Paused | PlaybackPhase::Starting
    )
}

pub fn project(input: &ProjectInput) -> MprisDerived {
    let active = is_active(input.phase);
    // CanPause is true while committed to playing — Playing or Starting (user
    // already pressed play). Paused exposes Play, not Pause; Stopped exposes
    // neither.
    let can_pause = matches!(
        input.phase,
        PlaybackPhase::Playing | PlaybackPhase::Starting
    );
    // Rate reflects actual frame motion, not user intent. Anything other
    // than steady playback (pre-roll, seek, buffer underrun) pins it to 0
    // so MPRIS clients don't extrapolate position.
    let rolling =
        input.phase == PlaybackPhase::Playing && !input.seeking && !input.buffering;
    MprisDerived {
        status: status_for(input.phase),
        can_play: active,
        can_pause,
        can_seek: active && input.metadata_duration_us > 0,
        can_control: active,
        metadata_active: active,
        rate: if rolling { input.pending_rate } else { 0.0 },
    }
}

// ============================================================================
// FFI
// ============================================================================

#[repr(C)]
#[derive(Clone, Copy)]
pub struct JfnMprisDerivedC {
    /// 0=Stopped 1=Playing 2=Paused
    pub status: u8,
    pub can_play: bool,
    pub can_pause: bool,
    pub can_seek: bool,
    pub can_control: bool,
    pub metadata_active: bool,
    pub rate: f64,
}

/// # Safety
/// `out` must point to writable storage for one `JfnMprisDerivedC`. `phase`
/// must be a valid PlaybackPhase discriminant (0..=3).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_mpris_project(
    phase: u8,
    seeking: bool,
    buffering: bool,
    metadata_duration_us: i64,
    pending_rate: f64,
    out: *mut JfnMprisDerivedC,
) {
    let phase = match phase {
        0 => PlaybackPhase::Starting,
        1 => PlaybackPhase::Playing,
        2 => PlaybackPhase::Paused,
        _ => PlaybackPhase::Stopped,
    };
    let derived = project(&ProjectInput {
        phase,
        seeking,
        buffering,
        metadata_duration_us,
        pending_rate,
    });
    unsafe {
        out.write(JfnMprisDerivedC {
            status: derived.status as u8,
            can_play: derived.can_play,
            can_pause: derived.can_pause,
            can_seek: derived.can_seek,
            can_control: derived.can_control,
            metadata_active: derived.metadata_active,
            rate: derived.rate,
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input(phase: PlaybackPhase) -> ProjectInput {
        ProjectInput {
            phase,
            seeking: false,
            buffering: false,
            metadata_duration_us: 0,
            pending_rate: 1.0,
        }
    }

    #[test]
    fn stopped_defaults() {
        let v = project(&input(PlaybackPhase::Stopped));
        assert_eq!(v.status, MprisStatus::Stopped);
        assert!(!v.can_play);
        assert!(!v.can_pause);
        assert!(!v.can_seek);
        assert!(!v.can_control);
        assert!(!v.metadata_active);
        assert_eq!(v.rate, 0.0);
    }

    #[test]
    fn stopped_to_playing_flips_caps() {
        let stopped = project(&input(PlaybackPhase::Stopped));
        let playing = project(&input(PlaybackPhase::Playing));
        assert_ne!(stopped.status, playing.status);
        assert!(!stopped.can_play && playing.can_play);
        assert!(!stopped.can_pause && playing.can_pause);
        assert!(!stopped.can_control && playing.can_control);
        // duration still 0 -> CanSeek unchanged at false
        assert!(!stopped.can_seek && !playing.can_seek);
    }

    #[test]
    fn metadata_arriving_while_playing_flips_can_seek() {
        let no_dur = project(&input(PlaybackPhase::Playing));
        let mut with_dur = input(PlaybackPhase::Playing);
        with_dur.metadata_duration_us = 60_000_000;
        let with_dur = project(&with_dur);
        assert!(!no_dur.can_seek);
        assert!(with_dur.can_seek);
        assert_eq!(no_dur.status, with_dur.status);
    }

    #[test]
    fn metadata_suppressed_while_stopped() {
        let mut i = input(PlaybackPhase::Stopped);
        i.metadata_duration_us = 60_000_000;
        let v = project(&i);
        assert!(!v.metadata_active);
        assert!(!v.can_seek);
    }

    #[test]
    fn playing_to_paused_only_status_and_can_pause_flip() {
        let mut i = input(PlaybackPhase::Playing);
        i.metadata_duration_us = 10_000_000;
        let a = project(&i);
        i.phase = PlaybackPhase::Paused;
        let b = project(&i);
        assert_ne!(a.status, b.status);
        assert!(a.can_pause && !b.can_pause);
        assert!(a.can_play && b.can_play);
        assert!(a.can_seek && b.can_seek);
        assert!(a.can_control && b.can_control);
        assert!(a.metadata_active && b.metadata_active);
    }

    #[test]
    fn buffering_while_playing_keeps_status_but_pins_rate() {
        let mut i = input(PlaybackPhase::Playing);
        i.metadata_duration_us = 10_000_000;
        let play = project(&i);
        i.buffering = true;
        let buf = project(&i);
        assert_eq!(play.status, MprisStatus::Playing);
        assert_eq!(buf.status, MprisStatus::Playing);
        assert_eq!(play.rate, 1.0);
        assert_eq!(buf.rate, 0.0);
    }

    #[test]
    fn buffering_locks_rate_pending_rate_restored() {
        let mut i = input(PlaybackPhase::Playing);
        i.pending_rate = 1.5;
        let clear = project(&i);
        assert_eq!(clear.rate, 1.5);
        i.buffering = true;
        let buf = project(&i);
        assert_eq!(buf.rate, 0.0);
    }

    #[test]
    fn seeking_also_pins_rate() {
        let mut i = input(PlaybackPhase::Playing);
        i.seeking = true;
        assert_eq!(project(&i).rate, 0.0);
    }

    #[test]
    fn redundant_input_produces_identical_view() {
        let mut i = input(PlaybackPhase::Playing);
        i.metadata_duration_us = 60_000_000;
        assert_eq!(project(&i), project(&i));
    }

    #[test]
    fn transition_to_stopped_clears_everything() {
        let mut i = input(PlaybackPhase::Playing);
        i.metadata_duration_us = 10_000_000;
        let a = project(&i);
        i.phase = PlaybackPhase::Stopped;
        let b = project(&i);
        assert_ne!(a.status, b.status);
        assert!(a.can_play && !b.can_play);
        assert!(a.can_pause && !b.can_pause);
        assert!(a.can_seek && !b.can_seek);
        assert!(a.can_control && !b.can_control);
        assert!(a.metadata_active && !b.metadata_active);
    }

    #[test]
    fn starting_projects_as_playing_with_rate_zero() {
        let mut i = input(PlaybackPhase::Starting);
        i.metadata_duration_us = 10_000_000;
        let v = project(&i);
        assert_eq!(v.status, MprisStatus::Playing);
        assert_eq!(v.rate, 0.0);
        assert!(v.can_play);
        assert!(v.can_pause);
        assert!(v.can_seek);
        assert!(v.can_control);
        assert!(v.metadata_active);
    }

    #[test]
    fn starting_to_playing_flips_rate_but_not_status() {
        let mut i = input(PlaybackPhase::Starting);
        i.metadata_duration_us = 10_000_000;
        let pre = project(&i);
        i.phase = PlaybackPhase::Playing;
        let play = project(&i);
        assert_eq!(pre.status, play.status);
        assert_eq!(pre.can_pause, play.can_pause);
        assert_ne!(pre.rate, play.rate);
    }

    #[test]
    fn paused_phase_status() {
        let v = project(&input(PlaybackPhase::Paused));
        assert_eq!(v.status, MprisStatus::Paused);
        assert!(v.can_play);
        assert!(!v.can_pause);
        assert!(v.can_control);
        assert!(v.metadata_active);
        assert_eq!(v.rate, 0.0);
    }
}
