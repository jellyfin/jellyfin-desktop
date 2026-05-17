//! Owned event types decoded from `mpv_event`.
//!
//! Raw `mpv_event` payloads (returned by `mpv_wait_event`) are only valid
//! until the next `mpv_wait_event` call on the same handle. `Event::from_raw`
//! copies the data out so the caller can drop the loan immediately.

use crate::log::LogLevel;
use crate::node::Node;
use crate::sys;
use std::ffi::CStr;

/// User-assigned ID passed as `reply_userdata` to
/// `mpv_observe_property`. Re-emitted on `Event::PropertyChange` so callers
/// can dispatch without string-comparing property names.
pub type ObserveId = u64;

#[derive(Clone, Debug, PartialEq)]
pub enum EndFileReason {
    Eof,
    Stop,
    Quit,
    Error(crate::error::Error),
    Redirect,
    Unknown(i32),
}

impl EndFileReason {
    fn from_raw(reason: sys::mpv_end_file_reason, error: i32) -> Self {
        match reason {
            sys::mpv_end_file_reason::MPV_END_FILE_REASON_EOF => Self::Eof,
            sys::mpv_end_file_reason::MPV_END_FILE_REASON_STOP => Self::Stop,
            sys::mpv_end_file_reason::MPV_END_FILE_REASON_QUIT => Self::Quit,
            sys::mpv_end_file_reason::MPV_END_FILE_REASON_ERROR => {
                Self::Error(crate::error::Error::new(error))
            }
            sys::mpv_end_file_reason::MPV_END_FILE_REASON_REDIRECT => Self::Redirect,
            other => Self::Unknown(other.0 as i32),
        }
    }
}

/// Property-change payload, format-typed. Mirrors what
/// `mpv_event_property::data` decodes to under each `mpv_format`.
#[derive(Clone, Debug, PartialEq)]
pub enum PropertyValue {
    None,
    Flag(bool),
    Int(i64),
    Double(f64),
    String(String),
    Node(Node),
}

impl PropertyValue {
    /// # Safety
    /// `p` must point to a valid `mpv_event_property` whose `data` (if
    /// non-null) matches the declared `format`.
    pub unsafe fn from_raw(p: *const sys::mpv_event_property) -> Self {
        if p.is_null() {
            return Self::None;
        }
        let p = unsafe { &*p };
        if p.data.is_null() {
            return Self::None;
        }
        match p.format {
            sys::mpv_format::MPV_FORMAT_FLAG => {
                Self::Flag(unsafe { *(p.data as *const i32) } != 0)
            }
            sys::mpv_format::MPV_FORMAT_INT64 => {
                Self::Int(unsafe { *(p.data as *const i64) })
            }
            sys::mpv_format::MPV_FORMAT_DOUBLE => {
                Self::Double(unsafe { *(p.data as *const f64) })
            }
            sys::mpv_format::MPV_FORMAT_STRING => unsafe {
                let pp = p.data as *const *const std::os::raw::c_char;
                let s = *pp;
                if s.is_null() {
                    Self::String(String::new())
                } else {
                    Self::String(CStr::from_ptr(s).to_string_lossy().into_owned())
                }
            },
            sys::mpv_format::MPV_FORMAT_NODE => {
                Self::Node(unsafe { Node::from_raw(p.data as *const sys::mpv_node) })
            }
            _ => Self::None,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct LogMessage {
    pub prefix: String,
    pub level: LogLevel,
    pub text: String,
}

/// Reply identifier carried by async command/property events.
pub type ReplyUserdata = u64;

#[derive(Clone, Debug, PartialEq)]
pub enum Event {
    /// `MPV_EVENT_NONE` — emitted on timeout from `wait_event`.
    None,
    Shutdown,
    LogMessage(LogMessage),
    GetPropertyReply {
        reply: ReplyUserdata,
        error: i32,
        value: PropertyValue,
        name: String,
    },
    SetPropertyReply {
        reply: ReplyUserdata,
        error: i32,
    },
    CommandReply {
        reply: ReplyUserdata,
        error: i32,
    },
    StartFile,
    EndFile(EndFileReason),
    FileLoaded,
    ClientMessage(Vec<String>),
    VideoReconfig,
    AudioReconfig,
    Seek,
    PlaybackRestart,
    PropertyChange {
        id: ObserveId,
        name: String,
        value: PropertyValue,
    },
    QueueOverflow,
    Hook {
        reply: ReplyUserdata,
        name: String,
    },
    Other(u32),
}

impl Event {
    /// Decode a raw `mpv_event` borrowed from libmpv into an owned `Event`.
    ///
    /// # Safety
    /// `ev` must reference a valid `mpv_event` returned by `mpv_wait_event`.
    /// All borrowed pointers are copied; the caller may invoke
    /// `mpv_wait_event` again immediately after this returns.
    pub unsafe fn from_raw(ev: *const sys::mpv_event) -> Self {
        if ev.is_null() {
            return Event::None;
        }
        let ev = unsafe { &*ev };
        match ev.event_id {
            sys::mpv_event_id::MPV_EVENT_NONE => Event::None,
            sys::mpv_event_id::MPV_EVENT_SHUTDOWN => Event::Shutdown,
            sys::mpv_event_id::MPV_EVENT_LOG_MESSAGE => unsafe {
                let m = &*(ev.data as *const sys::mpv_event_log_message);
                Event::LogMessage(LogMessage {
                    prefix: cstr_to_string(m.prefix),
                    level: LogLevel::from_raw(m.log_level),
                    text: cstr_to_string(m.text),
                })
            },
            sys::mpv_event_id::MPV_EVENT_GET_PROPERTY_REPLY => unsafe {
                let p = ev.data as *const sys::mpv_event_property;
                let name = if p.is_null() { String::new() } else { cstr_to_string((*p).name) };
                Event::GetPropertyReply {
                    reply: ev.reply_userdata,
                    error: ev.error,
                    value: PropertyValue::from_raw(p),
                    name,
                }
            },
            sys::mpv_event_id::MPV_EVENT_SET_PROPERTY_REPLY => Event::SetPropertyReply {
                reply: ev.reply_userdata,
                error: ev.error,
            },
            sys::mpv_event_id::MPV_EVENT_COMMAND_REPLY => Event::CommandReply {
                reply: ev.reply_userdata,
                error: ev.error,
            },
            sys::mpv_event_id::MPV_EVENT_START_FILE => Event::StartFile,
            sys::mpv_event_id::MPV_EVENT_END_FILE => unsafe {
                let d = &*(ev.data as *const sys::mpv_event_end_file);
                Event::EndFile(EndFileReason::from_raw(d.reason, d.error))
            },
            sys::mpv_event_id::MPV_EVENT_FILE_LOADED => Event::FileLoaded,
            sys::mpv_event_id::MPV_EVENT_CLIENT_MESSAGE => unsafe {
                let m = &*(ev.data as *const sys::mpv_event_client_message);
                let mut args = Vec::with_capacity(m.num_args.max(0) as usize);
                for i in 0..m.num_args {
                    let p = *m.args.offset(i as isize);
                    args.push(cstr_to_string(p));
                }
                Event::ClientMessage(args)
            },
            sys::mpv_event_id::MPV_EVENT_VIDEO_RECONFIG => Event::VideoReconfig,
            sys::mpv_event_id::MPV_EVENT_AUDIO_RECONFIG => Event::AudioReconfig,
            sys::mpv_event_id::MPV_EVENT_SEEK => Event::Seek,
            sys::mpv_event_id::MPV_EVENT_PLAYBACK_RESTART => Event::PlaybackRestart,
            sys::mpv_event_id::MPV_EVENT_PROPERTY_CHANGE => unsafe {
                let p = ev.data as *const sys::mpv_event_property;
                let name = if p.is_null() { String::new() } else { cstr_to_string((*p).name) };
                Event::PropertyChange {
                    id: ev.reply_userdata,
                    name,
                    value: PropertyValue::from_raw(p),
                }
            },
            sys::mpv_event_id::MPV_EVENT_QUEUE_OVERFLOW => Event::QueueOverflow,
            sys::mpv_event_id::MPV_EVENT_HOOK => unsafe {
                let h = &*(ev.data as *const sys::mpv_event_hook);
                Event::Hook {
                    reply: h.id,
                    name: cstr_to_string(h.name),
                }
            },
            other => Event::Other(other.0 as u32),
        }
    }
}

unsafe fn cstr_to_string(p: *const std::os::raw::c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
    }
}
