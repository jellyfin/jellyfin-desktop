//! Owned Rust mirror of `mpv_node`.
//!
//! libmpv hands out `mpv_node` trees via `mpv_get_property(..., MPV_FORMAT_NODE)`
//! and `mpv_event_property` payloads. Both forms reference memory owned by
//! libmpv that must be freed with `mpv_free_node_contents`. Rather than carry
//! that lifetime, `Node::from_raw` copies the tree into owned Rust values so
//! the caller can drop libmpv's allocation immediately.

use crate::sys;
use std::ffi::CStr;

#[derive(Clone, Debug, PartialEq)]
pub enum Node {
    None,
    String(String),
    Flag(bool),
    Int(i64),
    Double(f64),
    Array(NodeArray),
    Map(NodeMap),
    ByteArray(Vec<u8>),
}

pub type NodeArray = Vec<Node>;
pub type NodeMap = Vec<(String, Node)>;

impl Node {
    /// Deep-copy a raw `mpv_node` (from libmpv) into an owned `Node`. The
    /// caller still owns the raw node and must free it via
    /// `mpv_free_node_contents` if libmpv handed it out.
    ///
    /// # Safety
    /// `raw` must point to a valid `mpv_node` as produced by libmpv.
    pub unsafe fn from_raw(raw: *const sys::mpv_node) -> Self {
        if raw.is_null() {
            return Node::None;
        }
        let raw = unsafe { &*raw };
        match raw.format {
            sys::mpv_format::MPV_FORMAT_NONE => Node::None,
            sys::mpv_format::MPV_FORMAT_STRING => {
                let s = unsafe { raw.u.string };
                if s.is_null() {
                    Node::String(String::new())
                } else {
                    Node::String(
                        unsafe { CStr::from_ptr(s) }
                            .to_string_lossy()
                            .into_owned(),
                    )
                }
            }
            sys::mpv_format::MPV_FORMAT_FLAG => Node::Flag(unsafe { raw.u.flag } != 0),
            sys::mpv_format::MPV_FORMAT_INT64 => Node::Int(unsafe { raw.u.int64 }),
            sys::mpv_format::MPV_FORMAT_DOUBLE => Node::Double(unsafe { raw.u.double_ }),
            sys::mpv_format::MPV_FORMAT_NODE_ARRAY => unsafe {
                let list = raw.u.list;
                if list.is_null() {
                    return Node::Array(Vec::new());
                }
                let l = &*list;
                let mut out = Vec::with_capacity(l.num.max(0) as usize);
                for i in 0..l.num {
                    let v = l.values.add(i as usize);
                    out.push(Node::from_raw(v));
                }
                Node::Array(out)
            },
            sys::mpv_format::MPV_FORMAT_NODE_MAP => unsafe {
                let list = raw.u.list;
                if list.is_null() {
                    return Node::Map(Vec::new());
                }
                let l = &*list;
                let mut out = Vec::with_capacity(l.num.max(0) as usize);
                for i in 0..l.num {
                    let k = *l.keys.add(i as usize);
                    let key = if k.is_null() {
                        String::new()
                    } else {
                        CStr::from_ptr(k).to_string_lossy().into_owned()
                    };
                    let v = l.values.add(i as usize);
                    out.push((key, Node::from_raw(v)));
                }
                Node::Map(out)
            },
            sys::mpv_format::MPV_FORMAT_BYTE_ARRAY => unsafe {
                let ba = raw.u.ba;
                if ba.is_null() {
                    return Node::ByteArray(Vec::new());
                }
                let b = &*ba;
                if b.data.is_null() || b.size == 0 {
                    return Node::ByteArray(Vec::new());
                }
                let slice = std::slice::from_raw_parts(b.data as *const u8, b.size);
                Node::ByteArray(slice.to_vec())
            },
            _ => Node::None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        if let Node::String(s) = self { Some(s) } else { None }
    }

    pub fn as_int(&self) -> Option<i64> {
        if let Node::Int(v) = self { Some(*v) } else { None }
    }

    pub fn as_double(&self) -> Option<f64> {
        if let Node::Double(v) = self { Some(*v) } else { None }
    }

    pub fn as_flag(&self) -> Option<bool> {
        if let Node::Flag(v) = self { Some(*v) } else { None }
    }

    pub fn as_array(&self) -> Option<&NodeArray> {
        if let Node::Array(a) = self { Some(a) } else { None }
    }

    pub fn as_map(&self) -> Option<&NodeMap> {
        if let Node::Map(m) = self { Some(m) } else { None }
    }

    /// Lookup a key in a `Node::Map`. Returns `None` for non-maps or missing keys.
    pub fn get(&self, key: &str) -> Option<&Node> {
        self.as_map()?
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sys;
    use std::ffi::CString;

    fn raw_none() -> sys::mpv_node {
        let mut n: sys::mpv_node = unsafe { std::mem::zeroed() };
        n.format = sys::mpv_format::MPV_FORMAT_NONE;
        n
    }

    fn raw_int(v: i64) -> sys::mpv_node {
        let mut n = raw_none();
        n.format = sys::mpv_format::MPV_FORMAT_INT64;
        n.u.int64 = v;
        n
    }

    fn raw_double(v: f64) -> sys::mpv_node {
        let mut n = raw_none();
        n.format = sys::mpv_format::MPV_FORMAT_DOUBLE;
        n.u.double_ = v;
        n
    }

    fn raw_flag(v: bool) -> sys::mpv_node {
        let mut n = raw_none();
        n.format = sys::mpv_format::MPV_FORMAT_FLAG;
        n.u.flag = if v { 1 } else { 0 };
        n
    }

    #[test]
    fn null_pointer_decodes_to_none() {
        let n = unsafe { Node::from_raw(std::ptr::null()) };
        assert_eq!(n, Node::None);
    }

    #[test]
    fn scalar_formats_round_trip() {
        let n = raw_int(42);
        assert_eq!(unsafe { Node::from_raw(&n) }, Node::Int(42));
        let n = raw_double(2.5);
        assert_eq!(unsafe { Node::from_raw(&n) }, Node::Double(2.5));
        let n = raw_flag(true);
        assert_eq!(unsafe { Node::from_raw(&n) }, Node::Flag(true));
        let n = raw_flag(false);
        assert_eq!(unsafe { Node::from_raw(&n) }, Node::Flag(false));
    }

    #[test]
    fn map_decodes_keys_and_values() {
        // { "w": 1920, "h": 1080 }
        let mut values = vec![raw_int(1920), raw_int(1080)];
        let key_w = CString::new("w").unwrap();
        let key_h = CString::new("h").unwrap();
        let mut keys: Vec<*mut std::os::raw::c_char> =
            vec![key_w.as_ptr() as *mut _, key_h.as_ptr() as *mut _];
        let list = sys::mpv_node_list {
            num: 2,
            values: values.as_mut_ptr(),
            keys: keys.as_mut_ptr(),
        };
        let mut root = raw_none();
        root.format = sys::mpv_format::MPV_FORMAT_NODE_MAP;
        root.u.list = &list as *const _ as *mut _;

        let n = unsafe { Node::from_raw(&root) };
        assert_eq!(n.get("w").and_then(|v| v.as_int()), Some(1920));
        assert_eq!(n.get("h").and_then(|v| v.as_int()), Some(1080));
        assert!(n.get("missing").is_none());
    }

    #[test]
    fn array_decodes_in_order() {
        let mut values = vec![raw_int(1), raw_int(2), raw_int(3)];
        let list = sys::mpv_node_list {
            num: 3,
            values: values.as_mut_ptr(),
            keys: std::ptr::null_mut(),
        };
        let mut root = raw_none();
        root.format = sys::mpv_format::MPV_FORMAT_NODE_ARRAY;
        root.u.list = &list as *const _ as *mut _;

        let n = unsafe { Node::from_raw(&root) };
        let arr = n.as_array().unwrap();
        assert_eq!(arr.len(), 3);
        assert_eq!(arr[0].as_int(), Some(1));
        assert_eq!(arr[2].as_int(), Some(3));
    }

    #[test]
    fn empty_list_decodes() {
        let list = sys::mpv_node_list {
            num: 0,
            values: std::ptr::null_mut(),
            keys: std::ptr::null_mut(),
        };
        let mut root = raw_none();
        root.format = sys::mpv_format::MPV_FORMAT_NODE_ARRAY;
        root.u.list = &list as *const _ as *mut _;
        let n = unsafe { Node::from_raw(&root) };
        assert_eq!(n.as_array().map(|a| a.len()), Some(0));
    }
}
