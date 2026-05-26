pub mod app;
mod cli;
#[cfg(unix)]
pub mod signal_guard;
#[cfg(not(target_os = "macos"))]
mod single_instance;
