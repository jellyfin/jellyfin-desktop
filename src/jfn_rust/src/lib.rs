pub mod app;
mod cli;
mod instance_id;
pub mod manager;
mod platform_install;
mod window_geometry;
#[cfg(target_os = "linux")]
mod wl_interpose;
