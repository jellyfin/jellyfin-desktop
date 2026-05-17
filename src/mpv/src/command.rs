//! Helper for assembling NUL-terminated command argument arrays.
//!
//! libmpv's `mpv_command` / `mpv_command_async` take a `const char*[]`
//! terminated by a null pointer. This module owns the `CString` storage so
//! the pointers remain valid until the call returns.

use std::ffi::{CString, NulError};
use std::os::raw::c_char;

/// Owned command argument vector. Borrow `as_ptrs()` to obtain the
/// null-terminated pointer array libmpv expects.
pub struct Command {
    storage: Vec<CString>,
    ptrs: Vec<*const c_char>,
}

impl Command {
    pub fn new<I, S>(args: I) -> Result<Self, NulError>
    where
        I: IntoIterator<Item = S>,
        S: Into<Vec<u8>>,
    {
        let storage: Vec<CString> = args
            .into_iter()
            .map(|s| CString::new(s.into()))
            .collect::<Result<_, _>>()?;
        let mut ptrs: Vec<*const c_char> = storage.iter().map(|c| c.as_ptr()).collect();
        ptrs.push(std::ptr::null());
        Ok(Self { storage, ptrs })
    }

    pub fn as_ptr(&self) -> *mut *const c_char {
        self.ptrs.as_ptr() as *mut _
    }

    pub fn len(&self) -> usize {
        self.storage.len()
    }

    pub fn is_empty(&self) -> bool {
        self.storage.is_empty()
    }
}

// SAFETY: `Command` is logically a `Vec<CString>` plus its derived pointer
// table. Both halves are owned by the struct. The raw pointers in `ptrs`
// point into `storage`, which has stable addresses because `CString::as_ptr`
// refers to heap memory not the `CString` itself. Moving the `Command`
// preserves those addresses.
unsafe impl Send for Command {}
unsafe impl Sync for Command {}
