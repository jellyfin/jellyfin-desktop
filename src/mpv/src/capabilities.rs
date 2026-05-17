//! Decoder + demuxer enumeration, matching the prior C++
//! `mpv_capabilities::Query` exactly.
//!
//! Two sources:
//! - **Decoders**: linked libavcodec, iterated via `av_codec_iterate` and
//!   classified by `AVMediaType`. Deduped by `AVCodecID` so wrapper
//!   variants (`h264`, `h264_qsv`, ...) collapse to one entry under the
//!   generic name returned by `avcodec_get_name` — Jellyfin matches
//!   against ffprobe-derived generic names.
//! - **Demuxers**: mpv property `demuxer-lavf-list`, an array of strings.
//!
//! mpv routes all decoding through libavcodec, so the resulting codec set
//! is identical to what mpv's `decoder-list` would report.

use crate::handle::Handle;
use crate::node::Node;
use std::collections::HashSet;
use std::ffi::CStr;

#[allow(non_camel_case_types, non_snake_case, non_upper_case_globals, dead_code)]
mod avcodec_sys {
    include!(concat!(env!("OUT_DIR"), "/avcodec_bindings.rs"));
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MediaKind {
    Video,
    Audio,
    Subtitle,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Codec {
    pub name: String,
    pub kind: MediaKind,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Capabilities {
    pub decoders: Vec<Codec>,
    pub demuxers: Vec<String>,
}

pub fn query(handle: Option<&Handle>) -> Capabilities {
    let mut caps = Capabilities {
        decoders: enumerate_decoders(),
        demuxers: Vec::new(),
    };
    if let Some(h) = handle {
        match h.get_property_node("demuxer-lavf-list") {
            Ok(node) => caps.demuxers = parse_string_list(&node),
            Err(e) => tracing::warn!(
                target: "mpv",
                "mpv_get_property(demuxer-lavf-list) failed: {}",
                e
            ),
        }
    }
    caps
}

fn enumerate_decoders() -> Vec<Codec> {
    let mut out = Vec::new();
    let mut seen: HashSet<i32> = HashSet::new();
    let mut iter: *mut std::os::raw::c_void = std::ptr::null_mut();
    loop {
        let codec = unsafe { avcodec_sys::av_codec_iterate(&mut iter) };
        if codec.is_null() {
            break;
        }
        let codec_ref = unsafe { &*codec };
        if unsafe { avcodec_sys::av_codec_is_decoder(codec) } == 0 {
            continue;
        }
        let kind = match codec_ref.type_ {
            avcodec_sys::AVMediaType::AVMEDIA_TYPE_VIDEO => MediaKind::Video,
            avcodec_sys::AVMediaType::AVMEDIA_TYPE_AUDIO => MediaKind::Audio,
            avcodec_sys::AVMediaType::AVMEDIA_TYPE_SUBTITLE => MediaKind::Subtitle,
            _ => continue,
        };
        let id = codec_ref.id.0 as i32;
        if !seen.insert(id) {
            continue;
        }
        let name_ptr = unsafe { avcodec_sys::avcodec_get_name(codec_ref.id) };
        if name_ptr.is_null() {
            continue;
        }
        let name = unsafe { CStr::from_ptr(name_ptr) }
            .to_string_lossy()
            .into_owned();
        if name.is_empty() {
            continue;
        }
        out.push(Codec { name, kind });
    }
    out
}

fn parse_string_list(root: &Node) -> Vec<String> {
    let Some(arr) = root.as_array() else {
        return Vec::new();
    };
    arr.iter()
        .filter_map(|v| match v {
            Node::String(s) if !s.is_empty() => Some(s.clone()),
            _ => None,
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_string_list_skips_non_strings() {
        let arr = Node::Array(vec![
            Node::String("mp4".into()),
            Node::Int(7),
            Node::String("mkv".into()),
            Node::None,
            Node::String(String::new()),
        ]);
        assert_eq!(parse_string_list(&arr), vec!["mp4", "mkv"]);
    }

    #[test]
    fn parse_string_list_rejects_non_array() {
        assert!(parse_string_list(&Node::None).is_empty());
        assert!(parse_string_list(&Node::Int(0)).is_empty());
    }

    #[test]
    fn enumerate_decoders_includes_h264_and_aac() {
        // Linked libavcodec must expose at least these two well-known
        // decoder ids under their generic names.
        let codecs = enumerate_decoders();
        assert!(!codecs.is_empty(), "expected non-empty decoder list");
        let names: HashSet<&str> = codecs.iter().map(|c| c.name.as_str()).collect();
        assert!(names.contains("h264"), "missing h264 decoder");
        assert!(names.contains("aac"), "missing aac decoder");

        // Dedup invariant: every entry has a unique name (one per AVCodecID).
        let mut seen = HashSet::new();
        for c in &codecs {
            assert!(seen.insert(c.name.clone()), "duplicate codec {}", c.name);
        }

        // Kind classification: at least one of each must be present.
        assert!(codecs.iter().any(|c| c.kind == MediaKind::Video));
        assert!(codecs.iter().any(|c| c.kind == MediaKind::Audio));
    }
}
