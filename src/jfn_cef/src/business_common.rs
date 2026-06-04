//! Shared helpers for the three `business_*` modules.
//!
//! Two groups, separated by the dividers below:
//!   1. Generic CEF/Rust helpers — could lift into a `cef-rs-helpers` crate.
//!   2. App-specific dispatch — Jellyfin-desktop config wiring.

use std::ffi::CString;

/// Returns true if the supplied `MutexGuard`-bearing `Option` already holds
/// a value — i.e. a singleton `init` is being called twice. Crashes loud in
/// debug; logs + returns true in release so a programmer error never
/// escalates. `caller` names the offending init for the log line.
pub(crate) fn reject_double_init<T>(slot: &Option<T>, caller: &str) -> bool {
    if slot.is_some() {
        debug_assert!(false, "{caller} called twice");
        jfn_logging::log(
            jfn_logging::CATEGORY_CEF,
            jfn_logging::LEVEL_WARN,
            &format!("{caller} called twice; ignoring"),
        );
        return true;
    }
    false
}

// --- generic Rust/C interop ------------------------------------------------

/// Convert a JS-supplied string into a `CString` for FFI, logging + dropping
/// on interior NUL. `label` names the IPC arm in the warn message so the
/// log line is enough to locate the offending handler.
///
/// Avoids the prior `CString::new(x).unwrap_or_default()` pattern that
/// silently handed `""` to downstream consumers (e.g. mpv).
pub(crate) fn js_cstr_or_warn(label: &str, s: &str) -> Option<CString> {
    match CString::new(s) {
        Ok(c) => Some(c),
        Err(_) => {
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_WARN,
                &format!("{label}: interior NUL in JS string; dropping IPC"),
            );
            None
        }
    }
}

// --- app-specific dispatch -------------------------------------------------

/// `setSettingValue` IPC dispatch. Superset of the keys the overlay and the
/// main web UI send today — both UIs share this single source of truth so
/// new keys land in one place.
pub(crate) fn apply_setting_value(_section: &str, key: &str, value: &str) {
    match key {
        "hwdec" => jfn_config::set_hwdec(value),
        "audioPassthrough" => jfn_config::set_audio_passthrough(value),
        "audioExclusive" => jfn_config::set_audio_exclusive(value == "true"),
        "audioChannels" => jfn_config::set_audio_channels(value),
        "windowDecorations" => jfn_config::set_window_decorations(value),
        "hideScrollbar" => jfn_config::set_hide_scrollbar(value == "true"),
        "logLevel" => jfn_config::set_log_level(value),
        "forceTranscoding" => jfn_config::set_force_transcoding(value == "true"),
        // Pass empty platform_default — Rust setter clears when raw equals
        // the empty string. Neither caller has the live hostname handy here.
        "deviceName" => jfn_config::set_device_name(value, ""),
        _ => jfn_logging::log(
            jfn_logging::CATEGORY_CEF,
            jfn_logging::LEVEL_WARN,
            &format!("Unknown setting key: {_section}.{key}"),
        ),
    }
    jfn_config::settings_save_async();
}
