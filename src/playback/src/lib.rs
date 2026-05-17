//! Playback state machine + coordinator.
//!
//! Worker thread drains queued inputs into a deterministic state machine,
//! stamps each emitted event with the post-transition snapshot, and fans
//! out to registered sinks via the FFI vtable. Sink delivery is
//! non-blocking: sinks own their own consumer threads.

mod coordinator;
mod ffi;
mod state_machine;
mod types;

pub use coordinator::PlaybackCoordinator;
pub use ffi::*;
pub use state_machine::PlaybackStateMachine;
pub use types::*;
