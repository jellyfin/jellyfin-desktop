//! Small desktop-owned string catalogue.
//!
//! jellyfin-web keeps its own translations. This module only covers strings
//! that are owned by the desktop shell: CEF chrome, the about panel, and the
//! small client settings page injected by this app.

use serde_json::{Map, Value};

const FALLBACK_LOCALE: &str = "en-us";
const CEF_FALLBACK_LOCALE: &str = "en-US";
const CEF_LOCALES: &[&str] = &[
    "am", "ar", "bg", "bn", "ca", "cs", "da", "de", "el", "en-GB", "en-US", "es", "es-419", "et",
    "fa", "fi", "fil", "fr", "gu", "he", "hi", "hr", "hu", "id", "it", "ja", "kn", "ko", "lt",
    "lv", "ml", "mr", "ms", "nb", "nl", "pl", "pt-BR", "pt-PT", "ro", "ru", "sk", "sl", "sr", "sv",
    "sw", "ta", "te", "th", "tr", "uk", "ur", "vi", "zh-CN", "zh-TW",
];

#[derive(Clone, Copy)]
pub enum StringKey {
    About,
    AboutAppVersion,
    AboutCefVersion,
    AboutConfigDir,
    AboutCurrentLogFile,
    AboutJellyfinDesktop,
    Advanced,
    Audio,
    AudioChannelLayout,
    AudioChannelLayoutHelp,
    AudioExclusive,
    AudioExclusiveHelp,
    AudioPassthrough,
    AudioPassthroughHelp,
    Auto,
    Cancel,
    ChangesRestart,
    ClientSettings,
    Copy,
    Cut,
    Debug,
    DefaultInfo,
    DeviceName,
    DeviceNameHelp,
    Edit,
    Error,
    Exit,
    ForceTranscoding,
    ForceTranscodingHelp,
    HardwareDecoding,
    HardwareDecodingHelp,
    HideJellyfinDesktop,
    HideOthers,
    HideScrollbar,
    HideScrollbarHelp,
    InAppClientSide,
    LogLevel,
    LogLevelHelp,
    MpvConfig,
    OpenMpvConfigDir,
    Paste,
    Playback,
    Quit,
    Redo,
    Reload,
    ResetSavedServer,
    SelectAll,
    Server,
    ShowAll,
    Stereo,
    Surround51,
    Surround71,
    SystemServerSide,
    SystemThemedKde,
    ToggleFullscreen,
    Transcode,
    TransparentTitlebar,
    TransparentTitlebarHelp,
    Undo,
    Verbose,
    Warning,
    WindowDecorations,
    WindowDecorationsHelp,
}

impl StringKey {
    pub const fn id(self) -> &'static str {
        match self {
            Self::About => "about",
            Self::AboutAppVersion => "about.appVersion",
            Self::AboutCefVersion => "about.cefVersion",
            Self::AboutConfigDir => "about.configDir",
            Self::AboutCurrentLogFile => "about.currentLogFile",
            Self::AboutJellyfinDesktop => "about.jellyfinDesktop",
            Self::Advanced => "settings.advanced",
            Self::Audio => "settings.audio",
            Self::AudioChannelLayout => "settings.audioChannelLayout",
            Self::AudioChannelLayoutHelp => "settings.audioChannelLayoutHelp",
            Self::AudioExclusive => "settings.audioExclusive",
            Self::AudioExclusiveHelp => "settings.audioExclusiveHelp",
            Self::AudioPassthrough => "settings.audioPassthrough",
            Self::AudioPassthroughHelp => "settings.audioPassthroughHelp",
            Self::Auto => "settings.auto",
            Self::Cancel => "cancel",
            Self::ChangesRestart => "settings.changesRestart",
            Self::ClientSettings => "settings.clientSettings",
            Self::Copy => "menu.copy",
            Self::Cut => "menu.cut",
            Self::Debug => "settings.debug",
            Self::DefaultInfo => "settings.defaultInfo",
            Self::DeviceName => "settings.deviceName",
            Self::DeviceNameHelp => "settings.deviceNameHelp",
            Self::Edit => "menu.edit",
            Self::Error => "settings.error",
            Self::Exit => "menu.exit",
            Self::ForceTranscoding => "settings.forceTranscoding",
            Self::ForceTranscodingHelp => "settings.forceTranscodingHelp",
            Self::HardwareDecoding => "settings.hardwareDecoding",
            Self::HardwareDecodingHelp => "settings.hardwareDecodingHelp",
            Self::HideJellyfinDesktop => "menu.hideJellyfinDesktop",
            Self::HideOthers => "menu.hideOthers",
            Self::HideScrollbar => "settings.hideScrollbar",
            Self::HideScrollbarHelp => "settings.hideScrollbarHelp",
            Self::InAppClientSide => "settings.inAppClientSide",
            Self::LogLevel => "settings.logLevel",
            Self::LogLevelHelp => "settings.logLevelHelp",
            Self::MpvConfig => "settings.mpvConfig",
            Self::OpenMpvConfigDir => "settings.openMpvConfigDir",
            Self::Paste => "menu.paste",
            Self::Playback => "settings.playback",
            Self::Quit => "menu.quit",
            Self::Redo => "menu.redo",
            Self::Reload => "menu.reload",
            Self::ResetSavedServer => "settings.resetSavedServer",
            Self::SelectAll => "menu.selectAll",
            Self::Server => "settings.server",
            Self::ShowAll => "menu.showAll",
            Self::Stereo => "settings.stereo",
            Self::Surround51 => "settings.5_1Surround",
            Self::Surround71 => "settings.7_1Surround",
            Self::SystemServerSide => "settings.systemServerSide",
            Self::SystemThemedKde => "settings.systemThemedKde",
            Self::ToggleFullscreen => "menu.toggleFullscreen",
            Self::Transcode => "settings.transcode",
            Self::TransparentTitlebar => "settings.transparentTitlebar",
            Self::TransparentTitlebarHelp => "settings.transparentTitlebarHelp",
            Self::Undo => "menu.undo",
            Self::Verbose => "settings.verbose",
            Self::Warning => "settings.warning",
            Self::WindowDecorations => "settings.windowDecorations",
            Self::WindowDecorationsHelp => "settings.windowDecorationsHelp",
        }
    }
}

const ALL_KEYS: &[StringKey] = &[
    StringKey::About,
    StringKey::AboutAppVersion,
    StringKey::AboutCefVersion,
    StringKey::AboutConfigDir,
    StringKey::AboutCurrentLogFile,
    StringKey::AboutJellyfinDesktop,
    StringKey::Advanced,
    StringKey::Audio,
    StringKey::AudioChannelLayout,
    StringKey::AudioChannelLayoutHelp,
    StringKey::AudioExclusive,
    StringKey::AudioExclusiveHelp,
    StringKey::AudioPassthrough,
    StringKey::AudioPassthroughHelp,
    StringKey::Auto,
    StringKey::Cancel,
    StringKey::ChangesRestart,
    StringKey::ClientSettings,
    StringKey::Copy,
    StringKey::Cut,
    StringKey::Debug,
    StringKey::DefaultInfo,
    StringKey::DeviceName,
    StringKey::DeviceNameHelp,
    StringKey::Edit,
    StringKey::Error,
    StringKey::Exit,
    StringKey::ForceTranscoding,
    StringKey::ForceTranscodingHelp,
    StringKey::HardwareDecoding,
    StringKey::HardwareDecodingHelp,
    StringKey::HideJellyfinDesktop,
    StringKey::HideOthers,
    StringKey::HideScrollbar,
    StringKey::HideScrollbarHelp,
    StringKey::InAppClientSide,
    StringKey::LogLevel,
    StringKey::LogLevelHelp,
    StringKey::MpvConfig,
    StringKey::OpenMpvConfigDir,
    StringKey::Paste,
    StringKey::Playback,
    StringKey::Quit,
    StringKey::Redo,
    StringKey::Reload,
    StringKey::ResetSavedServer,
    StringKey::SelectAll,
    StringKey::Server,
    StringKey::ShowAll,
    StringKey::Stereo,
    StringKey::Surround51,
    StringKey::Surround71,
    StringKey::SystemServerSide,
    StringKey::SystemThemedKde,
    StringKey::ToggleFullscreen,
    StringKey::Transcode,
    StringKey::TransparentTitlebar,
    StringKey::TransparentTitlebarHelp,
    StringKey::Undo,
    StringKey::Verbose,
    StringKey::Warning,
    StringKey::WindowDecorations,
    StringKey::WindowDecorationsHelp,
];

pub fn text(key: StringKey) -> &'static str {
    text_for_locale(&desktop_locale(), key)
}

pub fn strings_json() -> String {
    let locale = desktop_locale();
    let mut values = Map::new();
    for key in ALL_KEYS {
        values.insert(
            key.id().to_string(),
            Value::String(text_for_locale(&locale, *key).to_string()),
        );
    }
    values.insert("locale".to_string(), Value::String(locale));
    Value::Object(values).to_string()
}

pub fn desktop_locale() -> String {
    normalize_locale(&raw_os_locale().unwrap_or_else(|| FALLBACK_LOCALE.to_string()))
}

pub fn cef_locale() -> String {
    let locale = desktop_locale();
    let preferred = match locale.as_str() {
        "zh-cn" | "zh-hans" => "zh-CN",
        "zh-tw" | "zh-hant" | "zh-hk" => "zh-TW",
        "pt-br" => "pt-BR",
        "pt-pt" => "pt-PT",
        "en-gb" => "en-GB",
        "en-us" => CEF_FALLBACK_LOCALE,
        "es-419" => "es-419",
        s => s.split('-').next().unwrap_or(FALLBACK_LOCALE),
    };
    if CEF_LOCALES.contains(&preferred) {
        return preferred.to_string();
    }
    CEF_FALLBACK_LOCALE.to_string()
}

pub fn cef_accept_language_list() -> String {
    let locale = cef_locale();
    let primary = locale.split('-').next().unwrap_or(CEF_FALLBACK_LOCALE);
    if primary.eq_ignore_ascii_case("en") {
        return format!("{locale},en");
    }
    format!("{locale},{primary},en-US,en")
}

fn normalize_locale(raw: &str) -> String {
    let locale = raw
        .split(['.', '@', ':'])
        .next()
        .unwrap_or(raw)
        .replace('_', "-")
        .to_ascii_lowercase();
    let locale = locale.trim();
    if locale.is_empty() || locale == "c" || locale == "posix" {
        return FALLBACK_LOCALE.to_string();
    }
    match locale {
        "zh" | "zh-cn" | "zh-sg" | "zh-hans" => "zh-cn".to_string(),
        "zh-tw" | "zh-hk" | "zh-mo" | "zh-hant" => "zh-tw".to_string(),
        "en" => FALLBACK_LOCALE.to_string(),
        other => other.to_string(),
    }
}

fn text_for_locale(locale: &str, key: StringKey) -> &'static str {
    if locale.starts_with("zh-cn") || locale == "zh-hans" {
        zh_cn(key)
    } else if locale.starts_with("zh-tw") || locale == "zh-hant" {
        zh_tw(key)
    } else {
        en_us(key)
    }
}

fn en_us(key: StringKey) -> &'static str {
    match key {
        StringKey::About => "About",
        StringKey::AboutAppVersion => "App version",
        StringKey::AboutCefVersion => "CEF version",
        StringKey::AboutConfigDir => "Config directory",
        StringKey::AboutCurrentLogFile => "Current log file",
        StringKey::AboutJellyfinDesktop => "About Jellyfin Desktop",
        StringKey::Advanced => "Advanced",
        StringKey::Audio => "Audio",
        StringKey::AudioChannelLayout => "Audio Channel Layout",
        StringKey::AudioChannelLayoutHelp => {
            "Force a specific channel layout. Leave empty for auto-detection."
        }
        StringKey::AudioExclusive => "Exclusive Audio Output",
        StringKey::AudioExclusiveHelp => {
            "Take exclusive control of the audio device during playback. May reduce latency but prevents other apps from playing audio."
        }
        StringKey::AudioPassthrough => "Audio Passthrough",
        StringKey::AudioPassthroughHelp => {
            "Comma-separated list of codecs to pass through to the audio device (e.g. ac3,eac3,dts-hd,truehd). Leave empty to disable."
        }
        StringKey::Auto => "Auto",
        StringKey::Cancel => "Cancel",
        StringKey::ChangesRestart => "Changes take effect after restarting the application.",
        StringKey::ClientSettings => "Client Settings",
        StringKey::Copy => "Copy",
        StringKey::Cut => "Cut",
        StringKey::Debug => "Debug",
        StringKey::DefaultInfo => "Default (Info)",
        StringKey::DeviceName => "Device Name",
        StringKey::DeviceNameHelp => {
            "Identifies this machine to the server. Leave blank to use the system hostname."
        }
        StringKey::Edit => "Edit",
        StringKey::Error => "Error",
        StringKey::Exit => "Exit",
        StringKey::ForceTranscoding => "Force Transcoding",
        StringKey::ForceTranscodingHelp => {
            "Always request a transcoded stream from the server, even when direct play would work."
        }
        StringKey::HardwareDecoding => "Hardware Decoding",
        StringKey::HardwareDecodingHelp => {
            "Hardware video decoding mode. Use \"auto\" for automatic detection or \"no\" to disable."
        }
        StringKey::HideJellyfinDesktop => "Hide Jellyfin Desktop",
        StringKey::HideOthers => "Hide Others",
        StringKey::HideScrollbar => "Hide Scrollbar",
        StringKey::HideScrollbarHelp => {
            "Hide scrollbars throughout the app. Scrolling with the wheel, trackpad, and keyboard still works. Requires restart."
        }
        StringKey::InAppClientSide => "In-app (client-side)",
        StringKey::LogLevel => "Log Level",
        StringKey::LogLevelHelp => "Set the application log verbosity level.",
        StringKey::MpvConfig => "MPV config",
        StringKey::OpenMpvConfigDir => "Open mpv config directory",
        StringKey::Paste => "Paste",
        StringKey::Playback => "Playback",
        StringKey::Quit => "Quit",
        StringKey::Redo => "Redo",
        StringKey::Reload => "Reload",
        StringKey::ResetSavedServer => "Reset Saved Server",
        StringKey::SelectAll => "Select All",
        StringKey::Server => "Server",
        StringKey::ShowAll => "Show All",
        StringKey::Stereo => "Stereo",
        StringKey::Surround51 => "5.1 Surround",
        StringKey::Surround71 => "7.1 Surround",
        StringKey::SystemServerSide => "System (server-side)",
        StringKey::SystemThemedKde => "System, themed (KDE)",
        StringKey::ToggleFullscreen => "Toggle Fullscreen",
        StringKey::Transcode => "Transcode",
        StringKey::TransparentTitlebar => "Transparent Titlebar",
        StringKey::TransparentTitlebarHelp => {
            "Overlay traffic light buttons on the window content instead of a separate titlebar. Requires restart."
        }
        StringKey::Undo => "Undo",
        StringKey::Verbose => "Verbose",
        StringKey::Warning => "Warning",
        StringKey::WindowDecorations => "Window Decorations",
        StringKey::WindowDecorationsHelp => {
            "How the window titlebar is drawn. In-app is needed on desktops without their own (e.g. GNOME). Auto-detected by default; changing requires restart."
        }
    }
}

fn zh_cn(key: StringKey) -> &'static str {
    match key {
        StringKey::About => "关于",
        StringKey::AboutAppVersion => "应用版本",
        StringKey::AboutCefVersion => "CEF 版本",
        StringKey::AboutConfigDir => "配置目录",
        StringKey::AboutCurrentLogFile => "当前日志文件",
        StringKey::AboutJellyfinDesktop => "关于 Jellyfin Desktop",
        StringKey::Advanced => "高级",
        StringKey::Audio => "音频",
        StringKey::AudioChannelLayout => "音频声道布局",
        StringKey::AudioChannelLayoutHelp => "强制使用指定声道布局。留空以自动检测。",
        StringKey::AudioExclusive => "独占音频输出",
        StringKey::AudioExclusiveHelp => {
            "播放时独占控制音频设备。可能降低延迟，但会阻止其他应用播放音频。"
        }
        StringKey::AudioPassthrough => "音频直通",
        StringKey::AudioPassthroughHelp => {
            "用逗号分隔要直通到音频设备的编解码器列表（例如 ac3,eac3,dts-hd,truehd）。留空以禁用。"
        }
        StringKey::Auto => "自动",
        StringKey::Cancel => "取消",
        StringKey::ChangesRestart => "更改将在重启应用后生效。",
        StringKey::ClientSettings => "客户端设置",
        StringKey::Copy => "复制",
        StringKey::Cut => "剪切",
        StringKey::Debug => "调试",
        StringKey::DefaultInfo => "默认（信息）",
        StringKey::DeviceName => "设备名称",
        StringKey::DeviceNameHelp => "向服务器标识此设备。留空以使用系统主机名。",
        StringKey::Edit => "编辑",
        StringKey::Error => "错误",
        StringKey::Exit => "退出",
        StringKey::ForceTranscoding => "强制转码",
        StringKey::ForceTranscodingHelp => "始终向服务器请求转码流，即使可以直接播放。",
        StringKey::HardwareDecoding => "硬件解码",
        StringKey::HardwareDecodingHelp => "硬件视频解码模式。使用“auto”自动检测，或使用“no”禁用。",
        StringKey::HideJellyfinDesktop => "隐藏 Jellyfin Desktop",
        StringKey::HideOthers => "隐藏其他应用",
        StringKey::HideScrollbar => "隐藏滚动条",
        StringKey::HideScrollbarHelp => {
            "在整个应用中隐藏滚动条。仍可使用滚轮、触控板和键盘滚动。需要重启。"
        }
        StringKey::InAppClientSide => "应用内（客户端）",
        StringKey::LogLevel => "日志级别",
        StringKey::LogLevelHelp => "设置应用日志详细程度。",
        StringKey::MpvConfig => "MPV 配置",
        StringKey::OpenMpvConfigDir => "打开 mpv 配置目录",
        StringKey::Paste => "粘贴",
        StringKey::Playback => "播放",
        StringKey::Quit => "退出",
        StringKey::Redo => "重做",
        StringKey::Reload => "重新加载",
        StringKey::ResetSavedServer => "重置已保存服务器",
        StringKey::SelectAll => "全选",
        StringKey::Server => "服务器",
        StringKey::ShowAll => "显示全部",
        StringKey::Stereo => "立体声",
        StringKey::Surround51 => "5.1 环绕声",
        StringKey::Surround71 => "7.1 环绕声",
        StringKey::SystemServerSide => "系统（服务端）",
        StringKey::SystemThemedKde => "系统，跟随主题（KDE）",
        StringKey::ToggleFullscreen => "切换全屏",
        StringKey::Transcode => "转码",
        StringKey::TransparentTitlebar => "透明标题栏",
        StringKey::TransparentTitlebarHelp => {
            "将交通灯按钮覆盖在窗口内容上，而不是使用单独的标题栏。需要重启。"
        }
        StringKey::Undo => "撤销",
        StringKey::Verbose => "详细",
        StringKey::Warning => "警告",
        StringKey::WindowDecorations => "窗口装饰",
        StringKey::WindowDecorationsHelp => {
            "窗口标题栏的绘制方式。应用内标题栏适用于没有系统标题栏的桌面（例如 GNOME）。默认自动检测；更改需要重启。"
        }
    }
}

fn zh_tw(key: StringKey) -> &'static str {
    match key {
        StringKey::About => "關於",
        StringKey::AboutAppVersion => "應用程式版本",
        StringKey::AboutCefVersion => "CEF 版本",
        StringKey::AboutConfigDir => "設定目錄",
        StringKey::AboutCurrentLogFile => "目前記錄檔",
        StringKey::AboutJellyfinDesktop => "關於 Jellyfin Desktop",
        StringKey::Advanced => "進階",
        StringKey::Audio => "音訊",
        StringKey::AudioChannelLayout => "音訊聲道配置",
        StringKey::AudioChannelLayoutHelp => "強制使用指定的聲道配置。留空以自動偵測。",
        StringKey::AudioExclusive => "獨佔音訊輸出",
        StringKey::AudioExclusiveHelp => {
            "播放時獨佔控制音訊裝置。可能降低延遲，但會阻止其他應用程式播放音訊。"
        }
        StringKey::AudioPassthrough => "音訊直通",
        StringKey::AudioPassthroughHelp => {
            "以逗號分隔要直通至音訊裝置的編解碼器清單（例如 ac3,eac3,dts-hd,truehd）。留空以停用。"
        }
        StringKey::Auto => "自動",
        StringKey::Cancel => "取消",
        StringKey::ChangesRestart => "變更會在重新啟動應用程式後生效。",
        StringKey::ClientSettings => "用戶端設定",
        StringKey::Copy => "複製",
        StringKey::Cut => "剪下",
        StringKey::Debug => "偵錯",
        StringKey::DefaultInfo => "預設（資訊）",
        StringKey::DeviceName => "裝置名稱",
        StringKey::DeviceNameHelp => "向伺服器識別此裝置。留空以使用系統主機名稱。",
        StringKey::Edit => "編輯",
        StringKey::Error => "錯誤",
        StringKey::Exit => "結束",
        StringKey::ForceTranscoding => "強制轉碼",
        StringKey::ForceTranscodingHelp => "一律向伺服器要求轉碼串流，即使可以直接播放也一樣。",
        StringKey::HardwareDecoding => "硬體解碼",
        StringKey::HardwareDecodingHelp => {
            "硬體視訊解碼模式。使用「auto」自動偵測，或使用「no」停用。"
        }
        StringKey::HideJellyfinDesktop => "隱藏 Jellyfin Desktop",
        StringKey::HideOthers => "隱藏其他項目",
        StringKey::HideScrollbar => "隱藏捲動軸",
        StringKey::HideScrollbarHelp => {
            "在整個應用程式中隱藏捲動軸。仍可使用滾輪、觸控板和鍵盤捲動。需要重新啟動。"
        }
        StringKey::InAppClientSide => "應用程式內（用戶端）",
        StringKey::LogLevel => "記錄層級",
        StringKey::LogLevelHelp => "設定應用程式記錄的詳細程度。",
        StringKey::MpvConfig => "MPV 設定",
        StringKey::OpenMpvConfigDir => "開啟 mpv 設定目錄",
        StringKey::Paste => "貼上",
        StringKey::Playback => "播放",
        StringKey::Quit => "結束",
        StringKey::Redo => "重做",
        StringKey::Reload => "重新載入",
        StringKey::ResetSavedServer => "重設已儲存的伺服器",
        StringKey::SelectAll => "全選",
        StringKey::Server => "伺服器",
        StringKey::ShowAll => "顯示全部",
        StringKey::Stereo => "立體聲",
        StringKey::Surround51 => "5.1 環繞聲",
        StringKey::Surround71 => "7.1 環繞聲",
        StringKey::SystemServerSide => "系統（伺服器端）",
        StringKey::SystemThemedKde => "系統，跟隨主題（KDE）",
        StringKey::ToggleFullscreen => "切換全螢幕",
        StringKey::Transcode => "轉碼",
        StringKey::TransparentTitlebar => "透明標題列",
        StringKey::TransparentTitlebarHelp => {
            "將交通燈按鈕覆蓋在視窗內容上，而不是使用獨立標題列。需要重新啟動。"
        }
        StringKey::Undo => "復原",
        StringKey::Verbose => "詳細",
        StringKey::Warning => "警告",
        StringKey::WindowDecorations => "視窗裝飾",
        StringKey::WindowDecorationsHelp => {
            "視窗標題列的繪製方式。應用程式內標題列適用於沒有系統標題列的桌面（例如 GNOME）。預設自動偵測；變更需要重新啟動。"
        }
    }
}

fn raw_os_locale() -> Option<String> {
    #[cfg(windows)]
    if let Some(locale) = windows_user_locale() {
        return Some(locale);
    }

    for key in ["LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"] {
        if let Ok(value) = std::env::var(key)
            && !value.trim().is_empty()
        {
            return Some(value);
        }
    }
    None
}

#[cfg(windows)]
fn windows_user_locale() -> Option<String> {
    use windows_sys::Win32::Globalization::GetUserDefaultLocaleName;

    let mut buf = [0u16; 85];
    let len = unsafe { GetUserDefaultLocaleName(buf.as_mut_ptr(), buf.len() as i32) };
    if len <= 1 {
        return None;
    }
    Some(String::from_utf16_lossy(&buf[..len as usize - 1]))
}
