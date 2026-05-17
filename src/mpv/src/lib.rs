//! Safe Rust bindings for libmpv (`mpv/client.h`).
//!
//! Scope is the control plane: handle lifecycle, options, properties,
//! commands, events, logging. Render APIs (`mpv/render.h`,
//! `mpv/render_gl.h`) are intentionally excluded — the desktop client uses
//! the mpv-as-window model where mpv owns its own window/GPU and libmpv is
//! only the control surface.

#![warn(unsafe_op_in_unsafe_fn)]

pub mod sys;

mod command;
mod error;
mod event;
mod event_loop;
mod handle;
mod log;
mod node;
mod options;
mod property;

pub mod boot;
pub mod capabilities;
pub mod color;
pub mod probe;

pub use command::Command;
pub use error::{Error, Result};
pub use event::{EndFileReason, Event, LogMessage, ObserveId, PropertyValue};
pub use event_loop::EventLoop;
pub use handle::{Handle, WakeupCallback};
pub use log::{LogLevel, forward_to_tracing as forward_log_to_tracing};
pub use node::{Node, NodeArray, NodeMap};
pub use options::{HWDEC_DEFAULT, hwdec_options, is_valid_hwdec};
pub use property::Format;
