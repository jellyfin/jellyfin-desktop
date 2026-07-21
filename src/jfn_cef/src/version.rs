//! Runtime version of the libcef loaded into this process.

use std::ffi::CStr;
use std::fmt;
use std::os::raw::c_int;
use std::sync::LazyLock;

// Entries (from CEF's cef_version.h): 0-2 CEF major/minor/patch,
// 3 commit number, 4-7 Chromium major/minor/build/patch.
unsafe extern "C" {
    fn cef_version_info(entry: c_int) -> c_int;
}

pub struct ShortHash([u8; 7]);

impl ShortHash {
    fn new(full: &str) -> Option<Self> {
        let bytes = full.as_bytes().get(..7)?;
        if !bytes.iter().all(u8::is_ascii_hexdigit) {
            return None;
        }
        let mut hash = [0u8; 7];
        hash.copy_from_slice(bytes);
        Some(Self(hash))
    }
}

impl fmt::Display for ShortHash {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(str::from_utf8(&self.0).unwrap_or_default())
    }
}

pub struct CefVersionInfo {
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
    pub commit: ShortHash,
    pub chromium: [u32; 4],
}

pub enum CefVersion {
    Known(CefVersionInfo),
    Unknown,
}

impl fmt::Display for CefVersion {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Known(v) => write!(
                f,
                "{}.{}.{}+g{}+chromium-{}.{}.{}.{}",
                v.major,
                v.minor,
                v.patch,
                v.commit,
                v.chromium[0],
                v.chromium[1],
                v.chromium[2],
                v.chromium[3],
            ),
            Self::Unknown => f.write_str("unknown"),
        }
    }
}

impl From<&CefVersion> for serde_json::Value {
    fn from(version: &CefVersion) -> Self {
        match version {
            CefVersion::Known(_) => Self::String(version.to_string()),
            CefVersion::Unknown => Self::Null,
        }
    }
}

fn commit_hash() -> Option<ShortHash> {
    // cef_api_hash's first call also configures the libcef API version;
    // it must get the same value the cef crate passes.
    let ptr = unsafe { cef::sys::cef_api_hash(cef::sys::CEF_API_VERSION_LAST, 2) };
    if ptr.is_null() {
        return None;
    }
    let full = unsafe { CStr::from_ptr(ptr) }.to_str().ok()?;
    ShortHash::new(full)
}

fn probe() -> CefVersion {
    let Some(commit) = commit_hash() else {
        return CefVersion::Unknown;
    };
    let v = |entry| unsafe { cef_version_info(entry) }.unsigned_abs();
    CefVersion::Known(CefVersionInfo {
        major: v(0),
        minor: v(1),
        patch: v(2),
        commit,
        chromium: [v(4), v(5), v(6), v(7)],
    })
}

static CEF_VERSION: LazyLock<CefVersion> = LazyLock::new(probe);

pub fn cef_version() -> &'static CefVersion {
    &CEF_VERSION
}
