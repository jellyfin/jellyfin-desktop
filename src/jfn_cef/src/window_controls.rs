//! Shared window-control (client-side decoration) IPC handling.
//!
//! The titlebar/window-controls are app chrome that must work from any browser
//! layer — the main web UI, the server-selection overlay, etc. Each layer's
//! message handler calls [`handle_window_op`] first; if it claims the message,
//! the layer's own dispatch is skipped.

use cef::*;

/// Whether the app should draw client-side decorations: user setting enabled
/// and running on a backend that needs them (Wayland). X11/macOS/Windows draw
/// their own and are excluded.
pub fn csd_enabled() -> bool {
    let display = jfn_platform_abi::get().display();
    jfn_config::client_side_decorations() && display == jfn_platform_abi::DisplayBackend::Wayland
}

fn list_int(args: &ListValue, idx: usize) -> i32 {
    // Integers can arrive as doubles across the V8 boundary.
    if args.get_type(idx).as_ref() == &sys::cef_value_type_t::VTYPE_DOUBLE {
        args.double(idx).round() as i32
    } else {
        args.int(idx)
    }
}

/// Whether `name` is a window-control / CSD IPC message. The base layer
/// dispatch uses this to route such messages here for every layer, before any
/// per-layer handler runs.
pub fn is_window_message(name: &str) -> bool {
    matches!(
        name,
        "windowMinimize"
            | "windowToggleMaximize"
            | "windowClose"
            | "windowStartMove"
            | "windowStartResize"
            | "csdReady"
    )
}

/// Tell the page's CSD module whether to show the titlebar, replying into the
/// frame that asked (so each layer gets the answer in its own context).
fn push_csd_state(browser: Option<&mut Browser>) {
    let Some(frame) = browser.and_then(|b| b.main_frame()) else {
        return;
    };
    let js = format!(
        "window.__jmpCsd&&window.__jmpCsd.setEnabled({});",
        csd_enabled()
    );
    let code = CefString::from(js.as_str());
    frame.execute_java_script(Some(&code), None, 0);
}

/// Handle a window-control / CSD message (callers gate on [`is_window_message`]).
/// `browser` is the layer that sent it, used to reply for `csdReady`.
pub fn handle_window_op(name: &str, args: Option<&ListValue>, browser: Option<&mut Browser>) {
    match name {
        "windowMinimize" => jfn_platform_abi::get().window_minimize(),
        "windowToggleMaximize" => jfn_platform_abi::get().window_toggle_maximize(),
        "windowStartMove" => jfn_platform_abi::get().window_start_move(),
        "windowStartResize" => {
            if let Some(a) = args {
                jfn_platform_abi::get().window_start_resize(list_int(a, 0));
            }
        }
        "windowClose" => jfn_playback::shutdown::jfn_shutdown_initiate(),
        "csdReady" => push_csd_state(browser),
        _ => {}
    }
}
