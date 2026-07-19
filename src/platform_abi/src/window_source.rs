//! Live window geometry, sourced from whichever component owns the window,
//! plus the payload-free change wakeup. Producers update their source and
//! call [`notify_window_changed`]; consumers subscribe and pull a
//! [`WindowSnapshot`].

use std::sync::Arc;

use parking_lot::Mutex;

use crate::geometry::{WindowExtent, WindowPos};

#[derive(Clone, Copy)]
pub struct WindowSnapshot {
    pub extent: Option<WindowExtent>,
    pub position: Option<WindowPos>,
    pub maximized: bool,
    pub fullscreen: bool,
}

pub trait WindowSource: Send + Sync {
    fn snapshot(&self) -> WindowSnapshot;
}

static WINDOW_SUBSCRIBERS: Mutex<Vec<Arc<dyn Fn() + Send + Sync>>> = Mutex::new(Vec::new());

/// Register a window-changed subscriber for the life of the process.
/// Subscribers must not depend on invocation order.
pub fn subscribe_window_changed<F: Fn() + Send + Sync + 'static>(cb: F) {
    WINDOW_SUBSCRIBERS.lock().push(Arc::new(cb));
}

/// Wake every subscriber; each pulls the current snapshot itself. Callers
/// must have already committed the state a pull would read.
pub fn notify_window_changed() {
    let subs: Vec<_> = WINDOW_SUBSCRIBERS.lock().clone();
    for cb in subs {
        cb();
    }
}
