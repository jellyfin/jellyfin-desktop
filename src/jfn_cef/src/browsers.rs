//! Process-wide layer registry. Ports `src/browser/browsers.{cpp,h}`.
//!
//! Owns the Vec<*mut JfnCefLayer> (each layer is jfn_cef_layer_new'd at
//! create time and jfn_cef_layer_free'd at remove), the active-input
//! target, and the broadcast display state. Restack hands the platform a
//! window-Z-ordered list of opaque surface pointers extracted from each
//! layer.

use std::ffi::c_char;
use std::os::raw::c_void;
use std::sync::Mutex;

use crate::client::JfnCefLayer;

use crate::client::{
    jfn_cef_layer_close_browser_force, jfn_cef_layer_free, jfn_cef_layer_get_surface,
    jfn_cef_layer_is_closed, jfn_cef_layer_new, jfn_cef_layer_on_deactivated, jfn_cef_layer_resize,
    jfn_cef_layer_send_mouse_move, jfn_cef_layer_set_focus,
    jfn_cef_layer_set_injection_profile_kind, jfn_cef_layer_set_refresh_rate,
    jfn_cef_layer_set_surface, jfn_cef_layer_wait_for_close, jfn_cef_set_default_frame_rate,
    jfn_cef_set_use_shared_textures,
};
use jfn_input::jfn_input_last_mouse_pos;

struct Browsers {
    layers: Vec<*mut JfnCefLayer>,
    active: *mut JfnCefLayer,
    lw: i32,
    lh: i32,
    pw: i32,
    ph: i32,
    frame_rate: i32,
}

unsafe impl Send for Browsers {}

static INSTANCE: Mutex<Option<Browsers>> = Mutex::new(None);

pub fn jfn_browsers_init(
    lw: i32,
    lh: i32,
    pw: i32,
    ph: i32,
    frame_rate: f64,
    use_shared_textures: bool,
) {
    let fr = if frame_rate > 0.0 {
        (frame_rate + 0.5) as i32
    } else {
        0
    };
    unsafe {
        jfn_cef_set_default_frame_rate(fr);
        jfn_cef_set_use_shared_textures(use_shared_textures);
    }
    *INSTANCE.lock().unwrap() = Some(Browsers {
        layers: Vec::new(),
        active: std::ptr::null_mut(),
        lw,
        lh,
        pw,
        ph,
        frame_rate: fr,
    });
    crate::bridge::install();
}

/// Tear down all remaining layers. Called once at the end of run_with_cef.
pub fn jfn_browsers_shutdown() {
    let Some(b) = INSTANCE.lock().unwrap().take() else {
        return;
    };
    for layer in &b.layers {
        let s = unsafe { jfn_cef_layer_get_surface(*layer) };
        if !s.is_null() {
            jfn_platform_abi::get().free_surface(s);
        }
        unsafe { jfn_cef_layer_free(*layer) };
    }
}

pub unsafe fn jfn_browsers_create(kind: *const c_char) -> *mut JfnCefLayer {
    let mut g = INSTANCE.lock().unwrap();
    let Some(b) = g.as_mut() else {
        return std::ptr::null_mut();
    };

    let surface = jfn_platform_abi::get().alloc_surface();
    let layer = jfn_cef_layer_new();
    if layer.is_null() {
        if !surface.is_null() {
            jfn_platform_abi::get().free_surface(surface);
        }
        return std::ptr::null_mut();
    }
    unsafe {
        jfn_cef_layer_set_surface(layer, surface);
        jfn_cef_layer_resize(layer, b.lw, b.lh, b.pw, b.ph);
        jfn_cef_layer_set_refresh_rate(layer, b.frame_rate as f64);
        if !kind.is_null() {
            let cstr = std::ffi::CStr::from_ptr(kind);
            let bytes = cstr.to_bytes();
            if !bytes.is_empty() {
                jfn_cef_layer_set_injection_profile_kind(
                    layer,
                    bytes.as_ptr() as *const c_char,
                    bytes.len(),
                );
            }
        }
    }
    b.layers.push(layer);
    restack(&b.layers);
    layer
}

pub fn jfn_browsers_remove(layer: *mut JfnCefLayer) {
    if layer.is_null() {
        return;
    }
    let mut g = INSTANCE.lock().unwrap();
    let Some(b) = g.as_mut() else { return };
    unsafe { jfn_cef_layer_on_deactivated(layer) };
    if b.active == layer {
        b.active = std::ptr::null_mut();
    }
    let pos = b.layers.iter().position(|l| *l == layer);
    let Some(idx) = pos else { return };
    let surface = unsafe { jfn_cef_layer_get_surface(layer) };
    b.layers.remove(idx);
    if !surface.is_null() {
        jfn_platform_abi::get().free_surface(surface);
    }
    unsafe { jfn_cef_layer_free(layer) };
    restack(&b.layers);
}

pub fn jfn_browsers_set_active(layer: *mut JfnCefLayer) {
    let mut g = INSTANCE.lock().unwrap();
    let Some(b) = g.as_mut() else { return };
    if b.active == layer {
        return;
    }
    let prev = b.active;
    b.active = layer;
    drop(g);
    if !prev.is_null() {
        unsafe {
            jfn_cef_layer_set_focus(prev, false);
            jfn_cef_layer_on_deactivated(prev);
        }
    }
    if !layer.is_null() {
        unsafe { jfn_cef_layer_set_focus(layer, true) };
        // Leave-then-move forces the renderer to re-emit OnCursorChange.
        let mut x = 0;
        let mut y = 0;
        let mut mods: u32 = 0;
        let valid = jfn_input_last_mouse_pos(&mut x, &mut y, &mut mods);
        if valid != 0 {
            unsafe {
                jfn_cef_layer_send_mouse_move(layer, x, y, mods, true);
                jfn_cef_layer_send_mouse_move(layer, x, y, mods, false);
            }
        }
    }
}

pub fn jfn_browsers_active() -> *mut JfnCefLayer {
    INSTANCE
        .lock()
        .unwrap()
        .as_ref()
        .map(|b| b.active)
        .unwrap_or(std::ptr::null_mut())
}

pub fn jfn_browsers_set_size(lw: i32, lh: i32, pw: i32, ph: i32) {
    let layers: Vec<*mut JfnCefLayer> = {
        let mut g = INSTANCE.lock().unwrap();
        let Some(b) = g.as_mut() else { return };
        b.lw = lw;
        b.lh = lh;
        b.pw = pw;
        b.ph = ph;
        b.layers.clone()
    };
    for l in layers {
        unsafe { jfn_cef_layer_resize(l, lw, lh, pw, ph) };
    }
}

pub fn jfn_browsers_set_scale(scale: f64) {
    let (new_lw, new_lh, pw, ph) = {
        let g = INSTANCE.lock().unwrap();
        let Some(b) = g.as_ref() else { return };
        if scale <= 0.0 || b.pw <= 0 || b.ph <= 0 {
            return;
        }
        (
            (b.pw as f64 / scale) as i32,
            (b.ph as f64 / scale) as i32,
            b.pw,
            b.ph,
        )
    };
    jfn_browsers_set_size(new_lw, new_lh, pw, ph);
}

pub fn jfn_browsers_set_refresh_rate(hz: f64) {
    if hz <= 0.0 {
        return;
    }
    let target = (hz + 0.5) as i32;
    let layers: Vec<*mut JfnCefLayer> = {
        let mut g = INSTANCE.lock().unwrap();
        let Some(b) = g.as_mut() else { return };
        b.frame_rate = target;
        jfn_cef_set_default_frame_rate(target);
        b.layers.clone()
    };
    for l in layers {
        unsafe { jfn_cef_layer_set_refresh_rate(l, hz) };
    }
}

pub fn jfn_browsers_all_closed() -> bool {
    let g = INSTANCE.lock().unwrap();
    let Some(b) = g.as_ref() else { return true };
    for l in &b.layers {
        if !unsafe { jfn_cef_layer_is_closed(*l) } {
            return false;
        }
    }
    true
}

pub fn jfn_browsers_close_all() {
    let snapshot: Vec<*mut JfnCefLayer> = INSTANCE
        .lock()
        .unwrap()
        .as_ref()
        .map(|b| b.layers.clone())
        .unwrap_or_default();
    for l in snapshot {
        unsafe { jfn_cef_layer_close_browser_force(l) };
    }
}

/// Drive an external BeginFrame on every layer. Called from the macOS
/// CADisplayLink tick at the display's real refresh rate so CEF produces
/// frames only when its compositor has invalidation.
#[cfg(target_os = "macos")]
pub fn jfn_browsers_send_external_begin_frame_all() {
    let snapshot: Vec<*mut JfnCefLayer> = INSTANCE
        .lock()
        .unwrap()
        .as_ref()
        .map(|b| b.layers.clone())
        .unwrap_or_default();
    for l in snapshot {
        unsafe { crate::client::jfn_cef_layer_send_external_begin_frame(l) };
    }
}

pub fn jfn_browsers_wait_all_closed() {
    let snapshot: Vec<*mut JfnCefLayer> = INSTANCE
        .lock()
        .unwrap()
        .as_ref()
        .map(|b| b.layers.clone())
        .unwrap_or_default();
    for l in snapshot {
        unsafe { jfn_cef_layer_wait_for_close(l) };
    }
}

fn restack(layers: &[*mut JfnCefLayer]) {
    let mut ordered: Vec<*mut c_void> = Vec::with_capacity(layers.len());
    for l in layers {
        let s = unsafe { jfn_cef_layer_get_surface(*l) };
        if !s.is_null() {
            ordered.push(s);
        }
    }
    jfn_platform_abi::get().restack(ordered.as_ptr(), ordered.len());
}
