//! systemd-logind idle inhibitor via zbus.
//!
//! Holds an OwnedFd returned by org.freedesktop.login1.Manager.Inhibit; the
//! inhibit lasts as long as the fd is open. Replacing the inhibit closes the
//! previous fd, which atomically releases the prior inhibitor.

use parking_lot::Mutex;
use std::os::fd::OwnedFd;

use zbus::blocking::Connection;
use zbus::zvariant::OwnedFd as ZOwnedFd;

const LEVEL_SYSTEM: u32 = 1;
const LEVEL_DISPLAY: u32 = 2;

struct State {
    bus: Option<Connection>,
    fd: Option<OwnedFd>,
}

static STATE: Mutex<State> = Mutex::new(State {
    bus: None,
    fd: None,
});

fn what_for(level: u32) -> Option<&'static str> {
    match level {
        LEVEL_SYSTEM => Some("sleep"),
        LEVEL_DISPLAY => Some("idle:sleep"),
        _ => None,
    }
}

pub fn set(level: u32) {
    let mut state = STATE.lock();
    state.fd = None;
    let Some(what) = what_for(level) else {
        return;
    };

    if state.bus.is_none() {
        match Connection::system() {
            Ok(c) => state.bus = Some(c),
            Err(e) => {
                tracing::error!("idle_inhibit: system bus connect failed: {}", e);
                return;
            }
        }
    }
    let Some(bus) = state.bus.as_ref() else {
        return;
    };

    let reply = bus.call_method(
        Some("org.freedesktop.login1"),
        "/org/freedesktop/login1",
        Some("org.freedesktop.login1.Manager"),
        "Inhibit",
        &(what, "Jellium Desktop", "Media playback", "block"),
    );
    match reply {
        Ok(msg) => match msg.body().deserialize::<ZOwnedFd>() {
            Ok(fd) => state.fd = Some(fd.into()),
            Err(e) => tracing::error!("idle_inhibit: Inhibit reply not fd: {}", e),
        },
        Err(e) => tracing::error!("idle_inhibit: Inhibit call failed: {}", e),
    }
}

pub fn cleanup() {
    let mut state = STATE.lock();
    state.fd = None;
    state.bus = None;
}
