//! Thin FFI wrappers around `Inner`. Each function looks up the `Arc<Inner>`
//! from a raw `JfnCefLayer*` and forwards to the corresponding `Inner`
//! method. Caller-visible state lives in `Inner`; this layer holds no logic.

use cef::{ImplBrowser, ImplBrowserHost, KeyEvent, MouseButtonType, MouseEvent, sys};
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::Arc;
use std::sync::atomic::Ordering;

use super::{DEFAULT_FRAME_RATE, Inner, JfnCefLayer, USE_SHARED_TEXTURES};

unsafe fn arc(h: *const JfnCefLayer) -> Arc<Inner> {
    Arc::clone(unsafe { &(*h).inner })
}

pub(crate) fn read_utf8(p: *const c_char, len: usize) -> String {
    if p.is_null() || len == 0 {
        return String::new();
    }
    let slice = unsafe { std::slice::from_raw_parts(p as *const u8, len) };
    String::from_utf8_lossy(slice).into_owned()
}

pub(crate) fn jfn_cef_layer_new() -> *mut JfnCefLayer {
    let layer = Box::into_raw(Box::new(JfnCefLayer {
        inner: Inner::new(),
    }));
    // Stash the raw handle so `handle_on_before_close` can auto-remove this
    // layer from the `Browsers` registry without per-business wiring.
    unsafe { (*layer).inner.set_layer_ptr(layer) };
    layer
}

pub(crate) unsafe fn jfn_cef_layer_free(h: *mut JfnCefLayer) {
    if h.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(h) });
}

pub(crate) unsafe fn jfn_cef_layer_set_name(h: *const JfnCefLayer, s: *const c_char) {
    let inner = unsafe { arc(h) };
    let new = if s.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(s) }.to_string_lossy().into_owned()
    };
    *inner.name.lock() = new;
}

/// Clone the layer's `Arc<Inner>`. The shutdown drain gate takes these under
/// the `INSTANCE` lock so it can wait on a layer's close after the
/// `JfnCefLayer` Box is freed by a self-removing `before_close_callback` (e.g.
/// the about layer): the owned `Arc` keeps `Inner` (and its `close_cv`) alive.
///
/// # Safety
/// `h` must be a live `JfnCefLayer` handle returned by `jfn_cef_layer_new`.
pub(crate) unsafe fn jfn_cef_layer_inner(h: *const JfnCefLayer) -> Arc<Inner> {
    unsafe { arc(h) }
}

/// # Safety
/// `h` must be a live `JfnCefLayer` handle returned by `jfn_cef_layer_new`.
pub unsafe fn jfn_cef_layer_wait_for_load(h: *const JfnCefLayer) {
    let l = unsafe { arc(h) };
    let mut g = l.load_mtx.lock();
    while !l.loaded.load(Ordering::Acquire) {
        l.load_cv.wait(&mut g);
    }
}

/// Process-wide default frame rate (set once at startup via the Browsers
/// ctor). Consumed by `Inner::cef_create_browser` when building
/// `CefBrowserSettings.windowless_frame_rate`. Zero values are ignored.
pub(crate) fn jfn_cef_set_default_frame_rate(hz: c_int) {
    if hz > 0 {
        DEFAULT_FRAME_RATE.store(hz, Ordering::Release);
    }
}

pub(crate) fn jfn_cef_set_use_shared_textures(enable: bool) {
    USE_SHARED_TEXTURES.store(enable, Ordering::Release);
}

/// Set the injection-profile kind for this layer ("web" / "overlay" /
/// "about"). The DictionaryValue is built lazily at browser-create time.
pub(crate) unsafe fn jfn_cef_layer_set_injection_profile_kind(
    h: *const JfnCefLayer,
    kind_utf8: *const c_char,
    len: usize,
) {
    let inner = unsafe { arc(h) };
    let s = read_utf8(kind_utf8, len);
    *inner.injection_kind.lock() = s;
}

pub(crate) unsafe fn jfn_cef_layer_can_go_back(h: *const JfnCefLayer) -> bool {
    let inner = unsafe { arc(h) };
    inner
        .browser_clone()
        .map(|b| b.can_go_back() == 1)
        .unwrap_or(false)
}

pub(crate) unsafe fn jfn_cef_layer_can_go_forward(h: *const JfnCefLayer) -> bool {
    let inner = unsafe { arc(h) };
    inner
        .browser_clone()
        .map(|b| b.can_go_forward() == 1)
        .unwrap_or(false)
}

pub(crate) unsafe fn jfn_cef_layer_go_back(h: *const JfnCefLayer) {
    if let Some(b) = unsafe { arc(h) }.browser_clone() {
        b.go_back();
    }
}

pub(crate) unsafe fn jfn_cef_layer_go_forward(h: *const JfnCefLayer) {
    if let Some(b) = unsafe { arc(h) }.browser_clone() {
        b.go_forward();
    }
}

pub(crate) unsafe fn jfn_cef_layer_set_focus(h: *const JfnCefLayer, focus: bool) {
    if let Some(host) = unsafe { arc(h) }.host() {
        host.set_focus(if focus { 1 } else { 0 });
    }
}

#[allow(clippy::too_many_arguments)] // mirrors CEF's KeyEvent layout 1:1
pub(crate) unsafe fn jfn_cef_layer_send_key_event(
    h: *const JfnCefLayer,
    type_: c_int,
    modifiers: u32,
    windows_key_code: c_int,
    native_key_code: c_int,
    is_system_key: bool,
    character: u16,
    unmodified_character: u16,
) {
    let Some(host) = unsafe { arc(h) }.host() else {
        return;
    };
    let raw_type: sys::cef_key_event_type_t = unsafe { std::mem::transmute(type_ as u32) };
    let ev = KeyEvent {
        type_: raw_type.into(),
        modifiers,
        windows_key_code,
        native_key_code,
        is_system_key: if is_system_key { 1 } else { 0 },
        character,
        unmodified_character,
        ..KeyEvent::default()
    };
    host.send_key_event(Some(&ev));
}

pub(crate) unsafe fn jfn_cef_layer_send_mouse_click(
    h: *const JfnCefLayer,
    x: c_int,
    y: c_int,
    modifiers: u32,
    button: c_int,
    mouse_up: bool,
    click_count: c_int,
) {
    let Some(host) = unsafe { arc(h) }.host() else {
        return;
    };
    let me = MouseEvent { x, y, modifiers };
    let raw_btn: sys::cef_mouse_button_type_t = unsafe { std::mem::transmute(button as u32) };
    host.send_mouse_click_event(
        Some(&me),
        MouseButtonType::from(raw_btn),
        if mouse_up { 1 } else { 0 },
        click_count,
    );
}

pub(crate) unsafe fn jfn_cef_layer_send_mouse_move(
    h: *const JfnCefLayer,
    x: c_int,
    y: c_int,
    modifiers: u32,
    leave: bool,
) {
    let Some(host) = unsafe { arc(h) }.host() else {
        return;
    };
    let me = MouseEvent { x, y, modifiers };
    host.send_mouse_move_event(Some(&me), if leave { 1 } else { 0 });
}

pub(crate) unsafe fn jfn_cef_layer_send_mouse_wheel(
    h: *const JfnCefLayer,
    x: c_int,
    y: c_int,
    modifiers: u32,
    dx: c_int,
    dy: c_int,
) {
    let Some(host) = unsafe { arc(h) }.host() else {
        return;
    };
    let me = MouseEvent { x, y, modifiers };
    host.send_mouse_wheel_event(Some(&me), dx, dy);
}

pub(crate) unsafe fn jfn_cef_layer_set_surface(h: *const JfnCefLayer, s: *mut c_void) {
    *unsafe { arc(h) }.surface.lock() = s;
}

pub(crate) unsafe fn jfn_cef_layer_get_surface(h: *const JfnCefLayer) -> *mut c_void {
    unsafe { arc(h) }.surface_ptr()
}

pub(crate) unsafe fn jfn_cef_layer_resize(
    h: *const JfnCefLayer,
    w: c_int,
    height: c_int,
    pw: c_int,
    ph: c_int,
) {
    unsafe { arc(h) }.resize(w, height, pw, ph);
}

pub(crate) unsafe fn jfn_cef_layer_set_refresh_rate(h: *const JfnCefLayer, hz: f64) {
    unsafe { arc(h) }.set_refresh_rate(hz);
}

/// # Safety
/// `h` must be a live `JfnCefLayer` handle; `url_utf8` must reference
/// `len` valid UTF-8 bytes (not necessarily NUL-terminated).
pub unsafe fn jfn_cef_layer_create(h: *const JfnCefLayer, url_utf8: *const c_char, len: usize) {
    let url = read_utf8(url_utf8, len);
    unsafe { arc(h) }.create(&url);
}

pub(crate) unsafe fn jfn_cef_layer_reset(h: *const JfnCefLayer) {
    unsafe { arc(h) }.reset();
}

pub(crate) unsafe fn jfn_cef_layer_load_url(
    h: *const JfnCefLayer,
    url_utf8: *const c_char,
    len: usize,
) {
    let url = read_utf8(url_utf8, len);
    unsafe { arc(h) }.load_url(&url);
}

pub(crate) unsafe fn jfn_cef_layer_exec_js(
    h: *const JfnCefLayer,
    js_utf8: *const c_char,
    len: usize,
) {
    let js = read_utf8(js_utf8, len);
    unsafe { arc(h) }.exec_js(&js);
}

#[cfg(target_os = "macos")]
pub(crate) unsafe fn jfn_cef_layer_send_external_begin_frame(h: *const JfnCefLayer) {
    unsafe { arc(h) }.send_external_begin_frame();
}

pub(crate) unsafe fn jfn_cef_layer_undo(h: *const JfnCefLayer) {
    unsafe { arc(h) }.frame_undo();
}
pub(crate) unsafe fn jfn_cef_layer_redo(h: *const JfnCefLayer) {
    unsafe { arc(h) }.frame_redo();
}
pub(crate) unsafe fn jfn_cef_layer_cut(h: *const JfnCefLayer) {
    unsafe { arc(h) }.frame_cut();
}
pub(crate) unsafe fn jfn_cef_layer_copy(h: *const JfnCefLayer) {
    unsafe { arc(h) }.frame_copy();
}
pub(crate) unsafe fn jfn_cef_layer_paste(h: *const JfnCefLayer) {
    unsafe { arc(h) }.frame_paste();
}
pub(crate) unsafe fn jfn_cef_layer_select_all(h: *const JfnCefLayer) {
    unsafe { arc(h) }.frame_select_all();
}

pub(crate) unsafe fn jfn_cef_layer_set_visible(h: *const JfnCefLayer, visible: bool) {
    unsafe { arc(h) }.set_visible(visible);
}

pub(crate) unsafe fn jfn_cef_layer_on_deactivated(h: *const JfnCefLayer) {
    unsafe { arc(h) }.on_deactivated();
}
