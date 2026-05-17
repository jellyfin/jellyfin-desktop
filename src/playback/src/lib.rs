//! Playback state machine + coordinator.
//!
//! Worker thread drains queued inputs into a deterministic state machine,
//! stamps each emitted event with the post-transition snapshot, and fans
//! out to registered sinks via the FFI vtable. Sink delivery is
//! non-blocking: sinks own their own consumer threads.

mod coordinator;
mod dispatcher;
mod ffi;
mod state_machine;
mod types;
mod wake_event;

pub use coordinator::PlaybackCoordinator;
pub use ffi::*;
pub use state_machine::PlaybackStateMachine;
pub use types::*;
pub use wake_event::WakeEvent;
