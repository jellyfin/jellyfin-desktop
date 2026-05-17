//! Format trait for typed property/option access.

use crate::sys;

/// Maps a Rust type to its libmpv format tag. Used by `Handle::get_property`,
/// `Handle::set_property_async`, `Handle::set_option`, and
/// `Handle::observe_property`.
pub trait Format: Sized {
    const MPV_FORMAT: sys::mpv_format;
}

impl Format for i64 {
    const MPV_FORMAT: sys::mpv_format = sys::mpv_format::MPV_FORMAT_INT64;
}

impl Format for f64 {
    const MPV_FORMAT: sys::mpv_format = sys::mpv_format::MPV_FORMAT_DOUBLE;
}

/// libmpv's flag format. Stored as `i32` (0 or 1) on the wire; the safe API
/// exposes `bool`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Flag(pub bool);

impl From<bool> for Flag {
    fn from(b: bool) -> Self {
        Self(b)
    }
}

impl From<Flag> for bool {
    fn from(f: Flag) -> Self {
        f.0
    }
}

impl Format for Flag {
    const MPV_FORMAT: sys::mpv_format = sys::mpv_format::MPV_FORMAT_FLAG;
}
