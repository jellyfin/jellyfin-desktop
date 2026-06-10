pub mod app;
mod cli;
pub mod manager;
mod platform_install;
#[cfg(unix)]
pub mod signal_guard;
mod single_instance;
mod window_geometry;
#[cfg(target_os = "linux")]
pub mod wl_interpose;
