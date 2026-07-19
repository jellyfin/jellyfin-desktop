//! The mpv-backed [`WindowSource`]: on backends where mpv owns the OS
//! window (macOS / Windows / X11), live geometry comes from the ingest
//! extent cell that mpv's property observations feed.

use jfn_platform_abi::{WindowSnapshot, WindowSource};

pub struct MpvWindowSource;

pub static MPV_WINDOW_SOURCE: MpvWindowSource = MpvWindowSource;

impl WindowSource for MpvWindowSource {
    fn snapshot(&self) -> WindowSnapshot {
        WindowSnapshot {
            extent: crate::ingest_driver::jfn_playback_window_extent(),
            position: jfn_platform_abi::get().query_window_position(),
            maximized: crate::ingest_driver::jfn_playback_window_maximized(),
            fullscreen: crate::ingest_driver::jfn_playback_fullscreen(),
        }
    }
}
