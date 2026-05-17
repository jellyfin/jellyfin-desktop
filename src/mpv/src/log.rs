//! libmpv log-level mapping.

use crate::sys;

/// libmpv log severities, in the order libmpv defines them. `Off` disables
/// subscription.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum LogLevel {
    Off = sys::mpv_log_level::MPV_LOG_LEVEL_NONE.0,
    Fatal = sys::mpv_log_level::MPV_LOG_LEVEL_FATAL.0,
    Error = sys::mpv_log_level::MPV_LOG_LEVEL_ERROR.0,
    Warn = sys::mpv_log_level::MPV_LOG_LEVEL_WARN.0,
    Info = sys::mpv_log_level::MPV_LOG_LEVEL_INFO.0,
    /// Maps to mpv's "v".
    Verbose = sys::mpv_log_level::MPV_LOG_LEVEL_V.0,
    /// Maps to mpv's "debug".
    Debug = sys::mpv_log_level::MPV_LOG_LEVEL_DEBUG.0,
    /// Maps to mpv's "trace".
    Trace = sys::mpv_log_level::MPV_LOG_LEVEL_TRACE.0,
}

impl LogLevel {
    /// Token accepted by `mpv_request_log_messages`.
    pub fn as_token(self) -> &'static str {
        match self {
            LogLevel::Off => "no",
            LogLevel::Fatal => "fatal",
            LogLevel::Error => "error",
            LogLevel::Warn => "warn",
            LogLevel::Info => "info",
            LogLevel::Verbose => "v",
            LogLevel::Debug => "debug",
            LogLevel::Trace => "trace",
        }
    }

    /// Inverse of `from_raw` for the libmpv enum.
    pub fn from_raw(raw: sys::mpv_log_level) -> Self {
        match raw {
            sys::mpv_log_level::MPV_LOG_LEVEL_FATAL => LogLevel::Fatal,
            sys::mpv_log_level::MPV_LOG_LEVEL_ERROR => LogLevel::Error,
            sys::mpv_log_level::MPV_LOG_LEVEL_WARN => LogLevel::Warn,
            sys::mpv_log_level::MPV_LOG_LEVEL_INFO => LogLevel::Info,
            sys::mpv_log_level::MPV_LOG_LEVEL_V => LogLevel::Verbose,
            sys::mpv_log_level::MPV_LOG_LEVEL_DEBUG => LogLevel::Debug,
            sys::mpv_log_level::MPV_LOG_LEVEL_TRACE => LogLevel::Trace,
            _ => LogLevel::Off,
        }
    }
}
