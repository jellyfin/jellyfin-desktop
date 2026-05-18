//! Open an own wl_display connection, query xdg-output for fractional scale,
//! disconnect. Returns the live fractional scale of the output containing
//! (x, y), or of the first output if x/y are negative. Returns 0.0 on failure
//! (no Wayland session, no xdg-output, etc.) — caller falls back to 1.0.
//!
//! Called before mpv_initialize so the result can scale-correct --geometry
//! pre-init.

use std::env;
use std::os::raw::{c_double, c_int};

use smithay_client_toolkit::output::{OutputHandler, OutputState};
use smithay_client_toolkit::registry::{ProvidesRegistryState, RegistryState};
use smithay_client_toolkit::{
    delegate_output, delegate_registry, registry_handlers,
};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::wl_output;
use wayland_client::{Connection, QueueHandle};

struct State {
    registry_state: RegistryState,
    output_state: OutputState,
}

impl OutputHandler for State {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }
    fn new_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn update_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn output_destroyed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
}

impl ProvidesRegistryState for State {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    registry_handlers![OutputState];
}

delegate_output!(State);
delegate_registry!(State);

fn probe(x: i32, y: i32) -> Option<f64> {
    if env::var_os("WAYLAND_DISPLAY").is_none() && env::var_os("WAYLAND_SOCKET").is_none() {
        return None;
    }

    let conn = Connection::connect_to_env().ok()?;
    let (globals, mut queue) = registry_queue_init::<State>(&conn).ok()?;
    let qh = queue.handle();

    let mut state = State {
        registry_state: RegistryState::new(&globals),
        output_state: OutputState::new(&globals, &qh),
    };

    queue.roundtrip(&mut state).ok()?;
    queue.roundtrip(&mut state).ok()?;

    let mut fallback: Option<f64> = None;
    for output in state.output_state.outputs() {
        let info = state.output_state.info(&output)?;
        let (lx, ly) = info.logical_position?;
        let (lw, lh) = info.logical_size?;
        let mode = info.modes.iter().find(|m| m.current).or_else(|| info.modes.first())?;
        let (mw, _mh) = mode.dimensions;
        if lw <= 0 || mw <= 0 {
            continue;
        }
        let scale = mw as f64 / lw as f64;
        if x >= 0 && y >= 0 && x >= lx && x < lx + lw && y >= ly && y < ly + lh {
            return Some(scale);
        }
        if fallback.is_none() {
            fallback = Some(scale);
        }
    }
    fallback
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wayland_scale_probe(x: c_int, y: c_int) -> c_double {
    probe(x, y).unwrap_or(0.0)
}
