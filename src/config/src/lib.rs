//! Settings store. Owns the in-memory state, JSON persistence, and the
//! singleton accessor that the rest of the workspace calls into.
//!
//! On-disk schema is a JSON object with the field names used by
//! [`SettingsData::to_json`]. Missing keys keep their defaults on load; save
//! suppresses fields that are at their default (empty strings, sentinel
//! values, zero geometry) so existing config files round-trip unchanged.

use jfn_platform_abi::WindowDecorations;
use parking_lot::{Condvar, Mutex};
use serde_json::{Map, Value, json};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;
use std::thread::{self, JoinHandle};

const DEVICE_NAME_MAX: usize = 64;
const HWDEC_DEFAULT: &str = "no";

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
    transparent_titlebar: bool,
    force_transcoding: bool,
    window_decorations: Option<WindowDecorations>,
    hide_scrollbar: bool,
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
            transparent_titlebar: true,
            force_transcoding: false,
            window_decorations: None,
            hide_scrollbar: true,
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
        if let Some(b) = v.get("transparentTitlebar").and_then(Value::as_bool) {
            self.transparent_titlebar = b;
        }
        if let Some(b) = v.get("forceTranscoding").and_then(Value::as_bool) {
            self.force_transcoding = b;
        }
        if let Some(d) = v
            .get("windowDecorations")
            .and_then(Value::as_str)
            .and_then(WindowDecorations::parse)
        {
            self.window_decorations = Some(d);
        }
        if let Some(b) = v.get("hideScrollbar").and_then(Value::as_bool) {
            self.hide_scrollbar = b;
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
            o.insert(
                "windowLogicalWidth".into(),
                json!(self.window.logical_width),
            );
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
        o.insert("windowMaximized".into(), Value::Bool(self.window.maximized));
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
        if !self.transparent_titlebar {
            o.insert("transparentTitlebar".into(), Value::Bool(false));
        }
        if !self.log_level.is_empty() {
            o.insert("logLevel".into(), Value::String(self.log_level.clone()));
        }
        if self.force_transcoding {
            o.insert("forceTranscoding".into(), Value::Bool(true));
        }
        if let Some(d) = self.window_decorations {
            o.insert(
                "windowDecorations".into(),
                Value::String(d.as_str().to_string()),
            );
        }
        if !self.hide_scrollbar {
            o.insert("hideScrollbar".into(), Value::Bool(false));
        }
        if !self.device_name.is_empty() {
            o.insert("deviceName".into(), Value::String(self.device_name.clone()));
        }
        Value::Object(o)
    }

    fn cli_json(&self, hwdec_opts: &[String]) -> String {
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
        // windowDecorations is absent: resolving its effective value needs the
        // Platform default, unavailable in the CEF renderer where cli_json runs.
        o.insert("hideScrollbar".into(), Value::Bool(self.hide_scrollbar));
        if !self.device_name.is_empty() {
            o.insert("deviceName".into(), Value::String(self.device_name.clone()));
        }
        o.insert(
            "deviceNameDefault".into(),
            Value::String(default_device_name()),
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

// Single persistent background save worker. save_async() coalesces into
// Pending::data (only the newest snapshot survives); the worker wakes on the
// condvar, writes the latest snapshot, then sleeps. Shutdown drains any queued
// write and joins the thread so nothing is lost at exit.
struct Pending {
    data: Option<SettingsData>,
    path: PathBuf,
    stop: bool,
    started: bool,
}

struct SaveWorker {
    pending: Mutex<Pending>,
    cv: Condvar,
    handle: Mutex<Option<JoinHandle<()>>>,
}

static SAVE_WORKER: OnceLock<SaveWorker> = OnceLock::new();

fn save_worker() -> &'static SaveWorker {
    SAVE_WORKER.get_or_init(|| SaveWorker {
        pending: Mutex::new(Pending {
            data: None,
            path: PathBuf::new(),
            stop: false,
            started: false,
        }),
        cv: Condvar::new(),
        handle: Mutex::new(None),
    })
}

fn save_worker_loop(w: &'static SaveWorker) {
    loop {
        let (data, path) = {
            let mut p = w.pending.lock();
            while p.data.is_none() && !p.stop {
                w.cv.wait(&mut p);
            }
            match p.data.take() {
                Some(d) => (d, p.path.clone()),
                None => return, // stop with nothing pending — drained
            }
        };
        save_data(&path, &data);
    }
}

fn save_data(path: &Path, data: &SettingsData) -> bool {
    let v = data.to_json();
    let Ok(mut text) = serde_json::to_string_pretty(&v) else {
        return false;
    };
    text.push('\n');
    let _guard = SAVE_LOCK.lock();
    jfn_paths::write_atomic(path, text.as_bytes()).is_ok()
}

// =====================================================================
// Public Rust API
// =====================================================================

/// Initialize the settings store with the on-disk path. Idempotent: only the
/// first call sets the path; subsequent calls are ignored.
pub fn settings_init(path: &Path) {
    let mut st = state().lock();
    if st.path.as_os_str().is_empty() {
        st.path = path.to_path_buf();
    }
}

/// Load settings from the configured path. Missing keys keep their defaults.
/// Returns false if the file is missing or contains invalid JSON.
pub fn settings_load() -> bool {
    let mut st = state().lock();
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
pub fn settings_save() -> bool {
    let (path, snap) = {
        let st = state().lock();
        (st.path.clone(), st.data.clone())
    };
    save_data(&path, &snap)
}

/// Snapshot current state and hand it to the background save worker. Repeated
/// calls coalesce: only the most recent snapshot is written. The worker is
/// started lazily on the first call. After [`settings_shutdown_save_worker`]
/// this becomes a no-op.
pub fn settings_save_async() {
    let (path, snap) = {
        let st = state().lock();
        (st.path.clone(), st.data.clone())
    };
    let w = save_worker();
    // Hold `handle` across the spawn so a second caller racing in between
    // `started = true` and the JoinHandle store can't observe a "started"
    // worker before the thread actually exists.
    let mut handle_guard = w.handle.lock();
    let need_spawn = {
        let mut p = w.pending.lock();
        if p.stop {
            return;
        }
        p.data = Some(snap);
        p.path = path;
        if p.started {
            false
        } else {
            p.started = true;
            true
        }
    };
    if need_spawn {
        *handle_guard = Some(thread::spawn(|| save_worker_loop(save_worker())));
    }
    drop(handle_guard);
    w.cv.notify_one();
}

/// Stop the background save worker after draining any pending write. Safe to
/// call if the worker was never started; safe to call multiple times.
pub fn settings_shutdown_save_worker() {
    let Some(w) = SAVE_WORKER.get() else {
        return;
    };
    {
        let mut p = w.pending.lock();
        if p.stop {
            return;
        }
        p.stop = true;
    }
    w.cv.notify_one();
    let handle = w.handle.lock().take();
    if let Some(h) = handle
        && let Err(e) = h.join()
    {
        eprintln!("[config] save worker panicked: {e:?}");
    }
}

macro_rules! string_accessors {
    ($getter:ident, $setter:ident, $field:ident) => {
        pub fn $getter() -> String {
            state().lock().data.$field.clone()
        }
        pub fn $setter(v: &str) {
            state().lock().data.$field = v.to_string();
        }
    };
}

macro_rules! bool_accessors {
    ($getter:ident, $setter:ident, $field:ident) => {
        pub fn $getter() -> bool {
            state().lock().data.$field
        }
        pub fn $setter(v: bool) {
            state().lock().data.$field = v;
        }
    };
}

string_accessors!(server_url, set_server_url, server_url);
string_accessors!(hwdec, set_hwdec, hwdec);
string_accessors!(audio_passthrough, set_audio_passthrough, audio_passthrough);
string_accessors!(audio_channels, set_audio_channels, audio_channels);
string_accessors!(log_level, set_log_level, log_level);

pub fn device_name() -> String {
    state().lock().data.device_name.clone()
}

#[cfg(unix)]
pub fn default_device_name() -> String {
    let mut buf = [0u8; 256];
    let rc = unsafe { libc::gethostname(buf.as_mut_ptr() as *mut _, buf.len()) };
    if rc != 0 {
        return String::new();
    }
    let len = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    let mut s = String::from_utf8_lossy(&buf[..len]).into_owned();
    s.truncate(DEVICE_NAME_MAX);
    s
}

#[cfg(windows)]
pub fn default_device_name() -> String {
    let mut s = std::env::var("COMPUTERNAME").unwrap_or_default();
    s.truncate(DEVICE_NAME_MAX);
    s
}

/// Setter for device_name. Trims and collapses whitespace, truncates to the
/// server's 64-char DeviceName column limit, and clears the override when the
/// result matches `platform_default` (so hostname changes propagate
/// automatically on the next launch).
pub fn set_device_name(raw: &str, platform_default: &str) {
    let cleaned = normalize_device_name(raw, platform_default);
    state().lock().data.device_name = cleaned;
}

bool_accessors!(audio_exclusive, set_audio_exclusive, audio_exclusive);
bool_accessors!(
    disable_gpu_compositing,
    set_disable_gpu_compositing,
    disable_gpu_compositing
);
bool_accessors!(
    transparent_titlebar,
    set_transparent_titlebar,
    transparent_titlebar
);
bool_accessors!(force_transcoding, set_force_transcoding, force_transcoding);
/// The user's explicit decoration choice, unresolved; `None` when unset.
pub fn configured_window_decorations() -> Option<WindowDecorations> {
    state().lock().data.window_decorations
}

/// Browser-process only: falls back to the installed `Platform`, which panics
/// if absent.
pub fn window_decorations_mode() -> WindowDecorations {
    let configured = state().lock().data.window_decorations;
    jfn_platform_abi::get().resolve_window_decorations(configured)
}

pub fn window_decorations() -> String {
    window_decorations_mode().as_str().to_string()
}
pub fn set_window_decorations(v: Option<&str>) {
    state().lock().data.window_decorations = v.and_then(WindowDecorations::parse);
}

/// True when the app draws its own (client-side) titlebar.
pub fn client_side_decorations() -> bool {
    window_decorations_mode() == WindowDecorations::Csd
}
pub fn titlebar_theme_color() -> bool {
    window_decorations_mode() == WindowDecorations::ServerThemed
}
bool_accessors!(hide_scrollbar, set_hide_scrollbar, hide_scrollbar);

pub fn window_geometry() -> JfnWindowGeometry {
    state().lock().data.window
}

pub fn set_window_geometry(g: JfnWindowGeometry) {
    state().lock().data.window = g;
}

pub fn cli_json(hwdec_opts: &[&str]) -> String {
    let snap = state().lock().data.clone();
    let opts: Vec<String> = hwdec_opts.iter().map(|s| (*s).to_string()).collect();
    snap.cli_json(&opts)
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
