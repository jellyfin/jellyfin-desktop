//! Backing store for the C++ `Settings` class. The C++ side owns string
//! buffers passed in; on load we hand back heap-allocated C strings that the
//! caller frees via `jfn_config_free_data`.
//!
//! The on-disk schema mirrors what `src/settings.cpp` wrote with cJSON. Field
//! presence is preserved: missing keys leave the corresponding struct field
//! untouched, and save-time suppression rules (empty strings, sentinel
//! values, zero geometry) match the legacy serializer so existing config
//! files round-trip unchanged.

use serde_json::{Map, Value, json};
use std::ffi::{CStr, CString, c_char};
use std::fs;
use std::io::Write;
use std::path::Path;
use std::ptr;

const DEVICE_NAME_MAX: usize = 64;

#[repr(C)]
pub struct JfnConfigData {
    pub server_url: *mut c_char,
    pub hwdec: *mut c_char,
    pub audio_passthrough: *mut c_char,
    pub audio_channels: *mut c_char,
    pub log_level: *mut c_char,
    pub device_name: *mut c_char,

    pub window_x: i32,
    pub window_y: i32,
    pub window_width: i32,
    pub window_height: i32,
    pub window_logical_width: i32,
    pub window_logical_height: i32,
    pub window_scale: f32,
    pub window_maximized: bool,

    pub audio_exclusive: bool,
    pub disable_gpu_compositing: bool,
    pub titlebar_theme_color: bool,
    pub transparent_titlebar: bool,
    pub force_transcoding: bool,
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

fn drop_cstr(p: *mut c_char) {
    if !p.is_null() {
        unsafe { drop(CString::from_raw(p)) };
    }
}

fn replace_string_field(slot: &mut *mut c_char, s: &str) {
    drop_cstr(*slot);
    *slot = string_to_cstr(s);
}

/// Initialize a [`JfnConfigData`] to default values.
///
/// # Safety
/// `d` must be a valid, properly aligned pointer to writable storage of size
/// at least `sizeof(JfnConfigData)`. Existing contents are overwritten without
/// being dropped — call [`jfn_config_free_data`] first if the struct was
/// previously populated. Passing a null pointer is a no-op.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_config_init_defaults(d: *mut JfnConfigData) {
    if d.is_null() {
        return;
    }
    unsafe {
        ptr::write(
            d,
            JfnConfigData {
                server_url: ptr::null_mut(),
                hwdec: ptr::null_mut(),
                audio_passthrough: ptr::null_mut(),
                audio_channels: ptr::null_mut(),
                log_level: ptr::null_mut(),
                device_name: ptr::null_mut(),
                window_x: -1,
                window_y: -1,
                window_width: 0,
                window_height: 0,
                window_logical_width: 0,
                window_logical_height: 0,
                window_scale: 0.0,
                window_maximized: false,
                audio_exclusive: false,
                disable_gpu_compositing: false,
                titlebar_theme_color: true,
                transparent_titlebar: true,
                force_transcoding: false,
            },
        );
    }
}

/// Free the heap-allocated C strings inside a [`JfnConfigData`] and null the
/// pointers. Numeric/bool fields are untouched.
///
/// # Safety
/// `d` must point to a valid `JfnConfigData` whose string fields were either
/// null or allocated by this crate (i.e. populated by [`jfn_config_load`] or
/// [`jfn_config_init_defaults`]). Strings borrowed from the caller must not
/// be passed here. Passing a null pointer is a no-op.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_config_free_data(d: *mut JfnConfigData) {
    if d.is_null() {
        return;
    }
    unsafe {
        let d = &mut *d;
        drop_cstr(d.server_url);
        drop_cstr(d.hwdec);
        drop_cstr(d.audio_passthrough);
        drop_cstr(d.audio_channels);
        drop_cstr(d.log_level);
        drop_cstr(d.device_name);
        d.server_url = ptr::null_mut();
        d.hwdec = ptr::null_mut();
        d.audio_passthrough = ptr::null_mut();
        d.audio_channels = ptr::null_mut();
        d.log_level = ptr::null_mut();
        d.device_name = ptr::null_mut();
    }
}

fn get_str<'a>(v: &'a Value, k: &str) -> Option<&'a str> {
    v.get(k).and_then(Value::as_str)
}
fn get_bool(v: &Value, k: &str) -> Option<bool> {
    v.get(k).and_then(Value::as_bool)
}
fn get_i32(v: &Value, k: &str) -> Option<i32> {
    v.get(k).and_then(Value::as_i64).map(|n| n as i32)
}
fn get_f32(v: &Value, k: &str) -> Option<f32> {
    v.get(k).and_then(Value::as_f64).map(|n| n as f32)
}

/// Parse `path` as JSON and overlay any present keys onto `out`. Missing
/// keys leave the corresponding field unchanged.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string. `out` must point to a
/// `JfnConfigData` previously initialized via [`jfn_config_init_defaults`];
/// any existing string fields will be replaced and freed when overwritten.
/// Returns false (leaving `out` unchanged) if either pointer is null, the
/// file is missing, or the JSON is invalid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_config_load(path: *const c_char, out: *mut JfnConfigData) -> bool {
    if path.is_null() || out.is_null() {
        return false;
    }
    let path = cstr_to_string(path);
    let Ok(contents) = fs::read_to_string(&path) else {
        return false;
    };
    let Ok(v) = serde_json::from_str::<Value>(&contents) else {
        return false;
    };
    if !v.is_object() {
        return false;
    }
    let d = unsafe { &mut *out };

    if let Some(s) = get_str(&v, "serverUrl") {
        replace_string_field(&mut d.server_url, s);
    }
    if let Some(s) = get_str(&v, "hwdec") {
        replace_string_field(&mut d.hwdec, s);
    }
    if let Some(s) = get_str(&v, "audioPassthrough") {
        replace_string_field(&mut d.audio_passthrough, s);
    }
    if let Some(s) = get_str(&v, "audioChannels") {
        replace_string_field(&mut d.audio_channels, s);
    }
    if let Some(s) = get_str(&v, "logLevel") {
        replace_string_field(&mut d.log_level, s);
    }
    if let Some(s) = get_str(&v, "deviceName") {
        let mut s = s.to_string();
        if s.len() > DEVICE_NAME_MAX {
            s.truncate(DEVICE_NAME_MAX);
        }
        replace_string_field(&mut d.device_name, &s);
    }

    if let Some(n) = get_i32(&v, "windowWidth") {
        d.window_width = n;
    }
    if let Some(n) = get_i32(&v, "windowHeight") {
        d.window_height = n;
    }
    if let Some(n) = get_i32(&v, "windowLogicalWidth") {
        d.window_logical_width = n;
    }
    if let Some(n) = get_i32(&v, "windowLogicalHeight") {
        d.window_logical_height = n;
    }
    if let Some(n) = get_f32(&v, "windowScale") {
        d.window_scale = n;
    }
    if let Some(n) = get_i32(&v, "windowX") {
        d.window_x = n;
    }
    if let Some(n) = get_i32(&v, "windowY") {
        d.window_y = n;
    }
    if let Some(b) = get_bool(&v, "windowMaximized") {
        d.window_maximized = b;
    }
    if let Some(b) = get_bool(&v, "audioExclusive") {
        d.audio_exclusive = b;
    }
    if let Some(b) = get_bool(&v, "disableGpuCompositing") {
        d.disable_gpu_compositing = b;
    }
    if let Some(b) = get_bool(&v, "titlebarThemeColor") {
        d.titlebar_theme_color = b;
    }
    if let Some(b) = get_bool(&v, "transparentTitlebar") {
        d.transparent_titlebar = b;
    }
    if let Some(b) = get_bool(&v, "forceTranscoding") {
        d.force_transcoding = b;
    }

    true
}

fn data_to_json(d: &JfnConfigData, hwdec_default: &str) -> Value {
    let mut o = Map::new();

    o.insert(
        "serverUrl".into(),
        Value::String(cstr_to_string(d.server_url)),
    );

    if d.window_width > 0 && d.window_height > 0 {
        o.insert("windowWidth".into(), json!(d.window_width));
        o.insert("windowHeight".into(), json!(d.window_height));
    }
    if d.window_logical_width > 0 && d.window_logical_height > 0 {
        o.insert("windowLogicalWidth".into(), json!(d.window_logical_width));
        o.insert("windowLogicalHeight".into(), json!(d.window_logical_height));
    }
    if d.window_scale > 0.0 {
        o.insert("windowScale".into(), json!(d.window_scale));
    }
    if d.window_x >= 0 && d.window_y >= 0 {
        o.insert("windowX".into(), json!(d.window_x));
        o.insert("windowY".into(), json!(d.window_y));
    }
    o.insert("windowMaximized".into(), Value::Bool(d.window_maximized));

    let hwdec = cstr_to_string(d.hwdec);
    if !hwdec.is_empty() && hwdec != hwdec_default {
        o.insert("hwdec".into(), Value::String(hwdec));
    }
    let ap = cstr_to_string(d.audio_passthrough);
    if !ap.is_empty() {
        o.insert("audioPassthrough".into(), Value::String(ap));
    }
    if d.audio_exclusive {
        o.insert("audioExclusive".into(), Value::Bool(true));
    }
    let ac = cstr_to_string(d.audio_channels);
    if !ac.is_empty() {
        o.insert("audioChannels".into(), Value::String(ac));
    }
    if d.disable_gpu_compositing {
        o.insert("disableGpuCompositing".into(), Value::Bool(true));
    }
    if !d.titlebar_theme_color {
        o.insert("titlebarThemeColor".into(), Value::Bool(false));
    }
    if !d.transparent_titlebar {
        o.insert("transparentTitlebar".into(), Value::Bool(false));
    }
    let ll = cstr_to_string(d.log_level);
    if !ll.is_empty() {
        o.insert("logLevel".into(), Value::String(ll));
    }
    if d.force_transcoding {
        o.insert("forceTranscoding".into(), Value::Bool(true));
    }
    let dn = cstr_to_string(d.device_name);
    if !dn.is_empty() {
        o.insert("deviceName".into(), Value::String(dn));
    }

    Value::Object(o)
}

/// Serialize `in_` to JSON and atomically write to `path`.
///
/// # Safety
/// `path` and `hwdec_default` must be valid NUL-terminated C strings. `in_`
/// must point to a valid `JfnConfigData` whose string fields are either null
/// or valid NUL-terminated C strings. The Rust side only reads through these
/// pointers and does not take ownership. Returns false on I/O or
/// serialization error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_config_save(
    path: *const c_char,
    in_: *const JfnConfigData,
    hwdec_default: *const c_char,
) -> bool {
    if path.is_null() || in_.is_null() {
        return false;
    }
    let path = cstr_to_string(path);
    let hwdec_default = cstr_to_string(hwdec_default);
    let d = unsafe { &*in_ };
    let v = data_to_json(d, &hwdec_default);
    let Ok(mut text) = serde_json::to_string_pretty(&v) else {
        return false;
    };
    text.push('\n');
    write_atomic(Path::new(&path), text.as_bytes()).is_ok()
}

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

/// Build the CLI-equivalent settings JSON string injected into the web UI.
/// Caller frees the returned string with [`jfn_config_free_string`].
///
/// # Safety
/// `in_` must point to a valid `JfnConfigData` (see [`jfn_config_save`] for
/// string field requirements). `platform_default` must be a valid
/// NUL-terminated C string. `hwdec_opts`, if non-null, must point to an
/// array of `n_opts` valid NUL-terminated C strings. Returns null on
/// serialization failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_config_cli_json(
    in_: *const JfnConfigData,
    platform_default: *const c_char,
    hwdec_opts: *const *const c_char,
    n_opts: usize,
) -> *mut c_char {
    if in_.is_null() {
        return ptr::null_mut();
    }
    let d = unsafe { &*in_ };
    let mut o = Map::new();

    let hwdec = cstr_to_string(d.hwdec);
    if !hwdec.is_empty() {
        o.insert("hwdec".into(), Value::String(hwdec));
    }
    let ap = cstr_to_string(d.audio_passthrough);
    if !ap.is_empty() {
        o.insert("audioPassthrough".into(), Value::String(ap));
    }
    if d.audio_exclusive {
        o.insert("audioExclusive".into(), Value::Bool(true));
    }
    let ac = cstr_to_string(d.audio_channels);
    if !ac.is_empty() {
        o.insert("audioChannels".into(), Value::String(ac));
    }
    if d.disable_gpu_compositing {
        o.insert("disableGpuCompositing".into(), Value::Bool(true));
    }
    if !d.titlebar_theme_color {
        o.insert("titlebarThemeColor".into(), Value::Bool(false));
    }
    if !d.transparent_titlebar {
        o.insert("transparentTitlebar".into(), Value::Bool(false));
    }
    let ll = cstr_to_string(d.log_level);
    if !ll.is_empty() {
        o.insert("logLevel".into(), Value::String(ll));
    }
    o.insert("forceTranscoding".into(), Value::Bool(d.force_transcoding));
    let dn = cstr_to_string(d.device_name);
    if !dn.is_empty() {
        o.insert("deviceName".into(), Value::String(dn));
    }
    o.insert(
        "deviceNameDefault".into(),
        Value::String(cstr_to_string(platform_default)),
    );

    let mut opts = Vec::with_capacity(n_opts);
    if !hwdec_opts.is_null() {
        for i in 0..n_opts {
            let p = unsafe { *hwdec_opts.add(i) };
            opts.push(Value::String(cstr_to_string(p)));
        }
    }
    o.insert("hwdecOptions".into(), Value::Array(opts));

    let text = match serde_json::to_string(&Value::Object(o)) {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };
    CString::new(text)
        .map(|c| c.into_raw())
        .unwrap_or(ptr::null_mut())
}

/// Free a string previously returned by [`jfn_config_cli_json`].
///
/// # Safety
/// `s` must either be null or a pointer previously returned by this crate
/// (e.g. from [`jfn_config_cli_json`]). Each pointer may only be freed once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_config_free_string(s: *mut c_char) {
    drop_cstr(s);
}

/// Validate that a Jellyfin /System/Info/Public response body is a JSON
/// object with a non-empty string `Id` field. Used at server-probe time to
/// distinguish real Jellyfin servers from arbitrary HTTP responders that
/// happen to return 200 OK.
///
/// # Safety
/// `body` must point to at least `len` bytes of readable memory (need not be
/// NUL-terminated). Passing a null pointer or zero length returns false.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_jellyfin_is_valid_public_info(
    body: *const c_char,
    len: usize,
) -> bool {
    if body.is_null() || len == 0 {
        return false;
    }
    let slice = unsafe { std::slice::from_raw_parts(body as *const u8, len) };
    let Ok(v) = serde_json::from_slice::<Value>(slice) else {
        return false;
    };
    let Some(o) = v.as_object() else { return false };
    o.get("Id")
        .and_then(Value::as_str)
        .map(|s| !s.is_empty())
        .unwrap_or(false)
}
