#![cfg(target_os = "linux")]

pub mod interaction_fsm;
pub mod render;

pub use render::{Fonts, Layout, layout, paint};

#[derive(Clone)]
pub struct MenuItem {
    pub id: i32,
    pub label: String,
    pub enabled: bool,
    pub separator: bool,
}
