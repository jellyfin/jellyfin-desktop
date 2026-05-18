//! libmpv error codes wrapped in `Result`.

use crate::sys;
use std::ffi::CStr;
use std::fmt;

/// libmpv error. `code` is the negative integer libmpv returns; the string
/// payload is the static `mpv_error_string` lookup at construction time.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Error {
    pub code: i32,
}

impl Error {
    pub fn new(code: i32) -> Self {
        Self { code }
    }

    /// Human-readable error string from libmpv. Never null — libmpv falls
    /// back to "unknown error" for out-of-range codes.
    pub fn message(&self) -> &'static str {
        // SAFETY: mpv_error_string returns a pointer to a static string.
        let ptr = unsafe { sys::mpv_error_string(self.code) };
        if ptr.is_null() {
            return "unknown mpv error";
        }
        unsafe { CStr::from_ptr(ptr) }.to_str().unwrap_or("invalid utf-8")
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "mpv::Error({}: {})", self.code, self.message())
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message())
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// Wrap a libmpv return code into `Result<()>`. libmpv contract: `>= 0` on
/// success, negative on failure.
pub(crate) fn check(code: i32) -> Result<()> {
    if code >= 0 { Ok(()) } else { Err(Error::new(code)) }
}
