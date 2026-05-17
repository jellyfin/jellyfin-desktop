//! Settings store. Owns the in-memory state, JSON persistence, and the
//! singleton accessor that the C++ side calls through.
//!
//! On-disk schema is a JSON object with the field names used by
//! [`SettingsData::to_json`]. Missing keys keep their defaults on load; save
//! suppresses fields that are at their default (empty strings, sentinel
//! values, zero geometry) so existing config files round-trip unchanged.

use serde_json::{Map, Value, json};
use std::ffi::{CStr, CString, c_char};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::{Mutex, OnceLock};
use std::thread;

const DEVICE_NAME_MAX: usize = 64;
const HWDEC_DEFAULT: &str = "no";

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct JfnWindowGeometry {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
    pub logical_width: i32,
    pub logical_height: i32,
    pub scale: f32,
    pub maximized: bool,
}

impl Default for JfnWindowGeometry {
    fn default() -> Self {
        Self {
            x: -1,
            y: -1,
            width: 0,
            height: 0,
            logical_width: 0,
            logical_height: 0,
            scale: 0.0,
            maximized: false,
        }
    }
}

#[derive(Clone, Debug)]
struct SettingsData {
    server_url: String,
    hwdec: String,
    audio_passthrough: String,
    audio_channels: String,
    log_level: String,
    device_name: String,
    window: JfnWindowGeometry,
    audio_exclusive: bool,
    disable_gpu_compositing: bool,
    titlebar_theme_color: bool,
    transparent_titlebar: bool,
    force_transcoding: bool,
}

impl Default for SettingsData {
    fn default() -> Self {
        Self {
            server_url: String::new(),
            hwdec: String::new(),
            audio_passthrough: String::new(),
            audio_channels: String::new(),
            log_level: String::new(),
            device_name: String::new(),
            window: JfnWindowGeometry::default(),
            audio_exclusive: false,
            disable_gpu_compositing: false,
            titlebar_theme_color: true,
            transparent_titlebar: true,
            force_transcoding: false,
        }
    }
}

impl SettingsData {
    fn overlay_json(&mut self, v: &Value) {
        let Some(_) = v.as_object() else {
            return;
        };
        if let Some(s) = v.get("serverUrl").and_then(Value::as_str) {
            self.server_url = s.into();
        }
        if let Some(s) = v.get("hwdec").and_then(Value::as_str) {
            self.hwdec = s.into();
        }
        if let Some(s) = v.get("audioPassthrough").and_then(Value::as_str) {
            self.audio_passthrough = s.into();
        }
        if let Some(s) = v.get("audioChannels").and_then(Value::as_str) {
            self.audio_channels = s.into();
        }
        if let Some(s) = v.get("logLevel").and_then(Value::as_str) {
            self.log_level = s.into();
        }
        if let Some(s) = v.get("deviceName").and_then(Value::as_str) {
            let mut s = s.to_string();
            if s.len() > DEVICE_NAME_MAX {
                s.truncate(DEVICE_NAME_MAX);
            }
            self.device_name = s;
        }
        if let Some(n) = v.get("windowWidth").and_then(Value::as_i64) {
            self.window.width = n as i32;
        }
        if let Some(n) = v.get("windowHeight").and_then(Value::as_i64) {
            self.window.height = n as i32;
        }
        if let Some(n) = v.get("windowLogicalWidth").and_then(Value::as_i64) {
            self.window.logical_width = n as i32;
        }
        if let Some(n) = v.get("windowLogicalHeight").and_then(Value::as_i64) {
            self.window.logical_height = n as i32;
        }
        if let Some(n) = v.get("windowScale").and_then(Value::as_f64) {
            self.window.scale = n as f32;
        }
        if let Some(n) = v.get("windowX").and_then(Value::as_i64) {
            self.window.x = n as i32;
        }
        if let Some(n) = v.get("windowY").and_then(Value::as_i64) {
            self.window.y = n as i32;
        }
        if let Some(b) = v.get("windowMaximized").and_then(Value::as_bool) {
            self.window.maximized = b;
        }
        if let Some(b) = v.get("audioExclusive").and_then(Value::as_bool) {
            self.audio_exclusive = b;
        }
        if let Some(b) = v.get("disableGpuCompositing").and_then(Value::as_bool) {
            self.disable_gpu_compositing = b;
        }
        if let Some(b) = v.get("titlebarThemeColor").and_then(Value::as_bool) {
            self.titlebar_theme_color = b;
        }
        if let Some(b) = v.get("transparentTitlebar").and_then(Value::as_bool) {
            self.transparent_titlebar = b;
        }
        if let Some(b) = v.get("forceTranscoding").and_then(Value::as_bool) {
            self.force_transcoding = b;
        }
    }

    fn to_json(&self) -> Value {
        let mut o = Map::new();
        o.insert("serverUrl".into(), Value::String(self.server_url.clone()));
        if self.window.width > 0 && self.window.height > 0 {
            o.insert("windowWidth".into(), json!(self.window.width));
            o.insert("windowHeight".into(), json!(self.window.height));
        }
        if self.window.logical_width > 0 && self.window.logical_height > 0 {
            o.insert("windowLogicalWidth".into(), json!(self.window.logical_width));
            o.insert(
                "windowLogicalHeight".into(),
                json!(self.window.logical_height),
            );
        }
        if self.window.scale > 0.0 {
            o.insert("windowScale".into(), json!(self.window.scale));
        }
        if self.window.x >= 0 && self.window.y >= 0 {
            o.insert("windowX".into(), json!(self.window.x));
            o.insert("windowY".into(), json!(self.window.y));
        }
        o.insert(
            "windowMaximized".into(),
            Value::Bool(self.window.maximized),
        );
        if !self.hwdec.is_empty() && self.hwdec != HWDEC_DEFAULT {
            o.insert("hwdec".into(), Value::String(self.hwdec.clone()));
        }
        if !self.audio_passthrough.is_empty() {
            o.insert(
                "audioPassthrough".into(),
                Value::String(self.audio_passthrough.clone()),
            );
        }
        if self.audio_exclusive {
            o.insert("audioExclusive".into(), Value::Bool(true));
        }
        if !self.audio_channels.is_empty() {
            o.insert(
                "audioChannels".into(),
                Value::String(self.audio_channels.clone()),
            );
        }
        if self.disable_gpu_compositing {
            o.insert("disableGpuCompositing".into(), Value::Bool(true));
        }
        if !self.titlebar_theme_color {
            o.insert("titlebarThemeColor".into(), Value::Bool(false));
        }
        if !self.transparent_titlebar {
            o.insert("transparentTitlebar".into(), Value::Bool(false));
        }
        if !self.log_level.is_empty() {
            o.insert("logLevel".into(), Value::String(self.log_level.clone()));
        }
        if self.force_transcoding {
            o.insert("forceTranscoding".into(), Value::Bool(true));
        }
        if !self.device_name.is_empty() {
            o.insert("deviceName".into(), Value::String(self.device_name.clone()));
        }
        Value::Object(o)
    }

    fn cli_json(&self, platform_default: &str, hwdec_opts: &[String]) -> String {
        let mut o = Map::new();
        if !self.hwdec.is_empty() {
            o.insert("hwdec".into(), Value::String(self.hwdec.clone()));
        }
        if !self.audio_passthrough.is_empty() {
            o.insert(
                "audioPassthrough".into(),
                Value::String(self.audio_passthrough.clone()),
            );
        }
        if self.audio_exclusive {
            o.insert("audioExclusive".into(), Value::Bool(true));
        }
        if !self.audio_channels.is_empty() {
            o.insert(
                "audioChannels".into(),
                Value::String(self.audio_channels.clone()),
            );
        }
        if self.disable_gpu_compositing {
            o.insert("disableGpuCompositing".into(), Value::Bool(true));
        }
        if !self.titlebar_theme_color {
            o.insert("titlebarThemeColor".into(), Value::Bool(false));
        }
        if !self.transparent_titlebar {
            o.insert("transparentTitlebar".into(), Value::Bool(false));
        }
        if !self.log_level.is_empty() {
            o.insert("logLevel".into(), Value::String(self.log_level.clone()));
        }
        o.insert(
            "forceTranscoding".into(),
            Value::Bool(self.force_transcoding),
        );
        if !self.device_name.is_empty() {
            o.insert("deviceName".into(), Value::String(self.device_name.clone()));
        }
        o.insert(
            "deviceNameDefault".into(),
            Value::String(platform_default.into()),
        );
        let opts: Vec<Value> = hwdec_opts
            .iter()
            .map(|s| Value::String(s.clone()))
            .collect();
        o.insert("hwdecOptions".into(), Value::Array(opts));
        serde_json::to_string(&Value::Object(o)).unwrap_or_default()
    }
}

struct State {
    data: SettingsData,
    path: PathBuf,
}

fn state() -> &'static Mutex<State> {
    STATE.get_or_init(|| {
        Mutex::new(State {
            data: SettingsData::default(),
            path: PathBuf::new(),
        })
    })
}

static STATE: OnceLock<Mutex<State>> = OnceLock::new();
static SAVE_LOCK: Mutex<()> = Mutex::new(());

fn write_atomic(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let dir = path.parent().unwrap_or(Path::new("."));
    let tmp = dir.join(format!(
        ".{}.tmp",
        path.file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| "config".into())
    ));
    {
        let mut f = fs::File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
    }
    fs::rename(&tmp, path)
}

fn save_data(path: &Path, data: &SettingsData) -> bool {
    let v = data.to_json();
    let Ok(mut text) = serde_json::to_string_pretty(&v) else {
        return false;
    };
    text.push('\n');
    let _guard = SAVE_LOCK.lock().unwrap();
    write_atomic(path, text.as_bytes()).is_ok()
}

fn cstr_to_string(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

fn string_to_cstr(s: &str) -> *mut c_char {
    CString::new(s).unwrap_or_default().into_raw()
}

// =====================================================================
// FFI — settings singleton
// =====================================================================

/// Initialize the settings store with the on-disk path. Idempotent: only the
/// first call sets the path; subsequent calls are ignored.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_settings_init(path: *const c_char) {
    let s = cstr_to_string(path);
    let mut st = state().lock().unwrap();
    if st.path.as_os_str().is_empty() {
        st.path = PathBuf::from(s);
    }
}

/// Load settings from the configured path. Missing keys keep their defaults.
/// Returns false if the file is missing or contains invalid JSON.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_settings_load() -> bool {
    let mut st = state().lock().unwrap();
    let path = st.path.clone();
    let Ok(contents) = fs::read_to_string(&path) else {
        return false;
    };
    let Ok(v) = serde_json::from_str::<Value>(&contents) else {
        return false;
    };
    if !v.is_object() {
        return false;
    }
    st.data.overlay_json(&v);
    true
}

/// Serialize current state and atomically write to the configured path.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_settings_save() -> bool {
    let (path, snap) = {
        let st = state().lock().unwrap();
        (st.path.clone(), st.data.clone())
    };
    save_data(&path, &snap)
}

/// Snapshot current state and save on a detached thread. Concurrent saves are
/// serialized by an internal mutex.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_settings_save_async() {
    let (path, snap) = {
        let st = state().lock().unwrap();
        (st.path.clone(), st.data.clone())
    };
    thread::spawn(move || {
        save_data(&path, &snap);
    });
}

/// Free a string previously returned by this module.
///
/// # Safety
/// `s` must be null or a pointer returned by one of the `jfn_settings_*`
/// string-returning functions, freed at most once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_settings_free_string(s: *mut c_char) {
    if !s.is_null() {
        unsafe { drop(CString::from_raw(s)) };
    }
}

macro_rules! string_getter {
    ($name:ident, $field:ident) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn $name() -> *mut c_char {
            let st = state().lock().unwrap();
            string_to_cstr(&st.data.$field)
        }
    };
}

macro_rules! string_setter {
    ($name:ident, $field:ident) => {
        /// # Safety
        /// `v` must be null or a valid NUL-terminated C string.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(v: *const c_char) {
            let s = cstr_to_string(v);
            let mut st = state().lock().unwrap();
            st.data.$field = s;
        }
    };
}

macro_rules! bool_getter {
    ($name:ident, $field:ident) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn $name() -> bool {
            state().lock().unwrap().data.$field
        }
    };
}

macro_rules! bool_setter {
    ($name:ident, $field:ident) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn $name(v: bool) {
            state().lock().unwrap().data.$field = v;
        }
    };
}

string_getter!(jfn_settings_get_server_url, server_url);
string_setter!(jfn_settings_set_server_url, server_url);
string_getter!(jfn_settings_get_hwdec, hwdec);
string_setter!(jfn_settings_set_hwdec, hwdec);
string_getter!(jfn_settings_get_audio_passthrough, audio_passthrough);
string_setter!(jfn_settings_set_audio_passthrough, audio_passthrough);
string_getter!(jfn_settings_get_audio_channels, audio_channels);
string_setter!(jfn_settings_set_audio_channels, audio_channels);
string_getter!(jfn_settings_get_log_level, log_level);
string_setter!(jfn_settings_set_log_level, log_level);
string_getter!(jfn_settings_get_device_name, device_name);

/// Setter for device_name. Trims and collapses whitespace, truncates to the
/// server's 64-char DeviceName column limit, and clears the override when the
/// result matches `platform_default` (so hostname changes propagate
/// automatically on the next launch).
///
/// # Safety
/// `v` and `platform_default` must each be null or a valid NUL-terminated C
/// string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_settings_set_device_name(
    v: *const c_char,
    platform_default: *const c_char,
) {
    let raw = cstr_to_string(v);
    let platform = cstr_to_string(platform_default);
    let cleaned = normalize_device_name(&raw, &platform);
    let mut st = state().lock().unwrap();
    st.data.device_name = cleaned;
}

fn normalize_device_name(raw: &str, platform_default: &str) -> String {
    // Server's auth header parser preserves whitespace verbatim, so " foo "
    // would round-trip into the Devices table.
    let mut trimmed = String::with_capacity(raw.len());
    let mut in_space = true;
    for c in raw.chars() {
        let ws = matches!(c, ' ' | '\t' | '\r' | '\n' | '\u{0b}' | '\u{0c}');
        if ws {
            if !in_space {
                trimmed.push(' ');
            }
            in_space = true;
        } else {
            trimmed.push(c);
            in_space = false;
        }
    }
    if trimmed.ends_with(' ') {
        trimmed.pop();
    }
    if trimmed.len() > DEVICE_NAME_MAX {
        trimmed.truncate(DEVICE_NAME_MAX);
    }
    if trimmed == platform_default {
        trimmed.clear();
    }
    trimmed
}

bool_getter!(jfn_settings_get_audio_exclusive, audio_exclusive);
bool_setter!(jfn_settings_set_audio_exclusive, audio_exclusive);
bool_getter!(jfn_settings_get_disable_gpu_compositing, disable_gpu_compositing);
bool_setter!(jfn_settings_set_disable_gpu_compositing, disable_gpu_compositing);
bool_getter!(jfn_settings_get_titlebar_theme_color, titlebar_theme_color);
bool_setter!(jfn_settings_set_titlebar_theme_color, titlebar_theme_color);
bool_getter!(jfn_settings_get_transparent_titlebar, transparent_titlebar);
bool_setter!(jfn_settings_set_transparent_titlebar, transparent_titlebar);
bool_getter!(jfn_settings_get_force_transcoding, force_transcoding);
bool_setter!(jfn_settings_set_force_transcoding, force_transcoding);

/// Copy the window geometry into `out`.
///
/// # Safety
/// `out` must be non-null and point to writable storage for a
/// [`JfnWindowGeometry`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_settings_get_window_geometry(out: *mut JfnWindowGeometry) {
    if out.is_null() {
        return;
    }
    let g = state().lock().unwrap().data.window;
    unsafe { ptr::write(out, g) };
}

/// Overwrite the window geometry from `in_`.
///
/// # Safety
/// `in_` must be non-null and point to a valid [`JfnWindowGeometry`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_settings_set_window_geometry(in_: *const JfnWindowGeometry) {
    if in_.is_null() {
        return;
    }
    let g = unsafe { *in_ };
    state().lock().unwrap().data.window = g;
}

/// Build the CLI-equivalent settings JSON string for injection into the web
/// UI. Caller frees with [`jfn_settings_free_string`].
///
/// # Safety
/// `platform_default` must be a valid NUL-terminated C string. `hwdec_opts`,
/// if non-null, must point to an array of `n_opts` valid NUL-terminated C
/// strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_settings_cli_json(
    platform_default: *const c_char,
    hwdec_opts: *const *const c_char,
    n_opts: usize,
) -> *mut c_char {
    let platform_default = cstr_to_string(platform_default);
    let mut opts: Vec<String> = Vec::with_capacity(n_opts);
    if !hwdec_opts.is_null() {
        for i in 0..n_opts {
            let p = unsafe { *hwdec_opts.add(i) };
            opts.push(cstr_to_string(p));
        }
    }
    let snap = state().lock().unwrap().data.clone();
    let s = snap.cli_json(&platform_default, &opts);
    CString::new(s)
        .map(|c| c.into_raw())
        .unwrap_or(ptr::null_mut())
}

#[cfg(test)]
mod tests {
    use super::normalize_device_name;

    const PLATFORM: &str = "platform-host";

    #[test]
    fn trims_leading_and_trailing_whitespace() {
        assert_eq!(normalize_device_name("  foo  ", PLATFORM), "foo");
        assert_eq!(normalize_device_name("\t\nfoo\r\n", PLATFORM), "foo");
    }

    #[test]
    fn collapses_internal_whitespace_runs() {
        assert_eq!(normalize_device_name("foo  bar", PLATFORM), "foo bar");
        assert_eq!(normalize_device_name("foo\t\tbar", PLATFORM), "foo bar");
        assert_eq!(
            normalize_device_name("foo \t\nbar   baz", PLATFORM),
            "foo bar baz"
        );
    }

    #[test]
    fn whitespace_only_is_empty() {
        assert_eq!(normalize_device_name("   \t\n  ", PLATFORM), "");
    }

    #[test]
    fn preserves_single_internal_spaces() {
        assert_eq!(
            normalize_device_name("Andrew's MacBook Pro", PLATFORM),
            "Andrew's MacBook Pro"
        );
    }

    #[test]
    fn clamps_to_64_chars() {
        let long_name = "x".repeat(100);
        assert_eq!(normalize_device_name(&long_name, PLATFORM), "x".repeat(64));
    }

    #[test]
    fn clamps_after_whitespace_normalization() {
        let padded = format!("  {}  ", "x".repeat(70));
        assert_eq!(normalize_device_name(&padded, PLATFORM).len(), 64);
    }

    #[test]
    fn clears_override_when_value_equals_platform_default() {
        assert_eq!(normalize_device_name(PLATFORM, PLATFORM), "");
    }

    #[test]
    fn clears_override_when_whitespace_padded_default() {
        let padded = format!("  {}  ", PLATFORM);
        assert_eq!(normalize_device_name(&padded, PLATFORM), "");
    }
}

