//! Native-shim injection profiles. Each browser kind ("web", "overlay",
//! "about") declares the JS function list + script list shipped to the
//! renderer via the `extra_info` DictionaryValue. The web profile additionally
//! carries the cached Jellyfin device-profile JSON.
//!
//! Built fresh per-browser-create on the C++ thread that calls
//! `CefBrowserHost::CreateBrowser`. CEF copies the dictionary into the
//! cross-process payload, so we don't hold a long-lived reference.

use cef::{
    CefString, CefStringUserfreeUtf16, DictionaryValue, ImplDictionaryValue, ImplListValue,
    dictionary_value_create, list_value_create, sys,
};
use jfn_platform_abi::WindowDecorations;
use std::os::raw::c_char;
use std::sync::OnceLock;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum NativeFunction {
    PlayerLoad,
    PlayerStop,
    PlayerPause,
    PlayerPlay,
    PlayerSeek,
    PlayerSetVolume,
    PlayerSetMuted,
    PlayerSetSpeed,
    PlayerSetSubtitle,
    PlayerAddSubtitle,
    PlayerSetAudio,
    PlayerAddAudio,
    PlayerSetAudioDelay,
    PlayerSetSubtitleDelay,
    PlayerSetAspectMode,
    PlayerOsdActive,
    OpenConfigDir,
    SaveServerUrl,
    NotifyMetadata,
    NotifyPosition,
    NotifySeek,
    NotifyPlaybackState,
    NotifyArtwork,
    NotifyQueueChange,
    NotifyRateChange,
    AppExit,
    SetSettingValue,
    ThemeColor,
    SetOsdVisible,
    SetCursorVisible,
    ToggleFullscreen,
    GetSavedServerUrl,
    NavigateMain,
    DismissOverlay,
    CheckServerConnectivity,
    CancelServerConnectivity,
    AboutOpenPath,
    AboutDismiss,
    WindowMinimize,
    WindowToggleMaximize,
    WindowClose,
    WindowStartMove,
    WindowStartResize,
    CsdReady,
    MenuItemSelected,
    MenuDismissed,
}

impl NativeFunction {
    fn from_name(name: &str) -> Option<Self> {
        Some(match name {
            "playerLoad" => Self::PlayerLoad,
            "playerStop" => Self::PlayerStop,
            "playerPause" => Self::PlayerPause,
            "playerPlay" => Self::PlayerPlay,
            "playerSeek" => Self::PlayerSeek,
            "playerSetVolume" => Self::PlayerSetVolume,
            "playerSetMuted" => Self::PlayerSetMuted,
            "playerSetSpeed" => Self::PlayerSetSpeed,
            "playerSetSubtitle" => Self::PlayerSetSubtitle,
            "playerAddSubtitle" => Self::PlayerAddSubtitle,
            "playerSetAudio" => Self::PlayerSetAudio,
            "playerAddAudio" => Self::PlayerAddAudio,
            "playerSetAudioDelay" => Self::PlayerSetAudioDelay,
            "playerSetSubtitleDelay" => Self::PlayerSetSubtitleDelay,
            "playerSetAspectMode" => Self::PlayerSetAspectMode,
            "playerOsdActive" => Self::PlayerOsdActive,
            "openConfigDir" => Self::OpenConfigDir,
            "saveServerUrl" => Self::SaveServerUrl,
            "notifyMetadata" => Self::NotifyMetadata,
            "notifyPosition" => Self::NotifyPosition,
            "notifySeek" => Self::NotifySeek,
            "notifyPlaybackState" => Self::NotifyPlaybackState,
            "notifyArtwork" => Self::NotifyArtwork,
            "notifyQueueChange" => Self::NotifyQueueChange,
            "notifyRateChange" => Self::NotifyRateChange,
            "appExit" => Self::AppExit,
            "setSettingValue" => Self::SetSettingValue,
            "themeColor" => Self::ThemeColor,
            "setOsdVisible" => Self::SetOsdVisible,
            "setCursorVisible" => Self::SetCursorVisible,
            "toggleFullscreen" => Self::ToggleFullscreen,
            "getSavedServerUrl" => Self::GetSavedServerUrl,
            "navigateMain" => Self::NavigateMain,
            "dismissOverlay" => Self::DismissOverlay,
            "checkServerConnectivity" => Self::CheckServerConnectivity,
            "cancelServerConnectivity" => Self::CancelServerConnectivity,
            "aboutOpenPath" => Self::AboutOpenPath,
            "aboutDismiss" => Self::AboutDismiss,
            "windowMinimize" => Self::WindowMinimize,
            "windowToggleMaximize" => Self::WindowToggleMaximize,
            "windowClose" => Self::WindowClose,
            "windowStartMove" => Self::WindowStartMove,
            "windowStartResize" => Self::WindowStartResize,
            "csdReady" => Self::CsdReady,
            "menuItemSelected" => Self::MenuItemSelected,
            "menuDismissed" => Self::MenuDismissed,
            _ => return None,
        })
    }

    pub(crate) fn name(self) -> &'static str {
        match self {
            Self::PlayerLoad => "playerLoad",
            Self::PlayerStop => "playerStop",
            Self::PlayerPause => "playerPause",
            Self::PlayerPlay => "playerPlay",
            Self::PlayerSeek => "playerSeek",
            Self::PlayerSetVolume => "playerSetVolume",
            Self::PlayerSetMuted => "playerSetMuted",
            Self::PlayerSetSpeed => "playerSetSpeed",
            Self::PlayerSetSubtitle => "playerSetSubtitle",
            Self::PlayerAddSubtitle => "playerAddSubtitle",
            Self::PlayerSetAudio => "playerSetAudio",
            Self::PlayerAddAudio => "playerAddAudio",
            Self::PlayerSetAudioDelay => "playerSetAudioDelay",
            Self::PlayerSetSubtitleDelay => "playerSetSubtitleDelay",
            Self::PlayerSetAspectMode => "playerSetAspectMode",
            Self::PlayerOsdActive => "playerOsdActive",
            Self::OpenConfigDir => "openConfigDir",
            Self::SaveServerUrl => "saveServerUrl",
            Self::NotifyMetadata => "notifyMetadata",
            Self::NotifyPosition => "notifyPosition",
            Self::NotifySeek => "notifySeek",
            Self::NotifyPlaybackState => "notifyPlaybackState",
            Self::NotifyArtwork => "notifyArtwork",
            Self::NotifyQueueChange => "notifyQueueChange",
            Self::NotifyRateChange => "notifyRateChange",
            Self::AppExit => "appExit",
            Self::SetSettingValue => "setSettingValue",
            Self::ThemeColor => "themeColor",
            Self::SetOsdVisible => "setOsdVisible",
            Self::SetCursorVisible => "setCursorVisible",
            Self::ToggleFullscreen => "toggleFullscreen",
            Self::GetSavedServerUrl => "getSavedServerUrl",
            Self::NavigateMain => "navigateMain",
            Self::DismissOverlay => "dismissOverlay",
            Self::CheckServerConnectivity => "checkServerConnectivity",
            Self::CancelServerConnectivity => "cancelServerConnectivity",
            Self::AboutOpenPath => "aboutOpenPath",
            Self::AboutDismiss => "aboutDismiss",
            Self::WindowMinimize => "windowMinimize",
            Self::WindowToggleMaximize => "windowToggleMaximize",
            Self::WindowClose => "windowClose",
            Self::WindowStartMove => "windowStartMove",
            Self::WindowStartResize => "windowStartResize",
            Self::CsdReady => "csdReady",
            Self::MenuItemSelected => "menuItemSelected",
            Self::MenuDismissed => "menuDismissed",
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum InjectedScript {
    NativeShim,
    MpvPlayerBase,
    MpvVideoPlayer,
    MpvAudioPlayer,
    InputPlugin,
    ClientSettings,
    Csd,
    ContextMenu,
}

impl InjectedScript {
    fn from_name(name: &str) -> Option<Self> {
        Some(match name {
            "native-shim.js" => Self::NativeShim,
            "mpv-player-base.js" => Self::MpvPlayerBase,
            "mpv-video-player.js" => Self::MpvVideoPlayer,
            "mpv-audio-player.js" => Self::MpvAudioPlayer,
            "input-plugin.js" => Self::InputPlugin,
            "client-settings.js" => Self::ClientSettings,
            "csd.js" => Self::Csd,
            "context-menu.js" => Self::ContextMenu,
            _ => return None,
        })
    }

    pub(crate) fn file_name(self) -> &'static str {
        match self {
            Self::NativeShim => "native-shim.js",
            Self::MpvPlayerBase => "mpv-player-base.js",
            Self::MpvVideoPlayer => "mpv-video-player.js",
            Self::MpvAudioPlayer => "mpv-audio-player.js",
            Self::InputPlugin => "input-plugin.js",
            Self::ClientSettings => "client-settings.js",
            Self::Csd => "csd.js",
            Self::ContextMenu => "context-menu.js",
        }
    }
}

const WEB_FUNCTIONS: &[NativeFunction] = &[
    NativeFunction::PlayerLoad,
    NativeFunction::PlayerStop,
    NativeFunction::PlayerPause,
    NativeFunction::PlayerPlay,
    NativeFunction::PlayerSeek,
    NativeFunction::PlayerSetVolume,
    NativeFunction::PlayerSetMuted,
    NativeFunction::PlayerSetSpeed,
    NativeFunction::PlayerSetSubtitle,
    NativeFunction::PlayerAddSubtitle,
    NativeFunction::PlayerSetAudio,
    NativeFunction::PlayerAddAudio,
    NativeFunction::PlayerSetAudioDelay,
    NativeFunction::PlayerSetSubtitleDelay,
    NativeFunction::PlayerSetAspectMode,
    NativeFunction::PlayerOsdActive,
    NativeFunction::OpenConfigDir,
    NativeFunction::SaveServerUrl,
    NativeFunction::NotifyMetadata,
    NativeFunction::NotifyPosition,
    NativeFunction::NotifySeek,
    NativeFunction::NotifyPlaybackState,
    NativeFunction::NotifyArtwork,
    NativeFunction::NotifyQueueChange,
    NativeFunction::NotifyRateChange,
    NativeFunction::AppExit,
    NativeFunction::SetSettingValue,
    NativeFunction::ThemeColor,
    NativeFunction::SetOsdVisible,
    NativeFunction::SetCursorVisible,
    NativeFunction::ToggleFullscreen,
];

const WEB_SCRIPTS: &[InjectedScript] = &[
    InjectedScript::NativeShim,
    InjectedScript::MpvPlayerBase,
    InjectedScript::MpvVideoPlayer,
    InjectedScript::MpvAudioPlayer,
    InjectedScript::InputPlugin,
    InjectedScript::ClientSettings,
];

const OVERLAY_FUNCTIONS: &[NativeFunction] = &[
    NativeFunction::GetSavedServerUrl,
    NativeFunction::SaveServerUrl,
    NativeFunction::NavigateMain,
    NativeFunction::DismissOverlay,
    NativeFunction::CheckServerConnectivity,
    NativeFunction::CancelServerConnectivity,
];

const ABOUT_FUNCTIONS: &[NativeFunction] =
    &[NativeFunction::AboutOpenPath, NativeFunction::AboutDismiss];

const WINDOW_FUNCTIONS: &[NativeFunction] = &[
    NativeFunction::WindowMinimize,
    NativeFunction::WindowToggleMaximize,
    NativeFunction::WindowClose,
    NativeFunction::WindowStartMove,
    NativeFunction::WindowStartResize,
    NativeFunction::CsdReady,
];

const FUNCTIONS_KEY: &str = "functions";
const SCRIPTS_KEY: &str = "scripts";
const DEVICE_PROFILE_JSON_KEY: &str = "device_profile_json";
const SHARED_TEXTURES_ENABLED_KEY: &str = "shared_textures_enabled";
const WINDOW_DECORATIONS_KEY: &str = "window_decorations";

static DEVICE_PROFILE_JSON: OnceLock<String> = OnceLock::new();

#[derive(Clone, Debug)]
pub(crate) struct ExtraInfo {
    functions: Vec<NativeFunction>,
    scripts: Vec<InjectedScript>,
    device_profile_json: Option<String>,
    shared_textures_enabled: bool,
    // Effective decoration mode, resolved by the browser process (the renderer
    // has no Platform on Linux to resolve it itself). Drives only the settings UI.
    window_decorations: Option<WindowDecorations>,
}

impl ExtraInfo {
    pub(crate) fn from_dictionary(dict: DictionaryValue) -> Self {
        Self {
            functions: read_native_functions(&dict),
            scripts: read_injected_scripts(&dict),
            device_profile_json: read_string(&dict, DEVICE_PROFILE_JSON_KEY),
            shared_textures_enabled: read_bool(&dict, SHARED_TEXTURES_ENABLED_KEY),
            window_decorations: read_string(&dict, WINDOW_DECORATIONS_KEY)
                .as_deref()
                .and_then(WindowDecorations::parse),
        }
    }

    pub(crate) fn into_dictionary(self) -> Option<DictionaryValue> {
        let dict = dictionary_value_create()?;
        write_native_functions(&dict, &self.functions)?;
        write_injected_scripts(&dict, &self.scripts)?;
        dict.set_bool(
            Some(&CefString::from(SHARED_TEXTURES_ENABLED_KEY)),
            if self.shared_textures_enabled { 1 } else { 0 },
        );
        if let Some(json) = self.device_profile_json {
            dict.set_string(
                Some(&CefString::from(DEVICE_PROFILE_JSON_KEY)),
                Some(&CefString::from(json.as_str())),
            );
        }
        if let Some(wd) = self.window_decorations {
            dict.set_string(
                Some(&CefString::from(WINDOW_DECORATIONS_KEY)),
                Some(&CefString::from(wd.as_str())),
            );
        }
        Some(dict)
    }

    pub(crate) fn functions(&self) -> &[NativeFunction] {
        &self.functions
    }

    pub(crate) fn scripts(&self) -> &[InjectedScript] {
        &self.scripts
    }

    pub(crate) fn device_profile_json(&self) -> Option<&str> {
        self.device_profile_json.as_deref()
    }

    pub(crate) fn shared_textures_enabled(&self) -> bool {
        self.shared_textures_enabled
    }

    pub(crate) fn window_decorations(&self) -> Option<&'static str> {
        self.window_decorations.map(WindowDecorations::as_str)
    }
}

fn read_native_functions(dict: &DictionaryValue) -> Vec<NativeFunction> {
    read_typed_list(dict, FUNCTIONS_KEY, NativeFunction::from_name)
}

fn read_injected_scripts(dict: &DictionaryValue) -> Vec<InjectedScript> {
    read_typed_list(dict, SCRIPTS_KEY, InjectedScript::from_name)
}

fn read_typed_list<T>(
    dict: &DictionaryValue,
    key: &str,
    parse: impl Fn(&str) -> Option<T>,
) -> Vec<T> {
    let Some(list) = dict.list(Some(&CefString::from(key))) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for i in 0..list.size() {
        let value = userfree_to_string(&list.string(i));
        if let Some(value) = parse(&value) {
            out.push(value);
        }
    }
    out
}

fn read_string(dict: &DictionaryValue, key: &str) -> Option<String> {
    let key = CefString::from(key);
    if dict.has_key(Some(&key)) == 1 {
        Some(userfree_to_string(&dict.string(Some(&key))))
    } else {
        None
    }
}

fn read_bool(dict: &DictionaryValue, key: &str) -> bool {
    let key = CefString::from(key);
    dict.has_key(Some(&key)) == 1 && dict.bool(Some(&key)) == 1
}

fn write_native_functions(dict: &DictionaryValue, functions: &[NativeFunction]) -> Option<()> {
    write_string_list(dict, FUNCTIONS_KEY, functions.iter().map(|f| f.name()))
}

fn write_injected_scripts(dict: &DictionaryValue, scripts: &[InjectedScript]) -> Option<()> {
    write_string_list(dict, SCRIPTS_KEY, scripts.iter().map(|s| s.file_name()))
}

fn write_string_list<'a>(
    dict: &DictionaryValue,
    key: &str,
    values: impl IntoIterator<Item = &'a str>,
) -> Option<()> {
    let mut list = list_value_create()?;
    for (idx, value) in values.into_iter().enumerate() {
        list.set_string(idx, Some(&CefString::from(value)));
    }
    dict.set_list(Some(&CefString::from(key)), Some(&mut list));
    Some(())
}

fn userfree_to_string(s: &CefStringUserfreeUtf16) -> String {
    let raw: Option<&sys::_cef_string_utf16_t> = s.into();
    raw.map(|r| {
        if r.str_.is_null() || r.length == 0 {
            String::new()
        } else {
            let slice = unsafe { std::slice::from_raw_parts(r.str_, r.length) };
            String::from_utf16_lossy(slice)
        }
    })
    .unwrap_or_default()
}

/// Set the cached Jellyfin device-profile JSON. Called once at startup
/// after mpv capabilities are queried. Returns silently if already set.
///
/// # Safety
/// `json_utf8` must reference `len` valid UTF-8 bytes, or be null.
pub unsafe fn jfn_cef_set_device_profile_json(json_utf8: *const c_char, len: usize) {
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

fn build_extra_info(
    functions: &[NativeFunction],
    scripts: &[InjectedScript],
    add_ctx_menu: bool,
    add_window: bool,
    shared_textures_enabled: bool,
) -> ExtraInfo {
    let mut functions = functions.to_vec();
    if add_window {
        functions.extend_from_slice(WINDOW_FUNCTIONS);
    }
    if add_ctx_menu {
        functions.extend_from_slice(&[
            NativeFunction::MenuItemSelected,
            NativeFunction::MenuDismissed,
        ]);
    }

    let mut scripts = scripts.to_vec();
    if add_window {
        scripts.push(InjectedScript::Csd);
    }
    if add_ctx_menu {
        scripts.push(InjectedScript::ContextMenu);
    }

    ExtraInfo {
        functions,
        scripts,
        device_profile_json: None,
        shared_textures_enabled,
        window_decorations: None,
    }
}

pub(crate) fn build_for_kind(
    kind: &str,
    add_ctx_menu: bool,
    shared_textures_enabled: bool,
) -> Option<ExtraInfo> {
    match kind {
        "web" => {
            let mut extra_info = build_extra_info(
                WEB_FUNCTIONS,
                WEB_SCRIPTS,
                add_ctx_menu,
                true,
                shared_textures_enabled,
            );
            if let Some(json) = DEVICE_PROFILE_JSON.get()
                && !json.is_empty()
            {
                extra_info.device_profile_json = Some(json.clone());
            }
            // Resolve the effective decoration mode here (the Platform is
            // installed in the browser process) and ship it to the renderer,
            // which has no Platform on Linux. Used only for the settings UI.
            extra_info.window_decorations = Some(jfn_config::window_decorations_mode());
            Some(extra_info)
        }
        "overlay" => Some(build_extra_info(
            OVERLAY_FUNCTIONS,
            &[],
            add_ctx_menu,
            true,
            shared_textures_enabled,
        )),
        "about" => Some(build_extra_info(
            ABOUT_FUNCTIONS,
            &[],
            add_ctx_menu,
            true,
            shared_textures_enabled,
        )),
        _ => None,
    }
}
