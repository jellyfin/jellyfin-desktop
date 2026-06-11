use core::ptr;
use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
use windows_sys::Win32::System::Threading::{
    CreateEventW, INFINITE, ResetEvent, SetEvent, WaitForSingleObject,
};

pub struct WakeEvent {
    handle: HANDLE,
}

// Win32 event HANDLEs are kernel objects; concurrent SetEvent/ResetEvent/Wait*
// are documented thread-safe.
unsafe impl Send for WakeEvent {}
unsafe impl Sync for WakeEvent {}

impl WakeEvent {
    pub fn new() -> Option<Self> {
        // manual-reset, initially non-signaled
        let h = unsafe { CreateEventW(ptr::null(), 1, 0, ptr::null()) };
        if h.is_null() {
            return None;
        }
        Some(WakeEvent { handle: h })
    }

    pub fn signal(&self) {
        unsafe {
            SetEvent(self.handle);
        }
    }

    pub fn drain(&self) {
        unsafe {
            ResetEvent(self.handle);
        }
    }

    /// Block until signaled. Manual-reset, so a `signal()` that lands
    /// before the call returns immediately.
    pub fn wait(&self) {
        unsafe {
            WaitForSingleObject(self.handle, INFINITE);
        }
    }
}

impl Drop for WakeEvent {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { CloseHandle(self.handle) };
        }
    }
}
