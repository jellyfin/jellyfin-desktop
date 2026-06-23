// JfnCefLayer is an opaque internal handle; callers within this crate
// pass it back unchanged. Marking each consumer unsafe would cascade
// without adding type safety, so the lint is suppressed module-wide.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

//! WebBrowser business logic.
//!
//! Routes the ~20 jellyfin-web IPC names to mpv, settings, theme color,
//! and the playback coordinator. The web layer's exec_js sink for the
//! playback coordinator is exposed as [`jfn_web_exec_js`] for boot wiring.

use cef::{ImplListValue, ListValue};
use parking_lot::Mutex;
use serde_json::Value;
use std::ffi::c_char;
use std::os::raw::c_void;
use std::sync::Arc;

use crate::browsers::{jfn_browsers_active, jfn_browsers_set_active};
use crate::business_common::{apply_setting_value, js_cstr_or_warn, reject_double_init};
use crate::client::{Inner, JfnCefLayer, jfn_cef_layer_inner, jfn_cef_layer_set_name};
use crate::ipc::{BrowserMessage, list_int, list_string};
use jfn_color::jfn_cef_parse_color;
use jfn_color::theme::{jfn_theme_color_on_color, jfn_theme_color_set_video_mode};
use jfn_mpv::api::{
    jfn_mpv_audio_add, jfn_mpv_load_file, jfn_mpv_pause, jfn_mpv_play, jfn_mpv_seek_absolute,
    jfn_mpv_set_aspect_mode, jfn_mpv_set_audio_delay, jfn_mpv_set_audio_track, jfn_mpv_set_muted,
    jfn_mpv_set_speed, jfn_mpv_set_subtitle_delay, jfn_mpv_set_subtitle_track, jfn_mpv_set_volume,
    jfn_mpv_stop, jfn_mpv_sub_add,
};
use jfn_mpv::boot::jfn_mpv_handle_get;
use jfn_playback::ingest_driver::jfn_playback_fullscreen;
use jfn_playback::shutdown::jfn_shutdown_initiate;
use jfn_playback::{Input as PbInput, MediaType as PbMediaType, post as pb_post};

use jfn_mpv::api::JfnMpvLoadOptions;

// MediaType matching jfn-playback's enum: Unknown=0, Audio=1, Video=2.
const MT_UNKNOWN: u8 = 0;
const MT_AUDIO: u8 = 1;
const MT_VIDEO: u8 = 2;

#[derive(Default)]
struct MediaMetadata {
    id: String,
    title: String,
    artist: String,
    album: String,
    track_number: i32,
    duration_us: i64,
    media_type: u8,
}

struct WebState {
    layer: Arc<Inner>,
    was_fullscreen_before_osd: bool,
}

static INSTANCE: Mutex<Option<WebState>> = Mutex::new(None);

pub fn jfn_web_init(layer: *mut JfnCefLayer) {
    if layer.is_null() {
        return;
    }
    // Reject double-init: prior INSTANCE would be silently overwritten and
    // its `was_fullscreen_before_osd` state lost.
    if reject_double_init(&INSTANCE.lock(), "jfn_web_init") {
        return;
    }

    let name = c"web";
    unsafe { jfn_cef_layer_set_name(layer, name.as_ptr()) };

    let inner = unsafe { jfn_cef_layer_inner(layer) };
    install_handlers(layer, Arc::clone(&inner));

    *INSTANCE.lock() = Some(WebState {
        layer: inner,
        was_fullscreen_before_osd: false,
    });
}

/// Execute JS in the main web layer. Called by the playback browser sink.
///
/// # Safety
/// `js_utf8` must be a NUL-terminated UTF-8 pointer, or null.
pub unsafe fn jfn_web_exec_js(js_utf8: *const c_char) {
    if js_utf8.is_null() {
        return;
    }
    // Clone the Arc<Inner> out under lock then release the lock before the
    // CEF call. The Arc keeps Inner alive across the call even if the layer
    // is closed mid-way; no TOCTOU window between lock-drop and use.
    let inner = match INSTANCE.lock().as_ref() {
        Some(s) => Arc::clone(&s.layer),
        None => return,
    };
    let js = unsafe { std::ffi::CStr::from_ptr(js_utf8) }.to_string_lossy();
    inner.exec_js(&js);
}

fn install_handlers(layer: *mut JfnCefLayer, inner_for_created: Arc<Inner>) {
    let l = unsafe { &*layer };

    l.set_created_callback_rust(Some(Box::new(move |_b: *mut c_void| {
        // Main browser takes input only if no other layer has already
        // claimed it (e.g. the server-selection overlay).
        if jfn_browsers_active().is_null() {
            let p = inner_for_created.layer_ptr();
            if !p.is_null() {
                jfn_browsers_set_active(p);
            }
        }
    })));

    l.set_message_handler_rust(Some(Box::new(handle_message)));

    // BeforeClose: clear INSTANCE so any post-close jfn_web_exec_js becomes
    // a no-op instead of touching a torn-down layer.
    l.set_before_close_callback_rust(Some(Box::new(|| {
        *INSTANCE.lock() = None;
    })));

    l.set_context_menu_builder_rust(Some(crate::app_menu::build_closure()));
    l.set_context_menu_dispatcher_rust(Some(crate::app_menu::dispatch_closure()));
}

fn parse_metadata_json(json: &str) -> MediaMetadata {
    let mut out = MediaMetadata::default();
    let Ok(v) = serde_json::from_str::<Value>(json) else {
        return out;
    };
    let Value::Object(d) = v else { return out };

    let get_str = |k: &str| d.get(k).and_then(Value::as_str).unwrap_or("").to_string();

    out.id = get_str("Id");
    out.title = get_str("Name");
    out.artist = get_str("SeriesName");
    if out.artist.is_empty()
        && let Some(arr) = d.get("Artists").and_then(Value::as_array)
        && let Some(first) = arr.first().and_then(Value::as_str)
    {
        out.artist = first.to_string();
    }
    out.album = get_str("SeasonName");
    if out.album.is_empty() {
        out.album = get_str("Album");
    }
    if let Some(n) = d.get("IndexNumber").and_then(Value::as_i64) {
        out.track_number = n as i32;
    }
    if let Some(t) = d.get("RunTimeTicks") {
        let ticks = t
            .as_f64()
            .or_else(|| t.as_i64().map(|n| n as f64))
            .unwrap_or(0.0);
        out.duration_us = ticks as i64 / 10;
    }
    out.media_type = match get_str("Type").as_str() {
        "Audio" => MT_AUDIO,
        "Movie" | "Episode" | "Video" | "MusicVideo" => MT_VIDEO,
        _ => MT_UNKNOWN,
    };
    out
}

fn media_type_to_pb(t: u8) -> PbMediaType {
    match t {
        MT_AUDIO => PbMediaType::Audio,
        MT_VIDEO => PbMediaType::Video,
        _ => PbMediaType::Unknown,
    }
}

fn post_metadata(meta: &MediaMetadata) {
    pb_post(PbInput::Metadata(jfn_playback::MediaMetadata {
        id: meta.id.clone(),
        title: meta.title.clone(),
        artist: meta.artist.clone(),
        album: meta.album.clone(),
        track_number: meta.track_number,
        duration_us: meta.duration_us,
        art_url: String::new(),
        art_data_uri: String::new(),
        media_type: media_type_to_pb(meta.media_type),
    }));
}

fn handle_player_load(args: &ListValue) {
    let url = list_string(args, 0);
    let start_ms = if args.size() > 1 {
        list_int(args, 1)
    } else {
        0
    };
    let video_idx = list_int(args, 2) as i64;
    let audio_idx = list_int(args, 3) as i64;
    let sub_idx = list_int(args, 4) as i64;
    let metadata_json = if args.size() > 5 {
        list_string(args, 5)
    } else {
        String::new()
    };
    let external_audio_url = if args.size() > 6 {
        list_string(args, 6)
    } else {
        String::new()
    };
    let external_sub_url = if args.size() > 7 {
        list_string(args, 7)
    } else {
        String::new()
    };
    let is_infinite_stream = if args.size() > 8 {
        args.bool(8) != 0
    } else {
        false
    };
    jfn_logging::log(
        jfn_logging::CATEGORY_CEF,
        jfn_logging::LEVEL_INFO,
        &format!(
            "playerLoad: video={video_idx} audio={audio_idx} sub={sub_idx} \
             start={start_ms}ms infinite={is_infinite_stream} \
             extAudio={external_audio_url} extSub={external_sub_url} url={url}"
        ),
    );

    let meta = if metadata_json.is_empty() {
        MediaMetadata::default()
    } else {
        parse_metadata_json(&metadata_json)
    };

    // Atomic pre-load posts so MPRIS/JS see start position before
    // mpv has opened the file.
    pb_post(PbInput::LoadStarting(meta.id.clone()));
    pb_post(PbInput::Position(start_ms as i64 * 1000));

    if !metadata_json.is_empty() {
        jfn_theme_color_set_video_mode(meta.media_type == MT_VIDEO);
        post_metadata(&meta);
    }

    let Some(url_c) = js_cstr_or_warn("playerLoad url", &url) else {
        return;
    };
    let Some(ext_audio_c) = js_cstr_or_warn("playerLoad ext audio", &external_audio_url) else {
        return;
    };
    let Some(ext_sub_c) = js_cstr_or_warn("playerLoad ext sub", &external_sub_url) else {
        return;
    };
    let opts = JfnMpvLoadOptions {
        start_secs: start_ms as f64 / 1000.0,
        video_track: video_idx,
        audio_track: audio_idx,
        sub_track: sub_idx,
        external_audio_url: ext_audio_c.as_ptr(),
        external_sub_url: ext_sub_c.as_ptr(),
        is_infinite_stream,
    };
    apply_subtitle_style_settings();
    unsafe { jfn_mpv_load_file(url_c.as_ptr(), &opts) };
}

/// Run `f` if the IPC arrived with an args list. Always returns `true` —
/// every arm using this is considered "handled" even when args are
/// missing, matching the prior behaviour.
fn with_args(args: Option<&ListValue>, f: impl FnOnce(&ListValue)) -> bool {
    if let Some(a) = args {
        f(a);
    }
    true
}

fn handle_message(message: BrowserMessage) -> bool {
    let args = message.args();

    // mpv handle not yet initialised — return false so CEF treats the message as unhandled.
    if jfn_mpv_handle_get().is_null() {
        return false;
    }

    match message.name() {
        "playerLoad" => with_args(args, handle_player_load),
        "playerStop" => {
            jfn_mpv_stop();
            true
        }
        "playerPause" => {
            jfn_mpv_pause();
            true
        }
        "playerPlay" => {
            jfn_mpv_play();
            true
        }
        "playerSeek" => with_args(args, |a| {
            jfn_mpv_seek_absolute(list_int(a, 0) as f64 / 1000.0);
        }),
        "playerSetVolume" => with_args(args, |a| {
            jfn_mpv_set_volume(list_int(a, 0) as f64);
        }),
        "playerSetMuted" => with_args(args, |a| {
            jfn_mpv_set_muted(a.bool(0) != 0);
        }),
        "playerSetSpeed" => with_args(args, |a| {
            jfn_mpv_set_speed(list_int(a, 0) as f64 / 1000.0);
        }),
        "playerSetSubtitle" => with_args(args, |a| {
            let id = list_int(a, 0) as i64;
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_INFO,
                &format!("playerSetSubtitle: {id}"),
            );
            jfn_mpv_set_subtitle_track(id);
        }),
        "playerAddSubtitle" => with_args(args, |a| {
            let url = list_string(a, 0);
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_INFO,
                &format!("playerAddSubtitle: {url}"),
            );
            if let Some(c) = js_cstr_or_warn("playerAddSubtitle url", &url) {
                unsafe { jfn_mpv_sub_add(c.as_ptr()) };
            }
        }),
        "playerSetAudio" => with_args(args, |a| {
            jfn_mpv_set_audio_track(list_int(a, 0) as i64);
        }),
        "playerAddAudio" => with_args(args, |a| {
            let url = list_string(a, 0);
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_INFO,
                &format!("playerAddAudio: {url}"),
            );
            if let Some(c) = js_cstr_or_warn("playerAddAudio url", &url) {
                unsafe { jfn_mpv_audio_add(c.as_ptr()) };
            }
        }),
        "playerSetAudioDelay" => with_args(args, |a| jfn_mpv_set_audio_delay(a.double(0))),
        "playerSetSubtitleDelay" => with_args(args, |a| jfn_mpv_set_subtitle_delay(a.double(0))),
        "playerSetAspectMode" => with_args(args, |a| {
            let mode = list_string(a, 0);
            if let Some(c) = js_cstr_or_warn("playerSetAspectMode", &mode) {
                unsafe { jfn_mpv_set_aspect_mode(c.as_ptr()) };
            }
        }),
        "playerOsdActive" => with_args(args, |a| {
            let active = a.bool(0) != 0;
            let mut g = INSTANCE.lock();
            let Some(st) = g.as_mut() else { return };
            if active {
                st.was_fullscreen_before_osd = jfn_playback_fullscreen();
            } else if !st.was_fullscreen_before_osd {
                jfn_platform_abi::get().set_fullscreen(false);
            }
        }),
        "toggleFullscreen" => {
            jfn_platform_abi::get().toggle_fullscreen();
            true
        }
        "saveServerUrl" => with_args(args, |a| {
            jfn_config::set_server_url(&list_string(a, 0));
            jfn_config::settings_save_async();
        }),
        "setSettingValue" => with_args(args, |a| {
            let section = list_string(a, 0);
            let key = list_string(a, 1);
            let value = list_string(a, 2);
            apply_setting_value(&section, &key, &value);
        }),
        "themeColor" => with_args(args, |a| {
            let color = list_string(a, 0);
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_DEBUG,
                &format!("themeColor IPC: {color}"),
            );
            if let Some(c) = js_cstr_or_warn("themeColor", &color) {
                let rgb = unsafe { jfn_cef_parse_color(c.as_ptr()) };
                jfn_theme_color_on_color(rgb);
            }
        }),
        "notifyMetadata" => with_args(args, |a| {
            let meta = parse_metadata_json(&list_string(a, 0));
            jfn_theme_color_set_video_mode(meta.media_type == MT_VIDEO);
            post_metadata(&meta);
        }),
        "notifyArtwork" => with_args(args, |a| {
            pb_post(PbInput::Artwork(list_string(a, 0)));
        }),
        "notifyQueueChange" => with_args(args, |a| {
            pb_post(PbInput::QueueCaps {
                can_go_next: a.bool(0) != 0,
                can_go_prev: a.bool(1) != 0,
            });
        }),
        "notifyPlaybackState" => {
            // mpv is the authoritative source via coordinator; ignore JS hint.
            true
        }
        "notifySeek" => with_args(args, |a| {
            pb_post(PbInput::Seeked(list_int(a, 0) as i64 * 1000));
        }),
        "appExit" => {
            jfn_shutdown_initiate();
            true
        }
        "openConfigDir" => {
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_INFO,
                "Opening mpv home directory",
            );
            if let Some(p) = crate::platform_ops::ops() {
                p.open_path(&jfn_paths::mpv_home());
            }
            true
        }
        _ => false,
    }
}

fn apply_subtitle_style_settings() {
    jfn_mpv_set_subtitle_scale(jfn_config::subtitle_scale());
    jfn_mpv_set_subtitle_bold(jfn_config::subtitle_bold());
}
