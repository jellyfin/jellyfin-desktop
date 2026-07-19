// JfnCefLayer is an opaque internal handle; callers within this crate
// pass it back unchanged. Marking each consumer unsafe would cascade
// without adding type safety, so the lint is suppressed module-wide.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

//! Process-wide layer registry.
//!
//! Owns the Vec<*mut JfnCefLayer> (each layer is jfn_cef_layer_new'd at
//! create time and jfn_cef_layer_free'd at remove), the active-input
//! target, and the broadcast display state. Restack hands the platform a
//! window-Z-ordered list of opaque surface pointers extracted from each
//! layer.

use parking_lot::Mutex;
use std::ffi::c_char;
use std::os::raw::c_void;
use std::sync::Arc;

use jfn_platform_abi::cursor::CursorShape;

use crate::client::{Inner, JfnCefLayer};

use crate::client::{
    jfn_cef_layer_free, jfn_cef_layer_get_surface, jfn_cef_layer_inner, jfn_cef_layer_new,
    jfn_cef_layer_on_deactivated, jfn_cef_layer_resize, jfn_cef_layer_set_focus,
    jfn_cef_layer_set_injection_profile_kind, jfn_cef_layer_set_refresh_rate,
    jfn_cef_layer_set_surface, jfn_cef_set_default_frame_rate, jfn_cef_set_use_shared_textures,
};
use crate::menu_ownership::MenuOwnership;
use crate::sink_routing::{Handle, Router, Sink};

// Runs inside `route_cursor`'s `INSTANCE` critical section, so `emit` MUST NOT
// lock `INSTANCE` — the lock is not reentrant.
struct CursorSink;

impl Sink<CursorShape> for CursorSink {
    fn emit(&mut self, shape: CursorShape) {
        if let Some(p) = jfn_platform_abi::try_get() {
            p.set_cursor(shape);
        }
    }
}

struct Browsers {
    layers: Vec<*mut JfnCefLayer>,
    /// Focus history stack — top is currently active. `set_active` pushes /
    /// promotes; `remove` drops the layer from the stack so the previous
    /// top auto-regains focus.
    active_stack: Vec<*mut JfnCefLayer>,
    cursor_router: Router<CursorShape, CursorSink>,
    menu: MenuOwnership,
    /// Last size applied via [`jfn_browsers_set_size`]; used only to size
    /// layers created after the fact (`jfn_browsers_create`).
    lw: i32,
    lh: i32,
    pw: i32,
    ph: i32,
    frame_rate: i32,
}

unsafe impl Send for Browsers {}

static INSTANCE: Mutex<Option<Browsers>> = Mutex::new(None);

pub fn jfn_browsers_init(frame_rate: f64, use_shared_textures: bool) {
    let fr = if frame_rate > 0.0 {
        (frame_rate + 0.5) as i32
    } else {
        0
    };
    jfn_cef_set_default_frame_rate(fr);
    jfn_cef_set_use_shared_textures(use_shared_textures);
    *INSTANCE.lock() = Some(Browsers {
        layers: Vec::new(),
        active_stack: Vec::new(),
        cursor_router: Router::new(CursorSink),
        menu: MenuOwnership::default(),
        lw: 0,
        lh: 0,
        pw: 0,
        ph: 0,
        frame_rate: fr,
    });
    crate::bridge::install();
}

/// Tear down all remaining layers. Called once at the end of run_with_cef.
pub fn jfn_browsers_shutdown() {
    let Some(b) = INSTANCE.lock().take() else {
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

/// # Safety
/// `kind` must be a NUL-terminated UTF-8 pointer naming a registered
/// injection kind, or null.
pub unsafe fn jfn_browsers_create(kind: *const c_char) -> *mut JfnCefLayer {
    let mut g = INSTANCE.lock();
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
    let handle = b.cursor_router.add_level();
    unsafe { jfn_cef_layer_inner(layer) }.set_cursor_handle(handle);
    restack(&b.layers);
    layer
}

/// Drop `layer` from the registry. MUST be called on TID_UI — all `layers`
/// and `active_stack` mutations are TID_UI-only, which is what lets the
/// post-`drop(g)` refocus dereference `new_top` without revalidating.
pub fn jfn_browsers_remove(layer: *mut JfnCefLayer) {
    if layer.is_null() {
        return;
    }
    let mut g = INSTANCE.lock();
    let Some(b) = g.as_mut() else { return };
    let was_top = b.active_stack.last().copied() == Some(layer);
    b.active_stack.retain(|l| *l != layer);
    let new_top = b
        .active_stack
        .last()
        .copied()
        .unwrap_or(std::ptr::null_mut());
    // `cursor_handle_of(layer)` derefs the layer, so this must run before
    // `jfn_cef_layer_free(layer)` below.
    if let Some(gone) = cursor_handle_of(layer) {
        b.cursor_router.remove(gone, cursor_handle_of(new_top));
    }
    unsafe { jfn_cef_layer_on_deactivated(layer) };
    let pos = b.layers.iter().position(|l| *l == layer);
    let Some(idx) = pos else { return };
    let surface = unsafe { jfn_cef_layer_get_surface(layer) };
    b.layers.remove(idx);
    if !surface.is_null() {
        jfn_platform_abi::get().free_surface(surface);
    }
    unsafe { jfn_cef_layer_free(layer) };
    restack(&b.layers);
    drop(g);
    // Skip refocus during shutdown — every layer is mid-close and posting
    // input/focus to a dying browser hangs TID_UI before OnBeforeClose fires.
    if was_top && !new_top.is_null() && !jfn_playback::shutdown::jfn_shutting_down() {
        refocus(new_top);
    }
}

pub fn jfn_browsers_set_active(layer: *mut JfnCefLayer) {
    let mut g = INSTANCE.lock();
    let Some(b) = g.as_mut() else { return };
    let prev = b
        .active_stack
        .last()
        .copied()
        .unwrap_or(std::ptr::null_mut());
    if prev == layer {
        return;
    }
    // Promote: drop any prior occurrence, then push to top.
    b.active_stack.retain(|l| *l != layer);
    if !layer.is_null() {
        b.active_stack.push(layer);
        if let Some(h) = cursor_handle_of(layer) {
            b.cursor_router.select(h);
        }
    }
    drop(g);
    if !prev.is_null() {
        unsafe {
            jfn_cef_layer_set_focus(prev, false);
            jfn_cef_layer_on_deactivated(prev);
        }
    }
    if !layer.is_null() {
        refocus(layer);
    }
}

fn refocus(layer: *mut JfnCefLayer) {
    unsafe { jfn_cef_layer_set_focus(layer, true) };
}

fn cursor_handle_of(layer: *mut JfnCefLayer) -> Option<Handle> {
    if layer.is_null() {
        return None;
    }
    unsafe { jfn_cef_layer_inner(layer) }.cursor_handle()
}

pub(crate) fn route_cursor(handle: Handle, shape: CursorShape) {
    if let Some(b) = INSTANCE.lock().as_mut() {
        b.cursor_router.emit(handle, shape);
    }
}

pub(crate) fn jfn_browsers_menu_open() -> Option<Handle> {
    INSTANCE.lock().as_mut().and_then(|b| b.menu.open())
}

pub(crate) fn jfn_browsers_menu_resolve(h: Handle) -> bool {
    INSTANCE
        .lock()
        .as_mut()
        .map(|b| b.menu.resolve(h))
        .unwrap_or(false)
}

pub fn jfn_browsers_active() -> *mut JfnCefLayer {
    INSTANCE
        .lock()
        .as_ref()
        .and_then(|b| b.active_stack.last().copied())
        .unwrap_or(std::ptr::null_mut())
}

pub fn jfn_browsers_set_size(lw: i32, lh: i32, pw: i32, ph: i32) {
    let layers: Vec<*mut JfnCefLayer> = {
        let mut g = INSTANCE.lock();
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

pub fn jfn_browsers_set_refresh_rate(hz: f64) {
    if hz <= 0.0 {
        return;
    }
    let target = (hz + 0.5) as i32;
    let layers: Vec<*mut JfnCefLayer> = {
        let mut g = INSTANCE.lock();
        let Some(b) = g.as_mut() else { return };
        b.frame_rate = target;
        jfn_cef_set_default_frame_rate(target);
        b.layers.clone()
    };
    for l in layers {
        unsafe { jfn_cef_layer_set_refresh_rate(l, hz) };
    }
}

/// Snapshot every layer's `Arc<Inner>`, then force-close its browser. MUST
/// run on TID_UI — called from `CloseAndCollectTask::execute`. The returned
/// `Arc<Inner>` set is exactly the set whose closes were issued, safe to
/// `wait_for_close` even after the layer's BeforeClose path frees its
/// `JfnCefLayer` Box (the Arc keeps `Inner` and its `close_cv` alive).
///
/// The lock is released before `close_browser_force` runs so a synchronous
/// `OnBeforeClose` callback (which now uniformly self-removes the layer via
/// `handle_on_before_close`) can re-take the lock without deadlocking.
pub(crate) fn jfn_browsers_close_and_snapshot() -> Vec<Arc<Inner>> {
    let inners: Vec<Arc<Inner>> = {
        let mut g = INSTANCE.lock();
        let Some(b) = g.as_mut() else {
            return Vec::new();
        };
        // A browser dying mid-menu must not strand the slot.
        b.menu.reset();
        b.layers
            .iter()
            .map(|l| unsafe { jfn_cef_layer_inner(*l) })
            .collect()
    };
    // Lock released — sync `OnBeforeClose` from close_browser_force can now
    // re-take INSTANCE via `handle_on_before_close`'s auto-remove without
    // deadlocking. Closes run on the held `Arc<Inner>`s, so no raw layer
    // ptr is dereferenced after a peer layer's auto-remove frees its Box.
    for i in &inners {
        i.close_browser_force();
    }
    inners
}

/// Drive an external BeginFrame on every layer. Entry point for the
/// platform's frame driver (macOS CADisplayLink tick at the display's
/// real refresh rate) so CEF produces frames only when its compositor
/// has invalidation. Dead on platforms without external BeginFrame.
pub fn jfn_browsers_send_external_begin_frame_all() {
    let snapshot: Vec<*mut JfnCefLayer> = INSTANCE
        .lock()
        .as_ref()
        .map(|b| b.layers.clone())
        .unwrap_or_default();
    for l in snapshot {
        unsafe { crate::client::jfn_cef_layer_send_external_begin_frame(l) };
    }
}

/// Mark every live browser visible or hidden. MUST run on TID_UI — called
/// from the `SetHiddenAllTask` posted by `jfn_browsers_set_hidden_all`.
/// CEF folds `WasHidden(true)` into pausing rendering / freeing GPU
/// compositing resources and `WasHidden(false)` into a paint kick.
pub(crate) fn jfn_browsers_apply_hidden_all(hidden: bool) {
    let inners: Vec<Arc<Inner>> = {
        let g = INSTANCE.lock();
        let Some(b) = g.as_ref() else {
            return;
        };
        b.layers
            .iter()
            .map(|l| unsafe { jfn_cef_layer_inner(*l) })
            .collect()
    };
    for i in &inners {
        i.cef_was_hidden(hidden);
    }
}

/// Request a visibility change on every live browser. Thread-agnostic:
/// posts a TID_UI task that calls `WasHidden(hidden)` on each layer.
pub fn jfn_browsers_set_hidden_all(hidden: bool) {
    crate::client::jfn_cef_post_set_hidden_all(hidden);
}

/// Force-close every layer's browser and block until each `OnBeforeClose`
/// has fired. Thread-agnostic: callable from any non-TID_UI thread (the
/// shutdown manager). Posts a single snapshot-and-close task onto TID_UI,
/// then waits on every `Arc<Inner>` that task closed — no second-snapshot
/// race window, and no UAF when a layer's `before_close_callback`
/// self-removes its `JfnCefLayer` Box mid-drain (the about layer does).
pub fn jfn_browsers_close_all_blocking() {
    let (tx, rx) = std::sync::mpsc::sync_channel::<Vec<Arc<Inner>>>(1);
    crate::client::jfn_cef_post_close_and_collect(tx);
    let inners = match rx.recv() {
        Ok(inners) => inners,
        Err(e) => {
            eprintln!("[cef] close-all wait set never arrived: {e}");
            return;
        }
    };
    for i in inners {
        i.wait_for_close();
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
    jfn_platform_abi::get().restack(&ordered);
}
