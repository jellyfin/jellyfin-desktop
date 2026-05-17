//! Jellyfin DeviceProfile JSON builder.
//!
//! Mirrors the C++ implementation that previously lived in
//! `src/jellyfin/device_profile.cpp`. Jellyfin source pinned to commit
//! 2c62d40 (matches the `third_party/jellyfin` submodule). Profile-vs-stream
//! matching is plain case-insensitive equality against ffprobe-derived names,
//! so any rename below has to mirror what the server stores on
//! `MediaSource.Container` / `MediaStream.Codec` at probe time.
//!
//! Match logic:
//!   <https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.Model/Extensions/ContainerHelper.cs#L82-L107>
//! Subtitle match in StreamBuilder:
//!   <https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.Model/Dlna/StreamBuilder.cs#L1476>
//! Container normalization at probe time (NormalizeFormat):
//!   <https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.MediaEncoding/Probing/ProbeResultNormalizer.cs#L270-L315>
//! Subtitle normalization at probe time (NormalizeSubtitleCodec):
//!   <https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.MediaEncoding/Probing/ProbeResultNormalizer.cs#L632-L652>

use serde_json::{Map, Value, json};
use std::ffi::{CStr, CString, c_char};
use std::os::raw::c_uchar;
use std::sync::Mutex;

#[repr(u8)]
#[derive(Copy, Clone, Eq, PartialEq, Debug)]
pub enum MediaKind {
    Video = 0,
    Audio = 1,
    Subtitle = 2,
}

#[derive(Clone, Debug)]
pub struct Codec {
    pub name: String,
    pub kind: MediaKind,
}

#[repr(C)]
pub struct JfnCodec {
    pub name: *const c_char,
    pub kind: c_uchar,
}

const SUBTITLE_RENAMES: &[(&str, &str)] = &[
    ("subrip", "srt"),
    ("ass", "ssa"),
    ("hdmv_pgs_subtitle", "PGSSUB"),
    ("dvd_subtitle", "DVDSUB"),
    ("dvb_subtitle", "DVBSUB"),
    ("dvb_teletext", "DVBTXT"),
];

const CONTAINER_RENAMES: &[(&str, &str)] =
    &[("matroska", "mkv"), ("mpegts", "ts"), ("mpegvideo", "mpeg")];

const TRANSCODE_CONTAINER: &str = "ts";
const TRANSCODE_PROTOCOL: &str = "hls";
const TRANSCODE_MAX_AUDIO_CHANNELS: &str = "6";

// Codec sets come from Jellyfin's StreamBuilder._supportedHls* lists (alac
// dropped to stay under the server's 40-char AudioCodec query-param
// validator, ^[a-zA-Z0-9\-\._,|]{0,40}$):
// https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.Model/Dlna/StreamBuilder.cs#L31-L33
const TRANSCODE_VIDEO_CODEC: &[&str] = &["av1", "hevc", "h264", "vp9"];
const TRANSCODE_AUDIO_CODEC_MP4: &[&str] =
    &["opus", "aac", "eac3", "ac3", "flac", "mp3", "dts", "truehd"];
const TRANSCODE_AUDIO_CODEC_TS: &[&str] = &["aac", "eac3", "ac3", "mp3"];

fn rename_lookup(table: &[(&'static str, &'static str)], key: &str) -> Option<&'static str> {
    for (k, v) in table {
        if *k == key {
            return Some(*v);
        }
    }
    None
}

// Expand each input through `renames` and return the deduped union of raw +
// renamed names. Inputs may be comma-joined ffmpeg aliases (e.g.
// "matroska,webm"); each piece is split before lookup.
fn expand_with_renames(inputs: &[String], renames: &[(&'static str, &'static str)]) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut push = |s: &str| {
        if s.is_empty() {
            return;
        }
        if !out.iter().any(|e| e == s) {
            out.push(s.to_string());
        }
    };
    for input in inputs {
        for piece in input.split(',') {
            push(piece);
            if let Some(renamed) = rename_lookup(renames, piece) {
                push(renamed);
            }
        }
    }
    out
}

// Filter `items` down to entries present in `allowed`, preserving the order
// of `items` (Jellyfin treats this as the server's preference order).
fn filter_in_order(items: &[&str], allowed: &[String]) -> Vec<String> {
    items
        .iter()
        .filter(|s| allowed.iter().any(|a| a == *s))
        .map(|s| s.to_string())
        .collect()
}

fn join_csv(items: &[String]) -> String {
    items.join(",")
}

fn subtitle_profile(format: &str, method: &str) -> Value {
    let mut m = Map::new();
    m.insert("Format".to_string(), Value::String(format.to_string()));
    m.insert("Method".to_string(), Value::String(method.to_string()));
    Value::Object(m)
}

/// Build the DeviceProfile JSON.
pub fn build(
    decoders: &[Codec],
    demuxers: &[String],
    device_name: &str,
    _app_version: &str,
    force_transcode: bool,
) -> String {
    let mut video_codecs: Vec<String> = Vec::new();
    let mut audio_codecs: Vec<String> = Vec::new();
    let mut subtitle_codecs: Vec<String> = Vec::new();
    for c in decoders {
        match c.kind {
            MediaKind::Video => video_codecs.push(c.name.clone()),
            MediaKind::Audio => audio_codecs.push(c.name.clone()),
            MediaKind::Subtitle => subtitle_codecs.push(c.name.clone()),
        }
    }

    let video_csv = if force_transcode {
        String::new()
    } else {
        join_csv(&video_codecs)
    };
    let audio_csv = if force_transcode {
        String::new()
    } else {
        join_csv(&audio_codecs)
    };

    let containers = expand_with_renames(demuxers, CONTAINER_RENAMES);
    let subtitle_names = expand_with_renames(&subtitle_codecs, SUBTITLE_RENAMES);

    // TranscodingProfiles tell the server which formats it may transcode TO,
    // not what we can decode. Stick to the curated preference list intersected
    // with mpv's decoder support — adding the rest would (a) invite the server
    // to transcode to formats we don't want as targets (truehd, dts, vp9...)
    // and (b) push the AudioCodec CSV past the server's 40-char query-param
    // validator (^[a-zA-Z0-9\-\._,|]{0,40}$).
    let transcode_video_csv = join_csv(&filter_in_order(TRANSCODE_VIDEO_CODEC, &video_codecs));
    let transcode_audio_csv_mp4 =
        join_csv(&filter_in_order(TRANSCODE_AUDIO_CODEC_MP4, &audio_codecs));
    let transcode_audio_csv_ts =
        join_csv(&filter_in_order(TRANSCODE_AUDIO_CODEC_TS, &audio_codecs));

    // DirectPlayProfiles. ContainerHelper.ContainsContainer splits both the
    // profile's Container and the file's MediaSource.Container on comma and
    // does case-insensitive equality, so one entry with every container
    // comma-joined matches identically to N entries with one container each —
    // without repeating the codec CSV per container.
    let container_csv = join_csv(&containers);
    let mut direct_play: Vec<Value> = Vec::new();
    if !video_csv.is_empty() || force_transcode {
        let mut e = Map::new();
        e.insert(
            "Container".to_string(),
            Value::String(container_csv.clone()),
        );
        e.insert("Type".to_string(), Value::String("Video".to_string()));
        e.insert("VideoCodec".to_string(), Value::String(video_csv.clone()));
        e.insert("AudioCodec".to_string(), Value::String(audio_csv.clone()));
        direct_play.push(Value::Object(e));
    }
    if !audio_csv.is_empty() || force_transcode {
        let mut e = Map::new();
        e.insert(
            "Container".to_string(),
            Value::String(container_csv.clone()),
        );
        e.insert("Type".to_string(), Value::String("Audio".to_string()));
        e.insert("AudioCodec".to_string(), Value::String(audio_csv.clone()));
        direct_play.push(Value::Object(e));
    }
    {
        let mut e = Map::new();
        e.insert("Type".to_string(), Value::String("Photo".to_string()));
        direct_play.push(Value::Object(e));
    }

    // mpv handles both Embed and External natively, so no need to distinguish.
    let mut sub_profiles: Vec<Value> = Vec::new();
    for fmt in &subtitle_names {
        sub_profiles.push(subtitle_profile(fmt, "Embed"));
        sub_profiles.push(subtitle_profile(fmt, "External"));
    }

    // TranscodingProfiles: describes what server should produce when a
    // transcode is unavoidable. Order of VideoCodec/AudioCodec is the
    // server's preference order.
    let mut transcoding: Vec<Value> = Vec::new();
    transcoding.push(json!({ "Type": "Audio" }));
    if !force_transcode {
        let mut fmp4 = Map::new();
        fmp4.insert("Container".to_string(), Value::String("mp4".to_string()));
        fmp4.insert("Type".to_string(), Value::String("Video".to_string()));
        fmp4.insert("Protocol".to_string(), Value::String("hls".to_string()));
        fmp4.insert(
            "AudioCodec".to_string(),
            Value::String(transcode_audio_csv_mp4),
        );
        fmp4.insert(
            "VideoCodec".to_string(),
            Value::String(transcode_video_csv.clone()),
        );
        fmp4.insert(
            "MaxAudioChannels".to_string(),
            Value::String(TRANSCODE_MAX_AUDIO_CHANNELS.to_string()),
        );
        transcoding.push(Value::Object(fmp4));
    }
    {
        let mut v = Map::new();
        v.insert(
            "Container".to_string(),
            Value::String(TRANSCODE_CONTAINER.to_string()),
        );
        v.insert("Type".to_string(), Value::String("Video".to_string()));
        v.insert(
            "Protocol".to_string(),
            Value::String(TRANSCODE_PROTOCOL.to_string()),
        );
        v.insert(
            "AudioCodec".to_string(),
            Value::String(transcode_audio_csv_ts),
        );
        v.insert("VideoCodec".to_string(), Value::String(transcode_video_csv));
        v.insert(
            "MaxAudioChannels".to_string(),
            Value::String(TRANSCODE_MAX_AUDIO_CHANNELS.to_string()),
        );
        transcoding.push(Value::Object(v));
    }
    transcoding.push(json!({ "Container": "jpeg", "Type": "Photo" }));

    let mut profile = Map::new();
    profile.insert("Name".to_string(), Value::String(device_name.to_string()));
    profile.insert(
        "MaxStaticBitrate".to_string(),
        Value::from(1_000_000_000i64),
    );
    profile.insert(
        "MusicStreamingTranscodingBitrate".to_string(),
        Value::from(1_280_000i64),
    );
    profile.insert("TimelineOffsetSeconds".to_string(), Value::from(5i64));
    profile.insert("DirectPlayProfiles".to_string(), Value::Array(direct_play));
    profile.insert("TranscodingProfiles".to_string(), Value::Array(transcoding));
    profile.insert("SubtitleProfiles".to_string(), Value::Array(sub_profiles));
    profile.insert("ResponseProfiles".to_string(), Value::Array(Vec::new()));
    profile.insert("ContainerProfiles".to_string(), Value::Array(Vec::new()));
    profile.insert("CodecProfiles".to_string(), Value::Array(Vec::new()));

    serde_json::to_string(&Value::Object(profile)).expect("serde_json::to_string on owned Value")
}

// ---- C ABI ----

unsafe fn cstr_to_string(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

/// # Safety
/// `decoders` must point to `n_decoders` valid `JfnCodec` entries; each
/// `name` must be a valid NUL-terminated C string. `demuxers` must point to
/// `n_demuxers` valid C-string pointers. `device_name` and `app_version`
/// must be valid NUL-terminated C strings (or NULL → empty).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_jellyfin_build_device_profile(
    decoders: *const JfnCodec,
    n_decoders: usize,
    demuxers: *const *const c_char,
    n_demuxers: usize,
    device_name: *const c_char,
    app_version: *const c_char,
    force_transcode: bool,
) -> *mut c_char {
    let decoders_slice: &[JfnCodec] = if decoders.is_null() || n_decoders == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(decoders, n_decoders) }
    };
    let demuxers_slice: &[*const c_char] = if demuxers.is_null() || n_demuxers == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(demuxers, n_demuxers) }
    };

    let mut codecs: Vec<Codec> = Vec::with_capacity(decoders_slice.len());
    for c in decoders_slice {
        let kind = match c.kind {
            0 => MediaKind::Video,
            1 => MediaKind::Audio,
            2 => MediaKind::Subtitle,
            _ => continue,
        };
        codecs.push(Codec {
            name: unsafe { cstr_to_string(c.name) },
            kind,
        });
    }

    let demuxers_vec: Vec<String> = demuxers_slice
        .iter()
        .map(|p| unsafe { cstr_to_string(*p) })
        .collect();

    let name = unsafe { cstr_to_string(device_name) };
    let version = unsafe { cstr_to_string(app_version) };
    let json = build(&codecs, &demuxers_vec, &name, &version, force_transcode);
    match std::ffi::CString::new(json) {
        Ok(cs) => cs.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

/// # Safety
/// `s` must be a pointer previously returned by a `jfn_jellyfin_*` function
/// that returns `*mut c_char`, or NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_jellyfin_free_string(s: *mut c_char) {
    if s.is_null() {
        return;
    }
    drop(unsafe { CString::from_raw(s) });
}

// ---- URL helpers ----

/// Trim surrounding whitespace, lowercase `Http:`/`Https:` scheme prefixes,
/// and prepend `http://` when no scheme is present.
pub fn normalize_input(user_input: &str) -> String {
    let trimmed = user_input.trim();
    let mut s = String::with_capacity(trimmed.len() + 7);
    let lower_prefix = |s: &str, p: &str| s.len() >= p.len() && s[..p.len()].eq_ignore_ascii_case(p);
    if lower_prefix(trimmed, "http:") {
        s.push_str("http:");
        s.push_str(&trimmed[5..]);
    } else if lower_prefix(trimmed, "https:") {
        s.push_str("https:");
        s.push_str(&trimmed[6..]);
    } else {
        s.push_str(trimmed);
    }
    if !s.contains("://") {
        s.insert_str(0, "http://");
    }
    s
}

/// Reduce a URL to its server base:
///   - if the URL contains `/web` (case-insensitive) in its path, truncate
///     at the last occurrence;
///   - otherwise return the origin (everything up to the first `/` after
///     `://`, or the whole string if there's no path).
pub fn extract_base_url(url: &str) -> String {
    let lower = url.to_ascii_lowercase();
    if let Some(pos) = lower.rfind("/web") {
        return url[..pos].to_string();
    }
    let host_start = match url.find("://") {
        Some(i) => i + 3,
        None => 0,
    };
    match url[host_start..].find('/') {
        Some(rel) => url[..host_start + rel].to_string(),
        None => url.to_string(),
    }
}

/// Validate that a Jellyfin `/System/Info/Public` response body is a JSON
/// object with a non-empty string `Id` field.
pub fn is_valid_public_info(body: &[u8]) -> bool {
    let Ok(v) = serde_json::from_slice::<Value>(body) else {
        return false;
    };
    let Some(o) = v.as_object() else { return false };
    o.get("Id")
        .and_then(Value::as_str)
        .map(|s| !s.is_empty())
        .unwrap_or(false)
}

fn cstring_or_null(s: String) -> *mut c_char {
    match CString::new(s) {
        Ok(cs) => cs.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

/// # Safety
/// `input` must be a valid NUL-terminated C string (or NULL → empty).
/// Returns a malloc'd, NUL-terminated UTF-8 string; free with
/// `jfn_jellyfin_free_string`. Returns NULL only on interior-NUL serialization
/// failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_jellyfin_normalize_input(input: *const c_char) -> *mut c_char {
    let s = unsafe { cstr_to_string(input) };
    cstring_or_null(normalize_input(&s))
}

/// # Safety
/// `url` must be a valid NUL-terminated C string (or NULL → empty).
/// Returns a malloc'd, NUL-terminated UTF-8 string; free with
/// `jfn_jellyfin_free_string`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_jellyfin_extract_base_url(url: *const c_char) -> *mut c_char {
    let s = unsafe { cstr_to_string(url) };
    cstring_or_null(extract_base_url(&s))
}

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
    is_valid_public_info(slice)
}

// ---- Cached device-profile JSON ----
//
// Set once at startup in the browser process; read by WebBrowser when
// building extra_info for the renderer. Empty string until set.

static CACHED_PROFILE: Mutex<String> = Mutex::new(String::new());

/// # Safety
/// `json` must be a valid NUL-terminated C string, or NULL to clear.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_jellyfin_set_cached_profile(json: *const c_char) {
    let s = unsafe { cstr_to_string(json) };
    if let Ok(mut guard) = CACHED_PROFILE.lock() {
        *guard = s;
    }
}

/// Returns a malloc'd copy of the cached profile JSON (possibly empty).
/// Free with `jfn_jellyfin_free_string`.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_jellyfin_cached_profile() -> *mut c_char {
    let s = CACHED_PROFILE
        .lock()
        .map(|g| g.clone())
        .unwrap_or_default();
    cstring_or_null(s)
}

// ---- tests ----

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    fn parse(s: &str) -> Value {
        serde_json::from_str(s).expect("valid JSON")
    }

    fn codec(name: &str, kind: MediaKind) -> Codec {
        Codec {
            name: name.to_string(),
            kind,
        }
    }

    #[test]
    fn empty_capabilities_emits_photo_only_direct_play() {
        let s = build(&[], &[], "dev", "1.0", false);
        let v = parse(&s);
        let dp = v["DirectPlayProfiles"].as_array().unwrap();
        assert_eq!(dp.len(), 1);
        assert_eq!(dp[0]["Type"], "Photo");
    }

    #[test]
    fn force_transcode_empties_codec_csvs_and_drops_fmp4() {
        let decoders = vec![
            codec("h264", MediaKind::Video),
            codec("aac", MediaKind::Audio),
        ];
        let s = build(&decoders, &["matroska".into()], "dev", "1.0", true);
        let v = parse(&s);
        let dp = v["DirectPlayProfiles"].as_array().unwrap();
        let video = dp.iter().find(|e| e["Type"] == "Video").unwrap();
        assert_eq!(video["VideoCodec"], "");
        assert_eq!(video["AudioCodec"], "");
        let audio = dp.iter().find(|e| e["Type"] == "Audio").unwrap();
        assert_eq!(audio["AudioCodec"], "");

        let tp = v["TranscodingProfiles"].as_array().unwrap();
        // Audio + Video (ts) + Photo. No fmp4 entry under force_transcode.
        assert!(!tp.iter().any(|e| e["Container"] == "mp4"));
    }

    #[test]
    fn container_rename_expands_and_dedupes() {
        // Container CSV is only emitted when there are video/audio decoders.
        let decoders = vec![codec("h264", MediaKind::Video)];
        let s = build(&decoders, &["matroska,webm".into()], "dev", "1.0", false);
        let v = parse(&s);
        let dp = v["DirectPlayProfiles"].as_array().unwrap();
        let video = dp.iter().find(|e| e["Type"] == "Video").unwrap();
        let container = video["Container"].as_str().unwrap();
        let parts: Vec<&str> = container.split(',').collect();
        assert!(parts.contains(&"matroska"));
        assert!(parts.contains(&"webm"));
        assert!(parts.contains(&"mkv"));
        // No duplicates.
        let mut sorted = parts.clone();
        sorted.sort();
        sorted.dedup();
        assert_eq!(sorted.len(), parts.len());
    }

    #[test]
    fn subtitle_rename_emits_both_methods() {
        let decoders = vec![codec("subrip", MediaKind::Subtitle)];
        let s = build(&decoders, &[], "dev", "1.0", false);
        let v = parse(&s);
        let sp = v["SubtitleProfiles"].as_array().unwrap();
        let formats: Vec<&str> = sp.iter().map(|e| e["Format"].as_str().unwrap()).collect();
        assert!(formats.contains(&"subrip"));
        assert!(formats.contains(&"srt"));
        // Each format appears with both Embed and External.
        for fmt in ["subrip", "srt"] {
            let methods: Vec<&str> = sp
                .iter()
                .filter(|e| e["Format"] == fmt)
                .map(|e| e["Method"].as_str().unwrap())
                .collect();
            assert!(methods.contains(&"Embed"));
            assert!(methods.contains(&"External"));
        }
    }

    #[test]
    fn transcode_audio_csv_uses_curated_order_not_decoder_order() {
        let decoders = vec![
            codec("h264", MediaKind::Video),
            codec("mp3", MediaKind::Audio),
            codec("aac", MediaKind::Audio),
            codec("opus", MediaKind::Audio),
        ];
        let s = build(&decoders, &["matroska".into()], "dev", "1.0", false);
        let v = parse(&s);
        let tp = v["TranscodingProfiles"].as_array().unwrap();
        let fmp4 = tp
            .iter()
            .find(|e| e["Container"] == "mp4")
            .expect("fmp4 entry");
        assert_eq!(fmp4["AudioCodec"], "opus,aac,mp3");
    }

    #[test]
    fn normalize_trims_whitespace() {
        assert_eq!(normalize_input("  http://example.com  "), "http://example.com");
        assert_eq!(normalize_input("\thttps://host\n"), "https://host");
    }

    #[test]
    fn normalize_lowercases_scheme() {
        assert_eq!(normalize_input("HTTP://example.com"), "http://example.com");
        assert_eq!(normalize_input("HTTPS://example.com"), "https://example.com");
        assert_eq!(normalize_input("Http://example.com"), "http://example.com");
        assert_eq!(normalize_input("Https://example.com"), "https://example.com");
    }

    #[test]
    fn normalize_prepends_http_when_no_scheme() {
        assert_eq!(normalize_input("example.com"), "http://example.com");
        assert_eq!(normalize_input("example.com:8096"), "http://example.com:8096");
        assert_eq!(normalize_input("192.168.1.10"), "http://192.168.1.10");
    }

    #[test]
    fn normalize_trims_whitespace_before_prepending_scheme() {
        // Trim must happen first: otherwise a leading space would get trapped
        // between the prepended scheme and the host, producing "http:// host".
        assert_eq!(normalize_input(" example.com"), "http://example.com");
        assert_eq!(normalize_input("\texample.com\n"), "http://example.com");
        assert_eq!(normalize_input("   example.com  "), "http://example.com");
    }

    #[test]
    fn normalize_leaves_well_formed_input_unchanged() {
        assert_eq!(normalize_input("http://example.com"), "http://example.com");
        assert_eq!(
            normalize_input("https://example.com/jellyfin"),
            "https://example.com/jellyfin"
        );
    }

    #[test]
    fn normalize_passes_non_http_schemes_through() {
        // Only Http:/Https: prefixes are touched; anything else passes through.
        assert_eq!(normalize_input("FTP://example.com"), "FTP://example.com");
    }

    #[test]
    fn extract_base_truncates_at_web() {
        assert_eq!(extract_base_url("https://host/web/index.html"), "https://host");
        assert_eq!(extract_base_url("https://host/web"), "https://host");
    }

    #[test]
    fn extract_base_preserves_prefix_before_web() {
        assert_eq!(
            extract_base_url("https://host/jellyfin/web/index.html"),
            "https://host/jellyfin"
        );
        assert_eq!(
            extract_base_url("https://host:8096/jellyfin/web/"),
            "https://host:8096/jellyfin"
        );
    }

    #[test]
    fn extract_base_uses_last_web_when_multiple() {
        assert_eq!(
            extract_base_url("https://host/web/app/web/index.html"),
            "https://host/web/app"
        );
    }

    #[test]
    fn extract_base_case_insensitive_web() {
        assert_eq!(extract_base_url("https://host/WEB/index.html"), "https://host");
        assert_eq!(extract_base_url("https://host/Web/index.html"), "https://host");
        assert_eq!(extract_base_url("https://host/wEb/index.html"), "https://host");
    }

    #[test]
    fn extract_base_returns_origin_when_no_web() {
        assert_eq!(extract_base_url("https://host/"), "https://host");
        assert_eq!(extract_base_url("https://host"), "https://host");
        assert_eq!(extract_base_url("https://host/foo"), "https://host");
        assert_eq!(extract_base_url("http://host:8096/foo"), "http://host:8096");
    }

    #[test]
    fn extract_base_handles_port_in_origin() {
        assert_eq!(
            extract_base_url("http://host:8096/web/index.html"),
            "http://host:8096"
        );
        assert_eq!(extract_base_url("http://localhost:8096/web/"), "http://localhost:8096");
        assert_eq!(
            extract_base_url("http://192.168.1.100:8096/web/"),
            "http://192.168.1.100:8096"
        );
        assert_eq!(extract_base_url("http://[::1]:8096/web/"), "http://[::1]:8096");
    }

    #[test]
    fn extract_base_strips_query_and_fragment_after_web() {
        assert_eq!(extract_base_url("https://host/web/?foo=bar"), "https://host");
        assert_eq!(extract_base_url("https://host/web/#section"), "https://host");
        assert_eq!(
            extract_base_url("https://host/jellyfin/web/?foo=bar#section"),
            "https://host/jellyfin"
        );
    }

    #[test]
    fn extract_base_treats_website_and_webdav_as_web_match() {
        // Matches Qt behavior: substring match on "/web" does not distinguish
        // these longer path segments. Locked in here so a future fix is deliberate.
        assert_eq!(extract_base_url("https://host/website/"), "https://host");
        assert_eq!(extract_base_url("https://host/webdav/"), "https://host");
    }

    #[test]
    fn extract_base_handles_degenerate_urls() {
        assert_eq!(extract_base_url("https://"), "https://");
        assert_eq!(extract_base_url("https:///web/"), "https://");
    }

    #[test]
    fn idn_hosts_survive_unchanged() {
        assert_eq!(normalize_input("http://example.みんな"), "http://example.みんな");
        assert_eq!(normalize_input("example.みんな"), "http://example.みんな");
        assert_eq!(
            normalize_input("  HTTPS://example.みんな/web "),
            "https://example.みんな/web"
        );

        assert_eq!(
            extract_base_url("http://example.みんな/web/"),
            "http://example.みんな"
        );
        assert_eq!(
            extract_base_url("https://example.みんな/jellyfin/web"),
            "https://example.みんな/jellyfin"
        );
        assert_eq!(extract_base_url("http://example.みんな/"), "http://example.みんな");
    }

    #[test]
    fn public_info_accepts_object_with_non_empty_id() {
        assert!(is_valid_public_info(br#"{"Id":"abc","ServerName":"x"}"#));
        assert!(is_valid_public_info(br#"{"ServerName":"x","Id":"zzz"}"#));
    }

    #[test]
    fn public_info_rejects_empty_or_missing_id() {
        assert!(!is_valid_public_info(br#"{"Id":""}"#));
        assert!(!is_valid_public_info(br#"{"ServerName":"x"}"#));
        assert!(!is_valid_public_info(br#"{}"#));
    }

    #[test]
    fn public_info_rejects_non_string_id() {
        assert!(!is_valid_public_info(br#"{"Id":null}"#));
        assert!(!is_valid_public_info(br#"{"Id":123}"#));
        assert!(!is_valid_public_info(br#"{"Id":true}"#));
    }

    #[test]
    fn public_info_rejects_non_object_json() {
        assert!(!is_valid_public_info(br#"["Id"]"#));
        assert!(!is_valid_public_info(br#""Id""#));
        assert!(!is_valid_public_info(b"null"));
    }

    #[test]
    fn public_info_rejects_invalid_json() {
        assert!(!is_valid_public_info(b""));
        assert!(!is_valid_public_info(b"not json"));
        assert!(!is_valid_public_info(br#"{"Id":"abc""#));
    }

    #[test]
    fn public_info_no_false_positive_on_substring() {
        // Regression: the old C++ code string-matched "Id" which matched any
        // body containing that substring. Real JSON parse must reject these.
        assert!(!is_valid_public_info(
            br#"<html>oops "Id" lives here</html>"#
        ));
    }

    #[test]
    fn top_level_keys_in_expected_order() {
        let s = build(&[], &[], "dev", "1.0", false);
        let v: Value = parse(&s);
        let obj = v.as_object().unwrap();
        let keys: Vec<&str> = obj.keys().map(|k| k.as_str()).collect();
        assert_eq!(
            keys,
            vec![
                "Name",
                "MaxStaticBitrate",
                "MusicStreamingTranscodingBitrate",
                "TimelineOffsetSeconds",
                "DirectPlayProfiles",
                "TranscodingProfiles",
                "SubtitleProfiles",
                "ResponseProfiles",
                "ContainerProfiles",
                "CodecProfiles",
            ]
        );
    }
}
