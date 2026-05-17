//! Safe wrapper for `mpv_handle`.
//!
//! Owns the underlying mpv instance via RAII; `Drop` calls
//! `mpv_terminate_destroy`. All methods are typed via the `Format` trait
//! so callers don't deal with `mpv_format` directly.

use crate::command::Command;
use crate::error::{Error, Result, check};
use crate::event::Event;
use crate::log::LogLevel;
use crate::property::{Flag, Format};
use crate::sys;
use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::ptr;
use std::sync::Mutex;

/// Type alias for the user-supplied wakeup callback. libmpv invokes this
/// on an arbitrary thread when new events are queued.
pub type WakeupCallback = Box<dyn Fn() + Send + Sync + 'static>;

/// Safe owner of an `mpv_handle`.
///
/// libmpv documents the handle as thread-safe for command/property
/// invocations from any thread, so `Handle: Send + Sync`. The wakeup
/// callback may fire on any thread.
pub struct Handle {
    raw: *mut sys::mpv_handle,
    /// Pinned wakeup callback, kept alive for the handle's lifetime so the
    /// trampoline pointer stays valid. libmpv guarantees no more callbacks
    /// fire after `mpv_terminate_destroy` returns.
    wakeup: Mutex<Option<Box<WakeupCallback>>>,
}

// SAFETY: libmpv handle methods are thread-safe per documentation.
unsafe impl Send for Handle {}
unsafe impl Sync for Handle {}

impl Handle {
    /// Construct a fresh uninitialized handle via `mpv_create`. Apply
    /// options with `set_option*`, then call `initialize()`.
    pub fn create() -> Result<Self> {
        let raw = unsafe { sys::mpv_create() };
        if raw.is_null() {
            // mpv_create returns NULL only on allocation failure or
            // unrecoverable init error; no error code is available.
            return Err(Error::new(sys::mpv_error::MPV_ERROR_NOMEM.0 as i32));
        }
        Ok(Self {
            raw,
            wakeup: Mutex::new(None),
        })
    }

    pub fn initialize(&self) -> Result<()> {
        check(unsafe { sys::mpv_initialize(self.raw) })
    }

    /// Explicitly destroy the handle. After this returns, the `Handle` is
    /// inert; `Drop` is a no-op. Use on platforms where the dtor can't run
    /// at scope exit (e.g. macOS GCD deadlock on main thread).
    pub fn terminate_destroy(&mut self) {
        if !self.raw.is_null() {
            unsafe { sys::mpv_terminate_destroy(self.raw) };
            self.raw = ptr::null_mut();
            // Drop the callback box; libmpv guarantees no further wakeups.
            self.wakeup.lock().unwrap().take();
        }
    }

    pub fn raw(&self) -> *mut sys::mpv_handle {
        self.raw
    }

    // -----------------------------------------------------------------
    // Options (must be called before initialize)
    // -----------------------------------------------------------------

    pub fn set_option<T: Format + Copy>(&self, name: &str, value: T) -> Result<()> {
        let c = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        let mut v = value;
        check(unsafe {
            sys::mpv_set_option(
                self.raw,
                c.as_ptr(),
                T::MPV_FORMAT,
                &mut v as *mut _ as *mut c_void,
            )
        })
    }

    pub fn set_option_flag(&self, name: &str, value: bool) -> Result<()> {
        let mut v: i32 = if value { 1 } else { 0 };
        let c = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        check(unsafe {
            sys::mpv_set_option(
                self.raw,
                c.as_ptr(),
                Flag::MPV_FORMAT,
                &mut v as *mut _ as *mut c_void,
            )
        })
    }

    pub fn set_option_string(&self, name: &str, value: &str) -> Result<()> {
        let n = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        let v = CString::new(value).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        check(unsafe { sys::mpv_set_option_string(self.raw, n.as_ptr(), v.as_ptr()) })
    }

    // -----------------------------------------------------------------
    // Properties (sync)
    // -----------------------------------------------------------------

    pub fn get_property<T: Format + Copy + Default>(&self, name: &str) -> Result<T> {
        let n = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        let mut out: T = T::default();
        check(unsafe {
            sys::mpv_get_property(
                self.raw,
                n.as_ptr(),
                T::MPV_FORMAT,
                &mut out as *mut _ as *mut c_void,
            )
        })?;
        Ok(out)
    }

    pub fn get_property_flag(&self, name: &str) -> Result<bool> {
        let n = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        let mut out: i32 = 0;
        check(unsafe {
            sys::mpv_get_property(
                self.raw,
                n.as_ptr(),
                Flag::MPV_FORMAT,
                &mut out as *mut _ as *mut c_void,
            )
        })?;
        Ok(out != 0)
    }

    pub fn get_property_string(&self, name: &str) -> Result<String> {
        let n = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        let p = unsafe { sys::mpv_get_property_string(self.raw, n.as_ptr()) };
        if p.is_null() {
            return Err(Error::new(sys::mpv_error::MPV_ERROR_PROPERTY_UNAVAILABLE.0 as i32));
        }
        let s = unsafe { std::ffi::CStr::from_ptr(p) }
            .to_string_lossy()
            .into_owned();
        unsafe { sys::mpv_free(p as *mut c_void) };
        Ok(s)
    }

    // -----------------------------------------------------------------
    // Properties (async)
    // -----------------------------------------------------------------

    pub fn set_property_async<T: Format + Copy>(
        &self,
        reply: u64,
        name: &str,
        value: T,
    ) -> Result<()> {
        let n = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        let mut v = value;
        check(unsafe {
            sys::mpv_set_property_async(
                self.raw,
                reply,
                n.as_ptr(),
                T::MPV_FORMAT,
                &mut v as *mut _ as *mut c_void,
            )
        })
    }

    pub fn set_property_flag_async(&self, reply: u64, name: &str, value: bool) -> Result<()> {
        let n = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        let mut v: i32 = if value { 1 } else { 0 };
        check(unsafe {
            sys::mpv_set_property_async(
                self.raw,
                reply,
                n.as_ptr(),
                Flag::MPV_FORMAT,
                &mut v as *mut _ as *mut c_void,
            )
        })
    }

    pub fn set_property_string_async(&self, reply: u64, name: &str, value: &str) -> Result<()> {
        let n = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        // libmpv's MPV_FORMAT_STRING takes a `const char**` pointing at a
        // C string; copy the value into a CString so it survives the call.
        let v = CString::new(value).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        let mut ptr_value: *const c_char = v.as_ptr();
        check(unsafe {
            sys::mpv_set_property_async(
                self.raw,
                reply,
                n.as_ptr(),
                sys::mpv_format::MPV_FORMAT_STRING,
                &mut ptr_value as *mut _ as *mut c_void,
            )
        })
    }

    // -----------------------------------------------------------------
    // Property observation
    // -----------------------------------------------------------------

    pub fn observe_property(&self, reply: u64, name: &str, format: sys::mpv_format) -> Result<()> {
        let n = CString::new(name).map_err(|_| Error::new(sys::mpv_error::MPV_ERROR_INVALID_PARAMETER.0 as i32))?;
        check(unsafe { sys::mpv_observe_property(self.raw, reply, n.as_ptr(), format) })
    }

    pub fn observe_property_typed<T: Format>(&self, reply: u64, name: &str) -> Result<()> {
        self.observe_property(reply, name, T::MPV_FORMAT)
    }

    pub fn observe_property_node(&self, reply: u64, name: &str) -> Result<()> {
        self.observe_property(reply, name, sys::mpv_format::MPV_FORMAT_NODE)
    }

    // -----------------------------------------------------------------
    // Commands
    // -----------------------------------------------------------------

    pub fn command(&self, cmd: &Command) -> Result<()> {
        check(unsafe { sys::mpv_command(self.raw, cmd.as_ptr()) })
    }

    pub fn command_async(&self, reply: u64, cmd: &Command) -> Result<()> {
        check(unsafe { sys::mpv_command_async(self.raw, reply, cmd.as_ptr()) })
    }

    // -----------------------------------------------------------------
    // Events
    // -----------------------------------------------------------------

    /// Block (or wait up to `timeout` seconds; negative = forever) for the
    /// next event. Returns owned `Event`; the underlying `mpv_event`
    /// pointer is consumed before this returns.
    pub fn wait_event(&self, timeout: f64) -> Event {
        let raw = unsafe { sys::mpv_wait_event(self.raw, timeout) };
        unsafe { Event::from_raw(raw) }
    }

    pub fn wakeup(&self) {
        unsafe { sys::mpv_wakeup(self.raw) };
    }

    /// Install a Rust closure as the wakeup callback. Replaces any prior
    /// callback. The closure may be invoked on arbitrary threads.
    pub fn set_wakeup_callback<F>(&self, cb: F)
    where
        F: Fn() + Send + Sync + 'static,
    {
        let boxed: Box<WakeupCallback> = Box::new(Box::new(cb));
        let ptr = Box::as_ref(&boxed) as *const WakeupCallback as *mut c_void;
        // Store first, then arm libmpv — otherwise a wakeup racing in
        // between would see a dangling pointer.
        let mut slot = self.wakeup.lock().unwrap();
        *slot = Some(boxed);
        unsafe { sys::mpv_set_wakeup_callback(self.raw, Some(wakeup_trampoline), ptr) };
    }

    pub fn request_log_messages(&self, level: LogLevel) -> Result<()> {
        let token = CString::new(level.as_token()).unwrap();
        check(unsafe { sys::mpv_request_log_messages(self.raw, token.as_ptr()) })
    }
}

unsafe extern "C" fn wakeup_trampoline(data: *mut c_void) {
    if data.is_null() {
        return;
    }
    let cb = unsafe { &*(data as *const WakeupCallback) };
    cb();
}

impl Drop for Handle {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            // mpv_terminate_destroy blocks until in-flight callbacks return.
            // After it returns, libmpv promises no further wakeup invocations,
            // so the boxed callback can be safely dropped via `wakeup`'s Drop.
            unsafe { sys::mpv_terminate_destroy(self.raw) };
            self.raw = ptr::null_mut();
        }
    }
}
