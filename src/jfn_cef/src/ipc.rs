//! Browser-process IPC helpers.
//!
//! This module is the boundary between CEF's `CefProcessMessage` transport and
//! the application/business handlers. CEF callback code constructs a typed
//! [`BrowserMessage`] once, then business modules consume browser/list wrappers
//! without receiving raw CEF pointers or owning-reference bookkeeping.

use cef::{
    Browser, CefString, Frame, ImplBrowser, ImplFrame, ImplListValue, ImplProcessMessage,
    ListValue, ProcessId, process_message_create, sys,
};

/// IPC message received in the browser process from renderer JavaScript.
pub(crate) struct BrowserMessage {
    name: String,
    args: Option<ListValue>,
    browser: Option<Browser>,
}

impl BrowserMessage {
    pub(crate) fn new(name: String, args: Option<ListValue>, browser: Option<Browser>) -> Self {
        Self {
            name,
            args,
            browser,
        }
    }

    pub(crate) fn name(&self) -> &str {
        &self.name
    }

    pub(crate) fn args(&self) -> Option<&ListValue> {
        self.args.as_ref()
    }

    pub(crate) fn browser(&self) -> Option<&Browser> {
        self.browser.as_ref()
    }

    pub(crate) fn main_frame(&self) -> Option<Frame> {
        self.browser.as_ref().and_then(|b| b.main_frame())
    }
}

/// Read a CefString out of a `ListValue` at `idx` as an owned Rust `String`.
pub(crate) fn list_string(args: &ListValue, idx: usize) -> String {
    crate::app::userfree_to_string(&args.string(idx))
}

/// Read an integer arg out of a `ListValue` at `idx`. Handles JS numbers
/// that the renderer happened to send as `VTYPE_DOUBLE` (e.g. via `parseFloat`).
pub(crate) fn list_int(args: &ListValue, idx: usize) -> i32 {
    let t = args.get_type(idx);
    if t.as_ref() == &sys::cef_value_type_t::VTYPE_DOUBLE {
        args.double(idx).round() as i32
    } else {
        args.int(idx)
    }
}

/// Build a `CefProcessMessage` named `name`, call `fill` to populate its
/// argument list, then ship it to the renderer process on `frame`.
pub(crate) fn send_to_renderer<F: FnOnce(&ListValue)>(frame: &Frame, name: &str, fill: F) {
    let Some(mut msg) = process_message_create(Some(&CefString::from(name))) else {
        return;
    };
    if let Some(args) = msg.argument_list() {
        fill(&args);
    }
    frame.send_process_message(
        ProcessId::from(sys::cef_process_id_t::PID_RENDERER),
        Some(&mut msg),
    );
}
