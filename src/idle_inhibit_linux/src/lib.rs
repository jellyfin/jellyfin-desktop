//! systemd-logind idle inhibitor via zbus.
//!
//! Holds an OwnedFd returned by org.freedesktop.login1.Manager.Inhibit; the
//! inhibit lasts as long as the fd is open. Replacing the inhibit closes the
//! previous fd, which atomically releases the prior inhibitor.

use std::os::fd::OwnedFd;
use std::sync::Mutex;

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

#[unsafe(no_mangle)]
pub extern "C" fn jfn_idle_inhibit_set(level: u32) {
    let mut state = STATE.lock().unwrap();
    state.fd = None;
    let Some(what) = what_for(level) else {
        return;
    };

    if state.bus.is_none() {
        match Connection::system() {
            Ok(c) => state.bus = Some(c),
            Err(e) => {
                log::error!("idle_inhibit: system bus connect failed: {}", e);
                return;
            }
        }
    }
    let bus = state.bus.as_ref().unwrap();

    let reply = bus.call_method(
        Some("org.freedesktop.login1"),
        "/org/freedesktop/login1",
        Some("org.freedesktop.login1.Manager"),
        "Inhibit",
        &(what, "Jellyfin Desktop", "Media playback", "block"),
    );
    match reply {
        Ok(msg) => match msg.body().deserialize::<ZOwnedFd>() {
            Ok(fd) => state.fd = Some(fd.into()),
            Err(e) => log::error!("idle_inhibit: Inhibit reply not fd: {}", e),
        },
        Err(e) => log::error!("idle_inhibit: Inhibit call failed: {}", e),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_idle_inhibit_cleanup() {
    let mut state = STATE.lock().unwrap();
    state.fd = None;
    state.bus = None;
}
