//! D3D11 + DirectComposition per-surface compositor.
//!
//! Owns all D3D / DComp / per-surface state. The platform module keeps only
//! HWND, cached scale, fullscreen bookkeeping, the WndProc hook, and the
//! input thread; it calls into this module via the narrow `jfn_win_*`
//! accessors at the bottom of the file to initialize, tear down, and drive
//! the transition-locked routines.

#![allow(non_snake_case)]

use parking_lot::Mutex;
use std::ffi::{c_int, c_void};

use windows::Win32::Foundation::{HANDLE, HWND};
use windows::Win32::Graphics::Direct3D::{
    D3D_DRIVER_TYPE_HARDWARE, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_BIND_SHADER_RESOURCE, D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_SDK_VERSION,
    D3D11_SUBRESOURCE_DATA, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT, D3D11CreateDevice,
    ID3D11Device, ID3D11Device1, ID3D11DeviceContext, ID3D11Texture2D,
};
use windows::Win32::Graphics::DirectComposition::{
    DCompositionCreateDevice, IDCompositionDevice, IDCompositionTarget, IDCompositionVisual,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_ALPHA_MODE_PREMULTIPLIED, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::{
    DXGI_PRESENT, DXGI_SWAP_CHAIN_DESC1, DXGI_SWAP_CHAIN_FLAG, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
    DXGI_USAGE_RENDER_TARGET_OUTPUT, IDXGIAdapter, IDXGIDevice, IDXGIFactory2, IDXGISwapChain1,
};
use windows_core::Interface;

use jfn_compositor_core::stack::SurfaceStack;
use jfn_compositor_core::transition::{PresentDecision, TransitionGate};
use jfn_platform_abi::JfnRect;

// =====================================================================
// Per-surface state. Stored as `Box<Surface>` and exposed across the C
// ABI as the opaque `*mut c_void` PlatformSurface pointer.
// =====================================================================

pub(crate) struct Surface {
    swap_chain: Option<IDXGISwapChain1>,
    visual: Option<IDCompositionVisual>,
    sw: i32,
    sh: i32,
    visible: bool,
    in_tree: bool,

    popup_visual: Option<IDCompositionVisual>,
    popup_swap_chain: Option<IDXGISwapChain1>,
    popup_sw: i32,
    popup_sh: i32,
    popup_visible: bool,
}

impl Surface {
    fn new() -> Self {
        Self {
            swap_chain: None,
            visual: None,
            sw: 0,
            sh: 0,
            visible: true,
            in_tree: false,
            popup_visual: None,
            popup_swap_chain: None,
            popup_sw: 0,
            popup_sh: 0,
            popup_visible: false,
        }
    }
}

// =====================================================================
// Shared compositor state. Mutex order: any caller that wants to touch
// `State.surfaces` / per-surface visuals must hold STATE.lock(). Equivalent
// of the C++ `g_win.surface_mtx`.
// =====================================================================

struct CompositorDevices {
    d3d_device: ID3D11Device1,
    d3d_context: ID3D11DeviceContext,
    dxgi_factory: IDXGIFactory2,
    dcomp_device: IDCompositionDevice,
    // Held only to keep the composition target (and its bound root) alive for
    // the lifetime of the compositor; never read after construction.
    #[allow(dead_code)]
    dcomp_target: IDCompositionTarget,
    dcomp_root: IDCompositionVisual,
}

// COM interfaces are Send+Sync-by-COM-spec for the apartment we created them
// in (MTA via D3D11CreateDevice). We serialize all access under STATE's
// Mutex so the apartment-confinement isn't violated.
unsafe impl Send for CompositorDevices {}
unsafe impl Send for Surface {}

struct State {
    devices: Option<CompositorDevices>,
    // Surface registry (live + stack order + main) shared with macOS via
    // jfn-compositor-core.
    surfaces: SurfaceStack<*mut Surface>,
    // Fullscreen/resize transition gate (was G_TRANSITIONING + expected_w/h +
    // transition_pw/ph), kept inside this single STATE lock.
    gate: TransitionGate,
    mpv_pw: i32,
    mpv_ph: i32,
    pending_lw: i32,
    pending_lh: i32,
}

unsafe impl Send for State {}

static STATE: Mutex<State> = Mutex::new(State {
    devices: None,
    surfaces: SurfaceStack::new(),
    gate: TransitionGate::new(),
    mpv_pw: 0,
    mpv_ph: 0,
    pending_lw: 0,
    pending_lh: 0,
});

/// Whether the main surface is currently gated. Takes the STATE lock, so
/// callers must not already hold it (none do).
pub(crate) fn gate_in_transition() -> bool {
    STATE.lock().gate.in_transition()
}

// =====================================================================
// Compositor init/cleanup — called from win_init/win_cleanup (C++).
// =====================================================================

/// Build D3D11 + DXGI + DComp devices and the root visual. Returns false
/// on failure with the partial state torn down.
pub fn jfn_win_init_compositor(hwnd: *mut c_void) -> bool {
    let hwnd = HWND(hwnd);
    let mut st = STATE.lock();
    if st.devices.is_some() {
        return true;
    }
    match init_devices(hwnd) {
        Ok(d) => {
            st.devices = Some(d);
            true
        }
        Err(e) => {
            tracing::error!(target: "platform", "compositor init failed: {e:?}");
            false
        }
    }
}

fn init_devices(hwnd: HWND) -> windows_core::Result<CompositorDevices> {
    unsafe {
        // D3D11 device + immediate context.
        let levels = [D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0];
        let mut base_device: Option<ID3D11Device> = None;
        let mut context: Option<ID3D11DeviceContext> = None;
        D3D11CreateDevice(
            None::<&windows::Win32::Graphics::Dxgi::IDXGIAdapter>,
            D3D_DRIVER_TYPE_HARDWARE,
            windows::Win32::Foundation::HMODULE(std::ptr::null_mut()),
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            Some(&levels),
            D3D11_SDK_VERSION,
            Some(&mut base_device),
            None,
            Some(&mut context),
        )?;
        let base_device = base_device.ok_or(windows_core::Error::from_thread())?;
        let context = context.ok_or(windows_core::Error::from_thread())?;
        let d3d_device: ID3D11Device1 = base_device.cast()?;

        // DXGI factory via the device's adapter.
        let dxgi_device: IDXGIDevice = d3d_device.cast()?;
        let adapter: IDXGIAdapter = dxgi_device.GetAdapter()?;
        let dxgi_factory: IDXGIFactory2 = adapter.GetParent()?;

        // DComp device on the DXGI device.
        let dcomp_device: IDCompositionDevice = DCompositionCreateDevice(&dxgi_device)?;
        let dcomp_target = dcomp_device.CreateTargetForHwnd(hwnd, false)?;
        let dcomp_root = dcomp_device.CreateVisual()?;
        dcomp_target.SetRoot(&dcomp_root)?;
        dcomp_device.Commit()?;

        Ok(CompositorDevices {
            d3d_device,
            d3d_context: context,
            dxgi_factory,
            dcomp_device,
            dcomp_target,
            dcomp_root,
        })
    }
}

/// Release all surfaces + devices. Called from win_cleanup (C++) after the
/// WndProc hook is unhooked and the input thread is joined.
pub fn jfn_win_cleanup_compositor() {
    let mut st = STATE.lock();
    // Free any remaining surfaces. Browsers should normally free them
    // first, but be defensive.
    let live: Vec<*mut Surface> = st.surfaces.take_live();
    for ptr in live {
        if !ptr.is_null() {
            // SAFETY: we own these pointers via Box::into_raw.
            unsafe {
                let mut s = Box::from_raw(ptr);
                detach_surface(&mut s, st.devices.as_ref());
                drop(s);
            }
        }
    }
    st.devices = None;
}

fn detach_surface(s: &mut Surface, devices: Option<&CompositorDevices>) {
    unsafe {
        if let Some(pv) = s.popup_visual.as_ref() {
            if let Some(v) = s.visual.as_ref() {
                let _ = v.RemoveVisual(pv);
            }
            let _ = pv.SetContent(None::<&windows_core::IUnknown>);
        }
        s.popup_visual = None;
        s.popup_swap_chain = None;
        if let Some(v) = s.visual.as_ref() {
            if s.in_tree {
                if let Some(d) = devices {
                    let _ = d.dcomp_root.RemoveVisual(v);
                }
            }
            let _ = v.SetContent(None::<&windows_core::IUnknown>);
        }
        s.visual = None;
        s.swap_chain = None;
    }
}

// =====================================================================
// Swap-chain helpers (locked).
// =====================================================================

fn create_swap_chain(
    devices: &CompositorDevices,
    width: i32,
    height: i32,
) -> Option<IDXGISwapChain1> {
    if width <= 0 || height <= 0 {
        return None;
    }
    let desc = DXGI_SWAP_CHAIN_DESC1 {
        Width: width as u32,
        Height: height as u32,
        Format: DXGI_FORMAT_B8G8R8A8_UNORM,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        BufferUsage: DXGI_USAGE_RENDER_TARGET_OUTPUT,
        BufferCount: 2,
        SwapEffect: DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
        AlphaMode: DXGI_ALPHA_MODE_PREMULTIPLIED,
        ..Default::default()
    };
    unsafe {
        match devices.dxgi_factory.CreateSwapChainForComposition(
            &devices.d3d_device,
            &desc,
            None::<&windows::Win32::Graphics::Dxgi::IDXGIOutput>,
        ) {
            Ok(sc) => Some(sc),
            Err(e) => {
                tracing::error!(target: "platform", "CreateSwapChainForComposition failed: {e:?}");
                None
            }
        }
    }
}

/// Ensure `sc` is sized (w,h); resize in place if possible, otherwise
/// recreate and rebind to `visual`. Updates `sw`/`sh` on success.
fn ensure_swap_chain(
    devices: &CompositorDevices,
    sc: &mut Option<IDXGISwapChain1>,
    sw: &mut i32,
    sh: &mut i32,
    visual: &IDCompositionVisual,
    w: i32,
    h: i32,
) {
    if w <= 0 || h <= 0 {
        return;
    }
    if let Some(existing) = sc.as_ref() {
        if *sw == w && *sh == h {
            return;
        }
        let resize = unsafe {
            existing.ResizeBuffers(
                2,
                w as u32,
                h as u32,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_SWAP_CHAIN_FLAG(0),
            )
        };
        if resize.is_ok() {
            *sw = w;
            *sh = h;
            return;
        }
        unsafe {
            let _ = visual.SetContent(None::<&windows_core::IUnknown>);
        }
        *sc = None;
    }

    if let Some(new_sc) = create_swap_chain(devices, w, h) {
        unsafe {
            let _ = visual.SetContent(&new_sc);
        }
        *sc = Some(new_sc);
        *sw = w;
        *sh = h;
    }
}

fn present_to_swap_chain(devices: &CompositorDevices, sc: &IDXGISwapChain1, src: &ID3D11Texture2D) {
    unsafe {
        match sc.GetBuffer::<ID3D11Texture2D>(0) {
            Ok(bb) => {
                devices.d3d_context.CopyResource(&bb, src);
                let _ = sc.Present(0, DXGI_PRESENT(0));
                let _ = devices.dcomp_device.Commit();
            }
            Err(e) => tracing::error!(target: "platform", "GetBuffer failed: {e:?}"),
        }
    }
}

// =====================================================================
// Surface lifecycle + stacking.
// =====================================================================

pub fn win_alloc_surface() -> *mut c_void {
    let mut st = STATE.lock();
    if st.devices.is_none() {
        return std::ptr::null_mut();
    }

    let mut s = Box::new(Surface::new());
    {
        let devices = st.devices.as_ref().unwrap();
        unsafe {
            let visual = match devices.dcomp_device.CreateVisual() {
                Ok(v) => v,
                Err(e) => {
                    tracing::error!(target: "platform", "CreateVisual failed: {e:?}");
                    return std::ptr::null_mut();
                }
            };

            let popup = devices.dcomp_device.CreateVisual().ok();
            if let Some(pv) = popup.as_ref() {
                let _ = visual.AddVisual(pv, true, None::<&IDCompositionVisual>);
            } else {
                tracing::error!(target: "platform", "CreateVisual(popup) failed");
            }

            match devices
                .dcomp_root
                .AddVisual(&visual, true, None::<&IDCompositionVisual>)
            {
                Ok(()) => s.in_tree = true,
                Err(e) => tracing::error!(target: "platform", "AddVisual failed: {e:?}"),
            }

            s.visual = Some(visual);
            s.popup_visual = popup;

            let _ = devices.dcomp_device.Commit();
        }
    }

    let ptr = Box::into_raw(s);
    st.surfaces.add_live(ptr);
    ptr as *mut c_void
}

pub fn win_free_surface(s: *mut c_void) {
    if s.is_null() {
        return;
    }
    let p = s as *mut Surface;

    let mut st = STATE.lock();
    st.surfaces.remove(p);

    let devices = st.devices.as_ref();
    unsafe {
        let mut s_box = Box::from_raw(p);
        detach_surface(&mut s_box, devices);
        if let Some(d) = devices {
            let _ = d.dcomp_device.Commit();
        }
        drop(s_box);
    }
}

/// Rebuild the child-list under `dcomp_root` in `ordered` order
/// (bottom -> top). Popup visuals stay nested under their owning surface,
/// so they're not in this list.
pub fn win_restack(ordered: *const *mut c_void, n: usize) {
    let mut st = STATE.lock();
    if st.devices.is_none() {
        return;
    }

    // Snapshot live pointers so we can detach without holding a borrow of
    // `st` while we mutate per-surface state.
    let live_ptrs: Vec<*mut Surface> = st.surfaces.live().to_vec();
    {
        let dcomp_root = st.devices.as_ref().unwrap().dcomp_root.clone();
        unsafe {
            for ptr in &live_ptrs {
                if ptr.is_null() {
                    continue;
                }
                let s = &mut **ptr;
                if let Some(v) = s.visual.as_ref() {
                    if s.in_tree {
                        let _ = dcomp_root.RemoveVisual(v);
                        s.in_tree = false;
                    }
                }
            }
        }
    }

    st.surfaces.clear_stack();
    let mut prev_visual: Option<IDCompositionVisual> = None;
    {
        let dcomp_root = st.devices.as_ref().unwrap().dcomp_root.clone();
        unsafe {
            for i in 0..n {
                let ptr = *ordered.add(i) as *mut Surface;
                if ptr.is_null() {
                    continue;
                }
                let s = &mut *ptr;
                let visual = match s.visual.as_ref() {
                    Some(v) => v.clone(),
                    None => continue,
                };
                let hr = if let Some(prev) = prev_visual.as_ref() {
                    dcomp_root.AddVisual(&visual, true, prev)
                } else {
                    dcomp_root.AddVisual(&visual, false, None::<&IDCompositionVisual>)
                };
                if let Err(e) = hr {
                    tracing::error!(target: "platform", "restack AddVisual failed: {e:?}");
                    continue;
                }
                s.in_tree = true;
                st.surfaces.push_stack(ptr);
                prev_visual = Some(visual);
            }
        }
    }
    st.surfaces.set_main_to_stack_first();
    unsafe {
        let _ = st.devices.as_ref().unwrap().dcomp_device.Commit();
    }
}

// =====================================================================
// Per-frame presentation.
// =====================================================================

pub fn win_surface_present(s: *mut c_void, raw_info: *const c_void) -> bool {
    if s.is_null() || raw_info.is_null() {
        return false;
    }
    let info = unsafe { &*(raw_info as *const cef::sys::_cef_accelerated_paint_info_t) };
    let handle = info.shared_texture_handle;
    if handle.is_null() {
        return false;
    }

    let mut st = STATE.lock();
    if st.devices.is_none() {
        return false;
    }
    let src: ID3D11Texture2D = unsafe {
        match st
            .devices
            .as_ref()
            .unwrap()
            .d3d_device
            .OpenSharedResource1::<ID3D11Texture2D>(HANDLE(handle))
        {
            Ok(t) => t,
            Err(e) => {
                tracing::error!(target: "platform", "OpenSharedResource1 failed: {e:?}");
                return false;
            }
        }
    };

    let mut td = D3D11_TEXTURE2D_DESC::default();
    unsafe {
        src.GetDesc(&mut td);
    }
    let w = td.Width as i32;
    let h = td.Height as i32;

    let p = s as *mut Surface;
    let is_main = st.surfaces.is_main(p);

    // Transition logic applies only to the bottom-most ("main") surface.
    if is_main {
        match st.gate.main_present_decision((w, h)) {
            PresentDecision::Reject => return false,
            PresentDecision::EndTransitionThenPresent => {
                // The gate cleared the transition flags; clear the
                // (write-only) pending logical size too, matching the rest
                // of end_transition_locked.
                st.pending_lw = 0;
                st.pending_lh = 0;
            }
            PresentDecision::Present => {}
        }
    }

    if is_main && st.mpv_pw > 0 && (w > st.mpv_pw + 2 || h > st.mpv_ph + 2) {
        return false;
    }

    let surf = unsafe { &mut *p };
    if !surf.visible {
        return false;
    }
    let visual = match surf.visual.as_ref() {
        Some(v) => v.clone(),
        None => return false,
    };

    let devices = st.devices.as_ref().unwrap();
    ensure_swap_chain(
        devices,
        &mut surf.swap_chain,
        &mut surf.sw,
        &mut surf.sh,
        &visual,
        w,
        h,
    );
    let sc = match surf.swap_chain.as_ref() {
        Some(sc) => sc.clone(),
        None => return false,
    };
    present_to_swap_chain(devices, &sc, &src);
    true
}

/// Software fallback: Windows is shared-textures-only in practice.
/// No-op to match prior overlay/about behavior.
pub fn win_surface_present_software(
    _s: *mut c_void,
    _dirty: *const JfnRect,
    _dirty_len: usize,
    _buffer: *const c_void,
    _w: c_int,
    _h: c_int,
) -> bool {
    false
}

pub fn win_surface_resize(s: *mut c_void, _lw: c_int, _lh: c_int, pw: c_int, ph: c_int) {
    if s.is_null() || pw <= 0 || ph <= 0 {
        return;
    }
    let st = STATE.lock();
    let devices = match st.devices.as_ref() {
        Some(d) => d,
        None => return,
    };
    let surf = unsafe { &mut *(s as *mut Surface) };
    // Only adjust the swap chain if it already exists — matches the prior
    // overlay/about semantics that avoid forcing a stale physical size
    // between a window resize and the next CEF paint. (ensure_swap_chain
    // rebinds at present time.)
    if surf.swap_chain.is_none() {
        return;
    }
    let visual = match surf.visual.as_ref() {
        Some(v) => v.clone(),
        None => return,
    };
    ensure_swap_chain(
        devices,
        &mut surf.swap_chain,
        &mut surf.sw,
        &mut surf.sh,
        &visual,
        pw,
        ph,
    );
    unsafe {
        let _ = devices.dcomp_device.Commit();
    }
}

pub fn win_surface_set_visible(s: *mut c_void, visible: bool) {
    if s.is_null() {
        return;
    }
    let st = STATE.lock();
    let devices = match st.devices.as_ref() {
        Some(d) => d,
        None => return,
    };
    let surf = unsafe { &mut *(s as *mut Surface) };
    if surf.visible == visible {
        return;
    }
    surf.visible = visible;
    let visual = match surf.visual.as_ref() {
        Some(v) => v.clone(),
        None => return,
    };
    if !visible {
        // Detach content and drop the swap chain so we don't display a
        // stale frame when the surface is shown again at a different size.
        unsafe {
            let _ = visual.SetContent(None::<&windows_core::IUnknown>);
        }
        surf.swap_chain = None;
        surf.sw = 0;
        surf.sh = 0;
    }
    // visible=true: content rebinds on next ensure_swap_chain via present.
    unsafe {
        let _ = devices.dcomp_device.Commit();
    }
}

// =====================================================================
// Transition state.
// =====================================================================

fn begin_transition_locked(st: &mut State) {
    // Capture the pre-resize physical size; the present path ends the
    // transition once a frame arrives at a different size.
    st.gate.begin_capturing((st.mpv_pw, st.mpv_ph));
    st.pending_lw = 0;
    st.pending_lh = 0;

    // Detach main surface's content to avoid stale frames while resizing.
    let Some(p) = st.surfaces.main() else {
        return;
    };
    let devices = match st.devices.as_ref() {
        Some(d) => d,
        None => return,
    };
    unsafe {
        let s = &mut *p;
        if let Some(v) = s.visual.as_ref() {
            let _ = v.SetContent(None::<&windows_core::IUnknown>);
        }
        s.swap_chain = None;
        s.sw = 0;
        s.sh = 0;
        let _ = devices.dcomp_device.Commit();
    }
}

fn end_transition_locked(st: &mut State) {
    st.gate.end();
    st.pending_lw = 0;
    st.pending_lh = 0;
}

/// Called by `win_begin_transition` (in lib.rs) — replaces the old
/// `win_begin_transition_impl` C++ helper. Takes STATE lock then runs
/// the locked routine.
pub fn jfn_win_begin_transition_locked() {
    let mut st = STATE.lock();
    begin_transition_locked(&mut st);
}

pub fn win_end_transition() {
    let mut st = STATE.lock();
    end_transition_locked(&mut st);
}

pub fn win_set_expected_size(w: c_int, h: c_int) {
    STATE.lock().gate.set_expected((w, h));
}

// =====================================================================
// Accessors used by C++ WndProc / fullscreen helpers.
// =====================================================================

/// Called from the WndProc on WM_SIZE: stores mpv's current physical size
/// (used by oversized-buffer rejection), records the logical size while a
/// transition is in progress, and ends that transition once the window has
/// settled at its new size. `force_end` ends it even if the physical size is
/// unchanged (a fullscreen-style edge that didn't alter the client size).
pub fn jfn_win_update_surface_size(lw: c_int, lh: c_int, pw: c_int, ph: c_int, force_end: bool) {
    let mut st = STATE.lock();
    if st.gate.in_transition() {
        st.pending_lw = lw;
        st.pending_lh = lh;
        // The captured size is the pre-resize size, so a differing physical
        // size means the resize we were holding frames for has landed — end
        // the transition and let the OSD present again. `force_end` covers a
        // settled fullscreen edge whose physical size didn't change.
        if force_end || st.gate.captured() != Some((pw, ph)) {
            end_transition_locked(&mut st);
        }
    }
    st.mpv_pw = pw;
    st.mpv_ph = ph;
}

/// Called from C++ WndProc on WM_SIZE to run begin_transition under the
/// state lock (matches the old win_begin_transition_locked behavior).
pub fn jfn_win_wndproc_begin_transition_locked() {
    let mut st = STATE.lock();
    begin_transition_locked(&mut st);
}

pub fn jfn_win_wndproc_end_transition_locked() {
    let mut st = STATE.lock();
    end_transition_locked(&mut st);
}

// =====================================================================
// Popup helpers.
// =====================================================================

pub fn win_popup_show(s: *mut c_void, x: c_int, y: c_int) {
    if s.is_null() {
        return;
    }
    let _st = STATE.lock();
    let surf = unsafe { &mut *(s as *mut Surface) };
    surf.popup_visible = true;
    if let Some(pv) = surf.popup_visual.as_ref() {
        let scale = crate::platform::win_get_scale();
        unsafe {
            let _ = pv.SetOffsetX2(x as f32 * scale);
            let _ = pv.SetOffsetY2(y as f32 * scale);
        }
    }
}

pub fn win_popup_hide(s: *mut c_void) {
    if s.is_null() {
        return;
    }
    let st = STATE.lock();
    let surf = unsafe { &mut *(s as *mut Surface) };
    surf.popup_visible = false;
    let pv = match surf.popup_visual.as_ref() {
        Some(v) => v.clone(),
        None => return,
    };
    unsafe {
        let _ = pv.SetContent(None::<&windows_core::IUnknown>);
    }
    surf.popup_swap_chain = None;
    surf.popup_sw = 0;
    surf.popup_sh = 0;
    if let Some(d) = st.devices.as_ref() {
        unsafe {
            let _ = d.dcomp_device.Commit();
        }
    }
}

pub fn win_popup_present(s: *mut c_void, raw_info: *const c_void, _lw: c_int, _lh: c_int) {
    if s.is_null() || raw_info.is_null() {
        return;
    }
    let info = unsafe { &*(raw_info as *const cef::sys::_cef_accelerated_paint_info_t) };
    let handle = info.shared_texture_handle;
    if handle.is_null() {
        return;
    }
    let st = STATE.lock();
    let devices = match st.devices.as_ref() {
        Some(d) => d,
        None => return,
    };
    let src: ID3D11Texture2D = unsafe {
        match devices
            .d3d_device
            .OpenSharedResource1::<ID3D11Texture2D>(HANDLE(handle))
        {
            Ok(t) => t,
            Err(_) => return,
        }
    };
    let mut td = D3D11_TEXTURE2D_DESC::default();
    unsafe {
        src.GetDesc(&mut td);
    }
    let w = td.Width as i32;
    let h = td.Height as i32;

    let surf = unsafe { &mut *(s as *mut Surface) };
    if !surf.popup_visible {
        return;
    }
    let pv = match surf.popup_visual.as_ref() {
        Some(v) => v.clone(),
        None => return,
    };
    ensure_swap_chain(
        devices,
        &mut surf.popup_swap_chain,
        &mut surf.popup_sw,
        &mut surf.popup_sh,
        &pv,
        w,
        h,
    );
    let sc = match surf.popup_swap_chain.as_ref() {
        Some(sc) => sc.clone(),
        None => return,
    };
    present_to_swap_chain(devices, &sc, &src);
}

pub fn win_popup_present_software(
    s: *mut c_void,
    buffer: *const c_void,
    pw: c_int,
    ph: c_int,
    _lw: c_int,
    _lh: c_int,
) {
    if s.is_null() || buffer.is_null() || pw <= 0 || ph <= 0 {
        return;
    }
    let st = STATE.lock();
    let devices = match st.devices.as_ref() {
        Some(d) => d,
        None => return,
    };
    let surf = unsafe { &mut *(s as *mut Surface) };
    if !surf.popup_visible {
        return;
    }
    let pv = match surf.popup_visual.as_ref() {
        Some(v) => v.clone(),
        None => return,
    };

    let desc = D3D11_TEXTURE2D_DESC {
        Width: pw as u32,
        Height: ph as u32,
        MipLevels: 1,
        ArraySize: 1,
        Format: DXGI_FORMAT_B8G8R8A8_UNORM,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        Usage: D3D11_USAGE_DEFAULT,
        BindFlags: D3D11_BIND_SHADER_RESOURCE.0 as u32,
        ..Default::default()
    };
    let init = D3D11_SUBRESOURCE_DATA {
        pSysMem: buffer,
        SysMemPitch: pw as u32 * 4,
        SysMemSlicePitch: 0,
    };
    let mut src: Option<ID3D11Texture2D> = None;
    unsafe {
        if devices
            .d3d_device
            .CreateTexture2D(&desc, Some(&init), Some(&mut src))
            .is_err()
        {
            return;
        }
    }
    let src = match src {
        Some(t) => t,
        None => return,
    };

    ensure_swap_chain(
        devices,
        &mut surf.popup_swap_chain,
        &mut surf.popup_sw,
        &mut surf.popup_sh,
        &pv,
        pw,
        ph,
    );
    let sc = match surf.popup_swap_chain.as_ref() {
        Some(sc) => sc.clone(),
        None => return,
    };
    present_to_swap_chain(devices, &sc, &src);
}
