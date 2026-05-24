//! Native-shim injection profiles. Each browser kind ("web", "overlay",
//! "about") declares the JS function list + script list shipped to the
//! renderer via the `extra_info` DictionaryValue. The web profile additionally
//! carries the cached Jellyfin device-profile JSON.
//!
//! Built fresh per-browser-create on the C++ thread that calls
//! `CefBrowserHost::CreateBrowser`. CEF copies the dictionary into the
//! cross-process payload, so we don't hold a long-lived reference.

use cef::{
    CefString, DictionaryValue, ImplDictionaryValue, ImplListValue, dictionary_value_create,
    list_value_create,
};
use std::os::raw::c_char;
use std::sync::OnceLock;

const WEB_FUNCTIONS: &[&str] = &[
    "playerLoad",
    "playerStop",
    "playerPause",
    "playerPlay",
    "playerSeek",
    "playerSetVolume",
    "playerSetMuted",
    "playerSetSpeed",
    "playerSetSubtitle",
    "playerAddSubtitle",
    "playerSetAudio",
    "playerAddAudio",
    "playerSetAudioDelay",
    "playerSetSubtitleDelay",
    "playerSetAspectMode",
    "playerOsdActive",
    "openConfigDir",
    "saveServerUrl",
    "notifyMetadata",
    "notifyPosition",
    "notifySeek",
    "notifyPlaybackState",
    "notifyArtwork",
    "notifyQueueChange",
    "notifyRateChange",
    "appExit",
    "setSettingValue",
    "themeColor",
    "setOsdVisible",
    "setCursorVisible",
    "toggleFullscreen",
];

const WEB_SCRIPTS: &[&str] = &[
    "native-shim.js",
    "mpv-player-base.js",
    "mpv-video-player.js",
    "mpv-audio-player.js",
    "input-plugin.js",
    "client-settings.js",
];

const OVERLAY_FUNCTIONS: &[&str] = &[
    "getSavedServerUrl",
    "saveServerUrl",
    "navigateMain",
    "dismissOverlay",
    "checkServerConnectivity",
    "cancelServerConnectivity",
    "overlayFadeComplete",
];

const ABOUT_FUNCTIONS: &[&str] = &["aboutOpenPath", "aboutDismiss"];

static DEVICE_PROFILE_JSON: OnceLock<String> = OnceLock::new();

/// Set the cached Jellyfin device-profile JSON. Called once at startup from
/// C++ after mpv capabilities are queried. Returns silently if already set.
pub unsafe extern "C" fn jfn_cef_set_device_profile_json(json_utf8: *const c_char, len: usize) {
    if json_utf8.is_null() || len == 0 {
        return;
    }
    let slice = unsafe { std::slice::from_raw_parts(json_utf8 as *const u8, len) };
    let s = match std::str::from_utf8(slice) {
        Ok(s) => s.to_string(),
        Err(_) => return,
    };
    let _ = DEVICE_PROFILE_JSON.set(s);
}

fn fill_list(funcs: &[&str], scripts: &[&str], add_ctx_menu: bool) -> Option<DictionaryValue> {
    let dict = dictionary_value_create()?;
    let fn_list = list_value_create()?;
    let mut idx = 0;
    for &name in funcs {
        fn_list.set_string(idx, Some(&CefString::from(name)));
        idx += 1;
    }
    if add_ctx_menu {
        fn_list.set_string(idx, Some(&CefString::from("menuItemSelected")));
        idx += 1;
        fn_list.set_string(idx, Some(&CefString::from("menuDismissed")));
    }
    let mut fn_list = fn_list;
    dict.set_list(Some(&CefString::from("functions")), Some(&mut fn_list));

    let script_list = list_value_create()?;
    let mut sidx = 0;
    for &name in scripts {
        script_list.set_string(sidx, Some(&CefString::from(name)));
        sidx += 1;
    }
    if add_ctx_menu {
        script_list.set_string(sidx, Some(&CefString::from("context-menu.js")));
    }
    let mut script_list = script_list;
    dict.set_list(Some(&CefString::from("scripts")), Some(&mut script_list));

    Some(dict)
}

pub fn build_for_kind(kind: &str, add_ctx_menu: bool) -> Option<DictionaryValue> {
    match kind {
        "web" => {
            let dict = fill_list(WEB_FUNCTIONS, WEB_SCRIPTS, add_ctx_menu)?;
            if let Some(json) = DEVICE_PROFILE_JSON.get()
                && !json.is_empty()
            {
                dict.set_string(
                    Some(&CefString::from("device_profile_json")),
                    Some(&CefString::from(json.as_str())),
                );
            }
            Some(dict)
        }
        "overlay" => fill_list(OVERLAY_FUNCTIONS, &[], add_ctx_menu),
        "about" => fill_list(ABOUT_FUNCTIONS, &[], add_ctx_menu),
        _ => None,
    }
}
