//! Non-unix stubs for the `Platform` process-lifecycle methods.

use super::Callback;

pub fn install_shutdown(_on_shutdown: fn()) {}

pub fn try_signal_existing(_instance_id: &str) -> bool {
    false
}

pub fn start_listener(_instance_id: &str, _cb: Callback) -> bool {
    false
}

pub fn stop_listener(_instance_id: &str) {}
