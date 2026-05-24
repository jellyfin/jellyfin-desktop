// Sets the layer name through a layer handle obtained from the registry —
// see browsers.rs for the matching allow rationale.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

//! WebBrowser business logic.
//!
//! Routes the ~20 jellyfin-web IPC names to mpv, settings, theme color,
//! and the playback coordinator. The web layer's exec_js sink for the
//! playback coordinator is exposed as [`jfn_web_exec_js`] for boot wiring.

use cef::rc::ConvertReturnValue;
use cef::*;
use parking_lot::Mutex;
use serde_json::Value;
use std::ffi::{CString, c_char};
use std::os::raw::c_void;

use crate::client::JfnCefLayer;

use crate::browsers::{jfn_browsers_active, jfn_browsers_set_active};
use crate::client::{jfn_cef_layer_exec_js, jfn_cef_layer_set_name};
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

use jfn_platform_abi::cursor::{CT_NONE, CT_POINTER};

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
    layer: *mut JfnCefLayer,
    was_fullscreen_before_osd: bool,
}

unsafe impl Send for WebState {}

static INSTANCE: Mutex<Option<WebState>> = Mutex::new(None);

pub fn jfn_web_init(layer: *mut JfnCefLayer) {
    if layer.is_null() {
        return;
    }
    let name = CString::new("web").unwrap();
    unsafe { jfn_cef_layer_set_name(layer, name.as_ptr()) };

    install_handlers(layer);

    *INSTANCE.lock() = Some(WebState {
        layer,
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
    let layer = match INSTANCE.lock().as_ref() {
        Some(s) => s.layer,
        None => return,
    };
    let len = unsafe { std::ffi::CStr::from_ptr(js_utf8) }
        .to_bytes()
        .len();
    unsafe { jfn_cef_layer_exec_js(layer, js_utf8, len) };
}

fn install_handlers(layer: *mut JfnCefLayer) {
    let l = unsafe { &*layer };

    let lp_created = LayerPtr(layer);
    l.set_created_callback_rust(Some(Box::new(move |_b: *mut c_void| {
        let lp = &lp_created;
        // Main browser takes input only if no other layer has already
        // claimed it (e.g. the server-selection overlay).
        if jfn_browsers_active().is_null() {
            jfn_browsers_set_active(lp.0);
        }
    })));

    l.set_message_handler_rust(Some(Box::new(
        move |name: &str, args_raw: *mut c_void, browser_raw: *mut c_void| -> bool {
            handle_message(name, args_raw, browser_raw)
        },
    )));

    l.set_context_menu_builder_rust(Some(crate::app_menu::build_closure()));
    l.set_context_menu_dispatcher_rust(Some(crate::app_menu::dispatch_closure()));
}

fn list_string(args: &ListValue, idx: usize) -> String {
    let userfree = args.string(idx);
    let cs: CefString = (&userfree).into();
    cs.to_string()
}

fn list_int(args: &ListValue, idx: usize) -> i32 {
    // Some integer args arrive as double; round to match the C++ helper.
    let t = args.get_type(idx);
    if t.as_ref() == &sys::cef_value_type_t::VTYPE_DOUBLE {
        args.double(idx).round() as i32
    } else {
        args.int(idx)
    }
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

fn apply_setting_value(_section: &str, key: &str, value: &str) {
    match key {
        "hwdec" => jfn_config::set_hwdec(value),
        "audioPassthrough" => jfn_config::set_audio_passthrough(value),
        "audioExclusive" => jfn_config::set_audio_exclusive(value == "true"),
        "audioChannels" => jfn_config::set_audio_channels(value),
        "titlebarThemeColor" => jfn_config::set_titlebar_theme_color(value == "true"),
        "logLevel" => jfn_config::set_log_level(value),
        "forceTranscoding" => jfn_config::set_force_transcoding(value == "true"),
        "deviceName" => jfn_config::set_device_name(value, ""),
        _ => jfn_logging::log(
            jfn_logging::CATEGORY_CEF,
            jfn_logging::LEVEL_WARN,
            &format!("Unknown setting key: {_section}.{key}"),
        ),
    }
    jfn_config::settings_save_async();
}

fn handle_message(name: &str, args_raw: *mut c_void, browser_raw: *mut c_void) -> bool {
    if jfn_mpv_handle_get().is_null() {
        // Adopt and drop refs so we don't leak.
        if !args_raw.is_null() {
            let _: ListValue = (args_raw as *mut sys::_cef_list_value_t).wrap_result();
        }
        if !browser_raw.is_null() {
            let _: Browser = (browser_raw as *mut sys::_cef_browser_t).wrap_result();
        }
        return false;
    }

    let args = (!args_raw.is_null())
        .then(|| -> ListValue { (args_raw as *mut sys::_cef_list_value_t).wrap_result() });
    if !browser_raw.is_null() {
        // Browser ref isn't needed by any web handler; just drop it.
        let _: Browser = (browser_raw as *mut sys::_cef_browser_t).wrap_result();
    }

    match name {
        "playerLoad" => {
            let Some(args) = args else { return true };
            let url = list_string(&args, 0);
            let start_ms = if args.size() > 1 {
                list_int(&args, 1)
            } else {
                0
            };
            let video_idx = list_int(&args, 2) as i64;
            let audio_idx = list_int(&args, 3) as i64;
            let sub_idx = list_int(&args, 4) as i64;
            let metadata_json = if args.size() > 5 {
                list_string(&args, 5)
            } else {
                String::new()
            };
            let external_audio_url = if args.size() > 6 {
                list_string(&args, 6)
            } else {
                String::new()
            };
            let external_sub_url = if args.size() > 7 {
                list_string(&args, 7)
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

            // Keep CStrings alive across jfn_mpv_load_file.
            let url_c = CString::new(url).unwrap_or_default();
            let ext_audio_c = CString::new(external_audio_url).unwrap_or_default();
            let ext_sub_c = CString::new(external_sub_url).unwrap_or_default();
            let opts = JfnMpvLoadOptions {
                start_secs: start_ms as f64 / 1000.0,
                video_track: video_idx,
                audio_track: audio_idx,
                sub_track: sub_idx,
                external_audio_url: ext_audio_c.as_ptr(),
                external_sub_url: ext_sub_c.as_ptr(),
                is_infinite_stream,
            };
            unsafe { jfn_mpv_load_file(url_c.as_ptr(), &opts) };
            true
        }
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
        "playerSeek" => {
            if let Some(args) = args {
                let pos = list_int(&args, 0) as f64 / 1000.0;
                jfn_mpv_seek_absolute(pos);
            }
            true
        }
        "playerSetVolume" => {
            if let Some(args) = args {
                jfn_mpv_set_volume(list_int(&args, 0) as f64);
            }
            true
        }
        "playerSetMuted" => {
            if let Some(args) = args {
                jfn_mpv_set_muted(args.bool(0) != 0);
            }
            true
        }
        "playerSetSpeed" => {
            if let Some(args) = args {
                jfn_mpv_set_speed(list_int(&args, 0) as f64 / 1000.0);
            }
            true
        }
        "playerSetSubtitle" => {
            if let Some(args) = args {
                let id = list_int(&args, 0) as i64;
                jfn_logging::log(
                    jfn_logging::CATEGORY_CEF,
                    jfn_logging::LEVEL_INFO,
                    &format!("playerSetSubtitle: {id}"),
                );
                jfn_mpv_set_subtitle_track(id);
            }
            true
        }
        "playerAddSubtitle" => {
            if let Some(args) = args {
                let url = list_string(&args, 0);
                jfn_logging::log(
                    jfn_logging::CATEGORY_CEF,
                    jfn_logging::LEVEL_INFO,
                    &format!("playerAddSubtitle: {url}"),
                );
                let c = CString::new(url).unwrap_or_default();
                unsafe { jfn_mpv_sub_add(c.as_ptr()) };
            }
            true
        }
        "playerSetAudio" => {
            if let Some(args) = args {
                jfn_mpv_set_audio_track(list_int(&args, 0) as i64);
            }
            true
        }
        "playerAddAudio" => {
            if let Some(args) = args {
                let url = list_string(&args, 0);
                jfn_logging::log(
                    jfn_logging::CATEGORY_CEF,
                    jfn_logging::LEVEL_INFO,
                    &format!("playerAddAudio: {url}"),
                );
                let c = CString::new(url).unwrap_or_default();
                unsafe { jfn_mpv_audio_add(c.as_ptr()) };
            }
            true
        }
        "playerSetAudioDelay" => {
            if let Some(args) = args {
                jfn_mpv_set_audio_delay(args.double(0));
            }
            true
        }
        "playerSetSubtitleDelay" => {
            if let Some(args) = args {
                jfn_mpv_set_subtitle_delay(args.double(0));
            }
            true
        }
        "playerSetAspectMode" => {
            if let Some(args) = args {
                let mode = list_string(&args, 0);
                let c = CString::new(mode).unwrap_or_default();
                unsafe { jfn_mpv_set_aspect_mode(c.as_ptr()) };
            }
            true
        }
        "playerOsdActive" => {
            if let Some(args) = args {
                let active = args.bool(0) != 0;
                let mut g = INSTANCE.lock();
                let Some(st) = g.as_mut() else { return true };
                if active {
                    st.was_fullscreen_before_osd = jfn_playback_fullscreen();
                } else if !st.was_fullscreen_before_osd {
                    jfn_platform_abi::get().set_fullscreen(false);
                }
            }
            true
        }
        "toggleFullscreen" => {
            jfn_platform_abi::get().toggle_fullscreen();
            true
        }
        "saveServerUrl" => {
            if let Some(args) = args {
                let url = list_string(&args, 0);
                jfn_config::set_server_url(&url);
                jfn_config::settings_save_async();
            }
            true
        }
        "setSettingValue" => {
            if let Some(args) = args {
                let section = list_string(&args, 0);
                let key = list_string(&args, 1);
                let value = list_string(&args, 2);
                apply_setting_value(&section, &key, &value);
            }
            true
        }
        "themeColor" => {
            if let Some(args) = args {
                let color = list_string(&args, 0);
                jfn_logging::log(
                    jfn_logging::CATEGORY_CEF,
                    jfn_logging::LEVEL_DEBUG,
                    &format!("themeColor IPC: {color}"),
                );
                let c = CString::new(color).unwrap_or_default();
                let rgb = unsafe { jfn_cef_parse_color(c.as_ptr()) };
                jfn_theme_color_on_color(rgb);
            }
            true
        }
        "notifyMetadata" => {
            if let Some(args) = args {
                let json = list_string(&args, 0);
                let meta = parse_metadata_json(&json);
                jfn_theme_color_set_video_mode(meta.media_type == MT_VIDEO);
                post_metadata(&meta);
            }
            true
        }
        "notifyArtwork" => {
            if let Some(args) = args {
                let uri = list_string(&args, 0);
                pb_post(PbInput::Artwork(uri));
            }
            true
        }
        "notifyQueueChange" => {
            if let Some(args) = args {
                let can_go_next = args.bool(0) != 0;
                let can_go_prev = args.bool(1) != 0;
                pb_post(PbInput::QueueCaps {
                    can_go_next,
                    can_go_prev,
                });
            }
            true
        }
        "notifyPlaybackState" => {
            // mpv is the authoritative source via coordinator; ignore JS hint.
            true
        }
        "notifySeek" => {
            if let Some(args) = args {
                let pos_ms = list_int(&args, 0) as i64;
                pb_post(PbInput::Seeked(pos_ms * 1000));
            }
            true
        }
        "setCursorVisible" => {
            if let Some(args) = args {
                let visible = args.bool(0) != 0;
                jfn_platform_abi::get().set_cursor(if visible { CT_POINTER } else { CT_NONE });
            }
            true
        }
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
            jfn_paths::open_mpv_home();
            true
        }
        _ => false,
    }
}

#[derive(Clone, Copy)]
struct LayerPtr(*mut JfnCefLayer);
unsafe impl Send for LayerPtr {}
unsafe impl Sync for LayerPtr {}
