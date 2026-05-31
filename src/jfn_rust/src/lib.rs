pub mod app;
mod cli;
pub mod manager;
#[cfg(unix)]
pub mod signal_guard;
#[cfg(not(target_os = "macos"))]
mod single_instance;
