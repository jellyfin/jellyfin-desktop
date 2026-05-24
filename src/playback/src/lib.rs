//! Playback state machine + coordinator.
//!
//! Worker thread drains queued inputs into a deterministic state machine,
//! stamps each emitted event with the post-transition snapshot, and fans
//! out to registered sinks via the FFI vtable. Sink delivery is
//! non-blocking: sinks own their own consumer threads.

pub mod browser_sink;
mod coordinator;
pub mod exec_js;
pub mod ffi;
mod hotkey;
pub mod idle_inhibit_sink;
mod ingest;
pub mod ingest_driver;
mod mpris;
#[cfg(target_os = "linux")]
pub mod mpris_sink;
pub mod shutdown;
mod state_machine;
pub mod theme_color_sink;
mod types;
pub mod wake_event;

pub use coordinator::PlaybackCoordinator;
pub use ffi::*;
pub use hotkey::*;
pub use shutdown::*;
pub use state_machine::PlaybackStateMachine;
pub use types::*;
pub use wake_event::WakeEvent;
