//! Playback state machine + coordinator.
//!
//! Worker thread drains queued inputs into a deterministic state machine,
//! stamps each emitted event with the post-transition snapshot, and fans
//! out to registered sinks via the FFI vtable. Sink delivery is
//! non-blocking: sinks own their own consumer threads.

mod browser_sink;
mod coordinator;
mod exec_js;
mod ffi;
mod hotkey;
mod ingest;
mod idle_inhibit_sink;
mod ingest_driver;
mod mpris;
#[cfg(target_os = "linux")]
mod mpris_sink;
mod position_cache;
mod shutdown;
mod state_machine;
mod theme_color_sink;
mod types;
mod wake_event;

pub use coordinator::PlaybackCoordinator;
pub use ffi::*;
pub use hotkey::*;
pub use shutdown::*;
pub use state_machine::PlaybackStateMachine;
pub use types::*;
pub use wake_event::WakeEvent;
