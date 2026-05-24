//! Embedded JS shim sources, included at compile time from `src/web/*.js`.

pub fn get(name: &str) -> Option<&'static str> {
    Some(match name {
        "native-shim.js" => include_str!("../../web/native-shim.js"),
        "mpv-player-base.js" => include_str!("../../web/mpv-player-base.js"),
        "mpv-video-player.js" => include_str!("../../web/mpv-video-player.js"),
        "mpv-audio-player.js" => include_str!("../../web/mpv-audio-player.js"),
        "input-plugin.js" => include_str!("../../web/input-plugin.js"),
        "client-settings.js" => include_str!("../../web/client-settings.js"),
        "context-menu.js" => include_str!("../../web/context-menu.js"),
        _ => return None,
    })
}
