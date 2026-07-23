//! Test whether the GPU stack can import a GBM-allocated dmabuf as an EGL
//! image and bind it to a GL texture. Run once during Wayland init to decide
//! whether CEF's shared-texture path will work; if not, we fall back to
//! software CEF rendering.
//!
//! libEGL, libX11, and libgbm are all dlopened so the binary keeps no link
//! dependency on them (the X11 case only fires when CEF runs under
//! `--ozone-platform=x11` over XWayland).

use crate::egl_dyn as egl;
use libloading::{Library, Symbol};
use std::ffi::{CStr, CString, c_char, c_int, c_uint, c_void};
use std::os::fd::RawFd;
use std::ptr;

// ARGB8888 fourcc — pulled from drm_fourcc.h to avoid a libdrm dep.
const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241;
// Sentinel meaning "no explicit modifier" — never a real advertised modifier,
// filtered out of eglQueryDmaBufModifiersEXT results defensively.
const DRM_FORMAT_MOD_INVALID: u64 = 0x00ff_ffff_ffff_ffff;
const GL_TEXTURE_2D: c_uint = 0x0DE1;
const GL_NO_ERROR: c_uint = 0;
const GBM_BO_USE_RENDERING: u32 = 0x0002;

const EGL_PLATFORM_X11_KHR: egl::Enum = 0x31D5;
const EGL_LINUX_DMA_BUF_EXT: egl::Enum = 0x3270;
const EGL_LINUX_DRM_FOURCC_EXT: egl::Int = 0x3271;
const EGL_DMA_BUF_PLANE0_FD_EXT: egl::Int = 0x3272;
const EGL_DMA_BUF_PLANE0_OFFSET_EXT: egl::Int = 0x3273;
const EGL_DMA_BUF_PLANE0_PITCH_EXT: egl::Int = 0x3274;
const EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT: egl::Int = 0x3443;
const EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT: egl::Int = 0x3444;
const EGL_DEVICE_EXT: egl::Int = 0x322C;
const EGL_DRM_RENDER_NODE_FILE_EXT: egl::Int = 0x3377;

type GbmDevice = c_void;
type GbmBo = c_void;
type X11Display = c_void;

type FnGbmCreateDevice = unsafe extern "C" fn(c_int) -> *mut GbmDevice;
type FnGbmDeviceDestroy = unsafe extern "C" fn(*mut GbmDevice);
type FnGbmBoCreate = unsafe extern "C" fn(*mut GbmDevice, u32, u32, u32, u32) -> *mut GbmBo;
type FnGbmBoDestroy = unsafe extern "C" fn(*mut GbmBo);
type FnGbmBoGetFd = unsafe extern "C" fn(*mut GbmBo) -> c_int;
type FnGbmBoGetStride = unsafe extern "C" fn(*mut GbmBo) -> u32;
// Optional: only present on libgbm >= 17.1. Missing means the driver only
// exposed the modifier-less API, so we fall back to implicit gbm_bo_create.
type FnGbmBoCreateWithModifiers =
    unsafe extern "C" fn(*mut GbmDevice, u32, u32, u32, *const u64, c_uint) -> *mut GbmBo;
type FnGbmBoGetModifier = unsafe extern "C" fn(*mut GbmBo) -> u64;

type FnXOpenDisplay = unsafe extern "C" fn(*const c_char) -> *mut X11Display;
type FnXCloseDisplay = unsafe extern "C" fn(*mut X11Display) -> c_int;

type FnGlGenTextures = unsafe extern "C" fn(c_int, *mut c_uint);
type FnGlBindTexture = unsafe extern "C" fn(c_uint, c_uint);
type FnGlDeleteTextures = unsafe extern "C" fn(c_int, *const c_uint);
type FnGlGetError = unsafe extern "C" fn() -> c_uint;
type FnGlEglImageTargetTexture2DOes = unsafe extern "C" fn(c_uint, *mut c_void);

type FnEglGetPlatformDisplayExt =
    unsafe extern "C" fn(egl::Enum, *mut c_void, *const egl::Int) -> egl::EGLDisplay;
type FnEglCreateImageKhr = unsafe extern "C" fn(
    egl::EGLDisplay,
    egl::EGLContext,
    egl::Enum,
    *mut c_void,
    *const egl::Int,
) -> *mut c_void;
type FnEglDestroyImageKhr = unsafe extern "C" fn(egl::EGLDisplay, *mut c_void) -> egl::Boolean;
type FnEglQueryDisplayAttribExt =
    unsafe extern "C" fn(egl::EGLDisplay, egl::Int, *mut isize) -> egl::Boolean;
type FnEglQueryDeviceStringExt = unsafe extern "C" fn(*mut c_void, egl::Int) -> *const c_char;
type FnEglQueryDmaBufModifiersExt = unsafe extern "C" fn(
    egl::EGLDisplay,
    egl::Int,
    egl::Int,
    *mut u64,
    *mut egl::Boolean,
    *mut egl::Int,
) -> egl::Boolean;

/// Returns true if a GBM-allocated ARGB8888 dmabuf can be imported as an EGL
/// image and bound to a GL texture on the EGL display CEF will use. The
/// `ozone_platform` selects which display type to test (`"wayland"` uses the
/// passed `wayland_egl_dpy`; anything else opens an XWayland display).
///
/// When libgbm or the DRM render node is unavailable the probe returns true
/// (assume supported) — same fallback the C++ version used, so the platform
/// can opt into shared textures and let Chromium fail loudly if the runtime
/// stack disagrees.
///
/// `wayland_egl_dpy` may be NULL when `ozone_platform != "wayland"`.
///
/// # Safety
/// `ozone_platform` must be NUL-terminated or null. `wayland_egl_dpy`
/// must be a live `*mut wl_display` when `ozone_platform == "wayland"`.
pub unsafe fn jfn_wl_dmabuf_probe(
    ozone_platform: *const c_char,
    wayland_egl_dpy: *mut c_void,
) -> bool {
    let ozone = if ozone_platform.is_null() {
        ""
    } else {
        unsafe { CStr::from_ptr(ozone_platform) }
            .to_str()
            .unwrap_or_default()
    };
    match probe(ozone, wayland_egl_dpy) {
        Ok(b) => b,
        Err(msg) => {
            tracing::warn!("dmabuf probe: {}", msg);
            false
        }
    }
}

fn probe(ozone: &str, wayland_egl_dpy: *mut c_void) -> Result<bool, String> {
    let egl_lib = unsafe { Library::new("libEGL.so.1") }
        .map_err(|e| format!("libEGL not available: {}", e))?;
    let egl = egl::Egl::load_from(egl_lib).map_err(|e| format!("EGL load failed: {}", e))?;

    let (display, owns_display, _x11_state) = acquire_display(&egl, ozone, wayland_egl_dpy)?;

    let result = (|| -> Result<bool, String> {
        if unsafe { (egl.bind_api)(egl::OPENGL_ES_API) } != egl::TRUE {
            return Err("eglBindAPI failed".into());
        }

        let cfg_attrs: [egl::Int; 5] = [
            egl::RENDERABLE_TYPE,
            egl::OPENGL_ES2_BIT,
            egl::SURFACE_TYPE,
            egl::PBUFFER_BIT,
            egl::NONE,
        ];
        let mut config: egl::EGLConfig = ptr::null_mut();
        let mut num_config: egl::Int = 0;
        if unsafe {
            (egl.choose_config)(display, cfg_attrs.as_ptr(), &mut config, 1, &mut num_config)
        } != egl::TRUE
        {
            return Err("eglChooseConfig failed".into());
        }
        if num_config == 0 || config.is_null() {
            return Err("no suitable EGL config".to_string());
        }

        let ctx_attrs: [egl::Int; 3] = [egl::CONTEXT_CLIENT_VERSION, 2, egl::NONE];
        let ctx =
            unsafe { (egl.create_context)(display, config, ptr::null_mut(), ctx_attrs.as_ptr()) };
        if ctx.is_null() {
            return Err("can't create GLES context".into());
        }

        let pb_attrs: [egl::Int; 5] = [egl::WIDTH, 1, egl::HEIGHT, 1, egl::NONE];
        let pbuf = unsafe { (egl.create_pbuffer_surface)(display, config, pb_attrs.as_ptr()) };
        // pbuf may legitimately be null; make_current will then fail.

        if unsafe { (egl.make_current)(display, pbuf, pbuf, ctx) } != egl::TRUE {
            if !pbuf.is_null() {
                unsafe { (egl.destroy_surface)(display, pbuf) };
            }
            unsafe { (egl.destroy_context)(display, ctx) };
            return Err("eglMakeCurrent failed".into());
        }

        let gl_result = run_gl_test(&egl, display);

        unsafe { (egl.make_current)(display, ptr::null_mut(), ptr::null_mut(), ptr::null_mut()) };
        if !pbuf.is_null() {
            unsafe { (egl.destroy_surface)(display, pbuf) };
        }
        unsafe { (egl.destroy_context)(display, ctx) };

        gl_result
    })();

    if owns_display {
        unsafe { (egl.terminate)(display) };
    }

    match &result {
        Ok(true) => tracing::info!("dmabuf probe: GBM -> EGL -> GL import OK"),
        Ok(false) => tracing::warn!("dmabuf probe: ARGB8888 dmabuf import failed"),
        Err(e) => tracing::warn!("dmabuf probe: {}", e),
    }
    result
}

struct X11Owned {
    _lib: Library,
    dpy: *mut X11Display,
    close: FnXCloseDisplay,
}

impl Drop for X11Owned {
    fn drop(&mut self) {
        if !self.dpy.is_null() {
            unsafe { (self.close)(self.dpy) };
        }
    }
}

fn acquire_display(
    egl: &egl::Egl,
    ozone: &str,
    wayland_egl_dpy: *mut c_void,
) -> Result<(egl::EGLDisplay, bool, Option<X11Owned>), String> {
    if ozone == "wayland" {
        tracing::info!("dmabuf probe: testing on Wayland EGL display");
        return Ok((wayland_egl_dpy as egl::EGLDisplay, false, None));
    }

    let lib = unsafe { Library::new("libX11.so.6") }
        .map_err(|e| format!("libX11 not available: {}", e))?;
    let open: libloading::Symbol<FnXOpenDisplay> = unsafe { lib.get(b"XOpenDisplay\0") }
        .map_err(|e| format!("XOpenDisplay missing: {}", e))?;
    let close: libloading::Symbol<FnXCloseDisplay> = unsafe { lib.get(b"XCloseDisplay\0") }
        .map_err(|e| format!("XCloseDisplay missing: {}", e))?;
    let close_fn: FnXCloseDisplay = *close;
    let dpy = unsafe { open(ptr::null()) };
    if dpy.is_null() {
        return Err("XOpenDisplay failed (no XWayland?)".into());
    }
    let owned = X11Owned {
        _lib: lib,
        dpy,
        close: close_fn,
    };

    let display: egl::EGLDisplay = if let Some(fp) = egl.get_proc("eglGetPlatformDisplayEXT") {
        let f: FnEglGetPlatformDisplayExt = unsafe { std::mem::transmute(fp) };
        unsafe { f(EGL_PLATFORM_X11_KHR, dpy, ptr::null()) }
    } else {
        unsafe { (egl.get_display)(dpy) }
    };
    if display.is_null() {
        return Err("no EGL display for X11".into());
    }

    let mut major: egl::Int = 0;
    let mut minor: egl::Int = 0;
    if unsafe { (egl.initialize)(display, &mut major, &mut minor) } != egl::TRUE {
        return Err("EGL init on X11 failed".into());
    }
    tracing::info!(
        "dmabuf probe: testing on X11 EGL display ({}.{})",
        major,
        minor
    );

    Ok((display, true, Some(owned)))
}

/// Modifiers the driver advertises for ARGB8888, filtered to ones usable as
/// a plain sampled 2D texture (drops external-only entries, relevant mainly
/// to planar/YUV formats but checked here for correctness). Empty when
/// `EGL_EXT_image_dma_buf_import_modifiers` isn't available — callers should
/// fall back to implicit-modifier import in that case.
fn query_argb8888_modifiers(egl: &egl::Egl, display: egl::EGLDisplay) -> Vec<u64> {
    let Ok(query) = get_gl::<FnEglQueryDmaBufModifiersExt>(egl, "eglQueryDmaBufModifiersEXT")
    else {
        return Vec::new();
    };
    let fourcc = DRM_FORMAT_ARGB8888 as egl::Int;
    let mut count: egl::Int = 0;
    let queried = unsafe {
        query(
            display,
            fourcc,
            0,
            ptr::null_mut(),
            ptr::null_mut(),
            &mut count,
        )
    };
    if queried != egl::TRUE || count <= 0 {
        return Vec::new();
    }

    let mut modifiers = vec![0u64; count as usize];
    let mut external_only = vec![0u32; count as usize];
    let queried = unsafe {
        query(
            display,
            fourcc,
            count,
            modifiers.as_mut_ptr(),
            external_only.as_mut_ptr(),
            &mut count,
        )
    };
    if queried != egl::TRUE {
        return Vec::new();
    }

    modifiers
        .into_iter()
        .zip(external_only)
        .filter(|(m, external)| *external == 0 && *m != DRM_FORMAT_MOD_INVALID)
        .map(|(m, _)| m)
        .collect()
}

fn run_gl_test(egl: &egl::Egl, display: egl::EGLDisplay) -> Result<bool, String> {
    let gen_tex = get_gl::<FnGlGenTextures>(egl, "glGenTextures")?;
    let bind_tex = get_gl::<FnGlBindTexture>(egl, "glBindTexture")?;
    let del_tex = get_gl::<FnGlDeleteTextures>(egl, "glDeleteTextures")?;
    let get_err = get_gl::<FnGlGetError>(egl, "glGetError")?;
    let img_target = get_gl::<FnGlEglImageTargetTexture2DOes>(egl, "glEGLImageTargetTexture2DOES")?;
    let create_image = get_gl::<FnEglCreateImageKhr>(egl, "eglCreateImageKHR")?;
    let destroy_image = get_gl::<FnEglDestroyImageKhr>(egl, "eglDestroyImageKHR")?;

    let gbm_lib = match unsafe { Library::new("libgbm.so.1") } {
        Ok(l) => l,
        Err(_) => {
            tracing::warn!("dmabuf probe: libgbm not available, assuming supported");
            return Ok(true);
        }
    };
    let gbm = match GbmFns::load(&gbm_lib) {
        Some(g) => g,
        None => {
            tracing::warn!("dmabuf probe: libgbm missing symbols, assuming supported");
            return Ok(true);
        }
    };

    let drm_fd = find_drm_node(egl, display)
        .ok_or(())
        .or_else(|_| open_legacy_node().ok_or(()));
    let drm_fd = match drm_fd {
        Ok(fd) => fd,
        Err(_) => {
            tracing::warn!("dmabuf probe: no DRM render node, assuming supported");
            return Ok(true);
        }
    };

    let device = unsafe { (gbm.create_device)(drm_fd) };
    if device.is_null() {
        unsafe { libc::close(drm_fd) };
        return Err("gbm_create_device failed".into());
    }

    // NVIDIA's EGL/GBM stack doesn't support implicit-modifier dmabuf import
    // the way Mesa does — glEGLImageTargetTexture2DOES fails with
    // GL_INVALID_OPERATION unless we negotiate an explicit one. Missing
    // symbols or an empty modifier list both fall through to the pre-existing
    // implicit gbm_bo_create path, so Mesa drivers are unaffected.
    let create_with_modifiers: Option<FnGbmBoCreateWithModifiers> = unsafe {
        gbm_lib
            .get(b"gbm_bo_create_with_modifiers\0")
            .ok()
            .map(|s: Symbol<FnGbmBoCreateWithModifiers>| *s)
    };
    let get_modifier: Option<FnGbmBoGetModifier> = unsafe {
        gbm_lib
            .get(b"gbm_bo_get_modifier\0")
            .ok()
            .map(|s: Symbol<FnGbmBoGetModifier>| *s)
    };
    let modifiers = query_argb8888_modifiers(egl, display);

    let (bo, modifier) = match (create_with_modifiers, get_modifier, modifiers.is_empty()) {
        (Some(create), Some(get_mod), false) => {
            let bo = unsafe {
                create(
                    device,
                    64,
                    64,
                    DRM_FORMAT_ARGB8888,
                    modifiers.as_ptr(),
                    modifiers.len() as c_uint,
                )
            };
            if bo.is_null() {
                tracing::warn!(
                    "dmabuf probe: gbm_bo_create_with_modifiers failed, falling back to implicit modifier"
                );
                (
                    unsafe {
                        (gbm.bo_create)(device, 64, 64, DRM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING)
                    },
                    None,
                )
            } else {
                (bo, Some(unsafe { get_mod(bo) }))
            }
        }
        _ => (
            unsafe { (gbm.bo_create)(device, 64, 64, DRM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING) },
            None,
        ),
    };
    if bo.is_null() {
        unsafe {
            (gbm.device_destroy)(device);
            libc::close(drm_fd);
        }
        return Err("gbm_bo_create ARGB8888 failed".into());
    }
    if let Some(m) = modifier {
        tracing::info!("dmabuf probe: using explicit modifier 0x{:x}", m);
    }

    let dmabuf_fd = unsafe { (gbm.bo_get_fd)(bo) };
    let stride = unsafe { (gbm.bo_get_stride)(bo) };

    let result = if dmabuf_fd < 0 {
        Err("gbm_bo_get_fd failed".to_string())
    } else {
        let mut img_attrs: Vec<egl::Int> = vec![
            egl::WIDTH,
            64,
            egl::HEIGHT,
            64,
            EGL_LINUX_DRM_FOURCC_EXT,
            DRM_FORMAT_ARGB8888 as egl::Int,
            EGL_DMA_BUF_PLANE0_FD_EXT,
            dmabuf_fd as egl::Int,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            stride as egl::Int,
        ];
        if let Some(m) = modifier {
            img_attrs.extend([
                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                (m & 0xffff_ffff) as egl::Int,
                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                (m >> 32) as egl::Int,
            ]);
        }
        img_attrs.push(egl::NONE);
        let image = unsafe {
            create_image(
                display,
                ptr::null_mut(),
                EGL_LINUX_DMA_BUF_EXT,
                ptr::null_mut(),
                img_attrs.as_ptr(),
            )
        };
        if image.is_null() {
            tracing::warn!(
                "dmabuf probe: eglCreateImageKHR failed (0x{:x}, modifier={:?})",
                unsafe { (egl.get_error)() },
                modifier
            );
            Ok(false)
        } else {
            let mut tex: c_uint = 0;
            unsafe {
                gen_tex(1, &mut tex);
                bind_tex(GL_TEXTURE_2D, tex);
                img_target(GL_TEXTURE_2D, image);
                let err = get_err();
                let ok = err == GL_NO_ERROR;
                if !ok {
                    tracing::warn!(
                        "dmabuf probe: glEGLImageTargetTexture2DOES failed (0x{:x})",
                        err
                    );
                }
                del_tex(1, &tex);
                destroy_image(display, image);
                Ok(ok)
            }
        }
    };

    if dmabuf_fd >= 0 {
        unsafe { libc::close(dmabuf_fd) };
    }
    unsafe {
        (gbm.bo_destroy)(bo);
        (gbm.device_destroy)(device);
        libc::close(drm_fd);
    }
    result
}

struct GbmFns {
    create_device: FnGbmCreateDevice,
    device_destroy: FnGbmDeviceDestroy,
    bo_create: FnGbmBoCreate,
    bo_destroy: FnGbmBoDestroy,
    bo_get_fd: FnGbmBoGetFd,
    bo_get_stride: FnGbmBoGetStride,
}

impl GbmFns {
    fn load(lib: &Library) -> Option<Self> {
        unsafe {
            Some(Self {
                create_device: *lib.get::<FnGbmCreateDevice>(b"gbm_create_device\0").ok()?,
                device_destroy: *lib
                    .get::<FnGbmDeviceDestroy>(b"gbm_device_destroy\0")
                    .ok()?,
                bo_create: *lib.get::<FnGbmBoCreate>(b"gbm_bo_create\0").ok()?,
                bo_destroy: *lib.get::<FnGbmBoDestroy>(b"gbm_bo_destroy\0").ok()?,
                bo_get_fd: *lib.get::<FnGbmBoGetFd>(b"gbm_bo_get_fd\0").ok()?,
                bo_get_stride: *lib.get::<FnGbmBoGetStride>(b"gbm_bo_get_stride\0").ok()?,
            })
        }
    }
}

fn find_drm_node_path(egl: &egl::Egl, display: egl::EGLDisplay) -> Option<CString> {
    let query_display_ptr = egl.get_proc("eglQueryDisplayAttribEXT")?;
    let query_device_str_ptr = egl.get_proc("eglQueryDeviceStringEXT")?;
    let query_display: FnEglQueryDisplayAttribExt =
        unsafe { std::mem::transmute(query_display_ptr) };
    let query_device_str: FnEglQueryDeviceStringExt =
        unsafe { std::mem::transmute(query_device_str_ptr) };

    let mut device_attrib: isize = 0;
    let ok = unsafe { query_display(display, EGL_DEVICE_EXT, &mut device_attrib) };
    if ok == 0 || device_attrib == 0 {
        return None;
    }
    let egl_device = device_attrib as *mut c_void;
    let node_ptr = unsafe { query_device_str(egl_device, EGL_DRM_RENDER_NODE_FILE_EXT) };
    if node_ptr.is_null() {
        return None;
    }
    Some(unsafe { CStr::from_ptr(node_ptr) }.to_owned())
}

fn find_drm_node(egl: &egl::Egl, display: egl::EGLDisplay) -> Option<RawFd> {
    let node = find_drm_node_path(egl, display)?;
    let fd = unsafe { libc::open(node.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
    if fd >= 0 {
        tracing::info!("dmabuf probe using render node: {}", node.to_string_lossy());
        Some(fd)
    } else {
        None
    }
}

/// # Safety
/// Same contract as [`jfn_wl_dmabuf_probe`].
pub unsafe fn cef_render_node(
    ozone_platform: *const c_char,
    wayland_egl_dpy: *mut c_void,
) -> Option<(i64, i64)> {
    let ozone = if ozone_platform.is_null() {
        ""
    } else {
        unsafe { CStr::from_ptr(ozone_platform) }
            .to_str()
            .unwrap_or_default()
    };
    match render_node(ozone, wayland_egl_dpy) {
        Ok(node) => node,
        Err(msg) => {
            tracing::warn!("cef render node: {}", msg);
            None
        }
    }
}

fn render_node(ozone: &str, wayland_egl_dpy: *mut c_void) -> Result<Option<(i64, i64)>, String> {
    let egl_lib = unsafe { Library::new("libEGL.so.1") }
        .map_err(|e| format!("libEGL not available: {}", e))?;
    let egl = egl::Egl::load_from(egl_lib).map_err(|e| format!("EGL load failed: {}", e))?;

    let (display, owns_display, _x11_state) = acquire_display(&egl, ozone, wayland_egl_dpy)?;
    let path = find_drm_node_path(&egl, display);
    if owns_display {
        unsafe { (egl.terminate)(display) };
    }
    Ok(path.as_deref().and_then(node_major_minor))
}

fn node_major_minor(path: &CStr) -> Option<(i64, i64)> {
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    if unsafe { libc::stat(path.as_ptr(), &mut st) } != 0 {
        return None;
    }
    // glibc dev_t encoding; the plain POSIX major/minor split decodes wrong.
    let rdev = st.st_rdev as u64;
    let major = ((rdev >> 8) & 0xfff) | ((rdev >> 32) & !0xfff_u64);
    let minor = (rdev & 0xff) | ((rdev >> 12) & !0xff_u64);
    Some((major as i64, minor as i64))
}

fn open_legacy_node() -> Option<RawFd> {
    for i in 128..136 {
        let path = format!("/dev/dri/renderD{}\0", i);
        let fd = unsafe {
            libc::open(
                path.as_ptr() as *const c_char,
                libc::O_RDWR | libc::O_CLOEXEC,
            )
        };
        if fd >= 0 {
            return Some(fd);
        }
    }
    None
}

fn get_gl<T>(egl: &egl::Egl, name: &str) -> Result<T, String> {
    egl.get_proc(name)
        .map(|p| unsafe { std::mem::transmute_copy::<extern "system" fn(), T>(&p) })
        .ok_or_else(|| format!("missing {}", name))
}
