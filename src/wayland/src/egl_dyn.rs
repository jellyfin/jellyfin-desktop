//! Minimal dynamic loader for libEGL.so.1.
//!
//! Replaces the `khronos-egl` crate, which transitively pins
//! `libloading = "0.8"` and blocks the workspace from moving to 0.9.
//! Only the subset of EGL used by `dmabuf_probe` and `lifecycle` is exposed.

use libloading::{Library, Symbol};
use std::ffi::{CString, c_char, c_void};

pub type Int = i32;
pub type Boolean = u32;
pub type Enum = u32;
pub type EGLDisplay = *mut c_void;
pub type EGLContext = *mut c_void;
pub type EGLSurface = *mut c_void;
pub type EGLConfig = *mut c_void;
pub type NativeDisplayType = *mut c_void;

pub const TRUE: Boolean = 1;
pub const NONE: Int = 0x3038;
pub const WIDTH: Int = 0x3057;
pub const HEIGHT: Int = 0x3056;
pub const SURFACE_TYPE: Int = 0x3033;
pub const RENDERABLE_TYPE: Int = 0x3040;
pub const PBUFFER_BIT: Int = 0x0001;
pub const OPENGL_ES2_BIT: Int = 0x0004;
pub const OPENGL_ES_API: Enum = 0x30A0;
pub const CONTEXT_CLIENT_VERSION: Int = 0x3098;

pub type FnGetDisplay = unsafe extern "C" fn(NativeDisplayType) -> EGLDisplay;
pub type FnInitialize = unsafe extern "C" fn(EGLDisplay, *mut Int, *mut Int) -> Boolean;
pub type FnTerminate = unsafe extern "C" fn(EGLDisplay) -> Boolean;
pub type FnBindApi = unsafe extern "C" fn(Enum) -> Boolean;
pub type FnChooseConfig =
    unsafe extern "C" fn(EGLDisplay, *const Int, *mut EGLConfig, Int, *mut Int) -> Boolean;
pub type FnCreateContext =
    unsafe extern "C" fn(EGLDisplay, EGLConfig, EGLContext, *const Int) -> EGLContext;
pub type FnCreatePbufferSurface =
    unsafe extern "C" fn(EGLDisplay, EGLConfig, *const Int) -> EGLSurface;
pub type FnMakeCurrent =
    unsafe extern "C" fn(EGLDisplay, EGLSurface, EGLSurface, EGLContext) -> Boolean;
pub type FnDestroySurface = unsafe extern "C" fn(EGLDisplay, EGLSurface) -> Boolean;
pub type FnDestroyContext = unsafe extern "C" fn(EGLDisplay, EGLContext) -> Boolean;
pub type FnGetProcAddress = unsafe extern "C" fn(*const c_char) -> Option<extern "system" fn()>;
pub type FnGetError = unsafe extern "C" fn() -> Int;

pub struct Egl {
    _lib: Library,
    pub get_display: FnGetDisplay,
    pub initialize: FnInitialize,
    pub terminate: FnTerminate,
    pub bind_api: FnBindApi,
    pub choose_config: FnChooseConfig,
    pub create_context: FnCreateContext,
    pub create_pbuffer_surface: FnCreatePbufferSurface,
    pub make_current: FnMakeCurrent,
    pub destroy_surface: FnDestroySurface,
    pub destroy_context: FnDestroyContext,
    pub get_proc_address_raw: FnGetProcAddress,
    pub get_error: FnGetError,
}

impl Egl {
    pub fn load_default() -> Result<Self, String> {
        let lib = unsafe { Library::new("libEGL.so.1") }
            .map_err(|e| format!("libEGL not available: {}", e))?;
        Self::load_from(lib)
    }

    pub fn load_from(lib: Library) -> Result<Self, String> {
        unsafe fn get<T: Copy>(lib: &Library, name: &[u8]) -> Result<T, String> {
            let sym: Symbol<T> = unsafe { lib.get(name) }
                .map_err(|e| format!("missing {}: {}", String::from_utf8_lossy(name), e))?;
            Ok(*sym)
        }
        unsafe {
            Ok(Egl {
                get_display: get(&lib, b"eglGetDisplay\0")?,
                initialize: get(&lib, b"eglInitialize\0")?,
                terminate: get(&lib, b"eglTerminate\0")?,
                bind_api: get(&lib, b"eglBindAPI\0")?,
                choose_config: get(&lib, b"eglChooseConfig\0")?,
                create_context: get(&lib, b"eglCreateContext\0")?,
                create_pbuffer_surface: get(&lib, b"eglCreatePbufferSurface\0")?,
                make_current: get(&lib, b"eglMakeCurrent\0")?,
                destroy_surface: get(&lib, b"eglDestroySurface\0")?,
                destroy_context: get(&lib, b"eglDestroyContext\0")?,
                get_proc_address_raw: get(&lib, b"eglGetProcAddress\0")?,
                get_error: get(&lib, b"eglGetError\0")?,
                _lib: lib,
            })
        }
    }

    pub fn get_proc(&self, name: &str) -> Option<extern "system" fn()> {
        let c = CString::new(name).ok()?;
        unsafe { (self.get_proc_address_raw)(c.as_ptr()) }
    }
}
