//! Registry probe for decoration-related globals.
//!
//! The probe opens its own connection, reads the global list, and caches the
//! answer for the lifetime of the process.
//!
//! Seeded from `Platform::early_init`, which runs before the mpv proxy
//! rewrites `WAYLAND_DISPLAY` — a later lazy probe would connect to the proxy
//! socket instead of the real compositor.

use std::sync::OnceLock;
use std::time::Duration;

use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::wl_registry;
use wayland_client::{Connection, Dispatch, QueueHandle};

#[derive(Copy, Clone, Debug, Default)]
pub(crate) struct DecorationGlobals {
    /// `org_kde_kwin_server_decoration_palette_manager` — SSD can be tinted.
    pub(crate) kde_palette: bool,
}

static GLOBALS: OnceLock<DecorationGlobals> = OnceLock::new();

pub(crate) fn init() {
    let _ = GLOBALS.set(probe_bounded(Duration::from_secs(2)));
}

/// Probe failure (or a missed `init`) reads as "no globals", which resolves
/// to CSD — the only mode that never depends on the compositor.
pub(crate) fn globals() -> DecorationGlobals {
    GLOBALS.get().copied().unwrap_or_default()
}

struct ProbeState;

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for ProbeState {
    fn event(
        _: &mut Self,
        _: &wl_registry::WlRegistry,
        _: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

fn probe() -> DecorationGlobals {
    if std::env::var_os("WAYLAND_DISPLAY").is_none() && std::env::var_os("WAYLAND_SOCKET").is_none()
    {
        return DecorationGlobals::default();
    }
    let Ok(conn) = Connection::connect_to_env() else {
        return DecorationGlobals::default();
    };
    let Ok((globals, _queue)) = registry_queue_init::<ProbeState>(&conn) else {
        return DecorationGlobals::default();
    };
    let mut found = DecorationGlobals::default();
    globals.contents().with_list(|list| {
        for global in list {
            if global.interface == "org_kde_kwin_server_decoration_palette_manager" {
                found.kde_palette = true;
            }
        }
    });
    found
}

/// [`probe`] on a throwaway thread, abandoned on timeout: the round trip
/// blocks indefinitely if the compositor stalls, and this runs inline during
/// startup.
fn probe_bounded(timeout: Duration) -> DecorationGlobals {
    let (tx, rx) = std::sync::mpsc::channel();
    let spawned = std::thread::Builder::new()
        .name("wl-deco-probe".into())
        .spawn(move || {
            let _ = tx.send(probe());
        })
        .is_ok();
    if !spawned {
        return DecorationGlobals::default();
    }
    rx.recv_timeout(timeout).unwrap_or_default()
}
