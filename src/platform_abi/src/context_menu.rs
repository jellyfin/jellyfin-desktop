use std::ffi::c_int;

use crate::DisplayBackend;

pub struct JfnMenuItem {
    pub id: c_int,
    pub label: String,
    pub enabled: bool,
    pub separator: bool,
}

/// Selection callback: receives the chosen item id, or `-1` for dismissed.
pub type MenuSelectionFn = Box<dyn FnOnce(c_int) + Send>;

pub struct JsMenuChannel {
    pub exec: Box<dyn FnOnce(String)>,
    /// Stores `on_selected` until the menuItemSelected / menuDismissed IPC
    /// fires it.
    pub park_selection: Box<dyn FnOnce(MenuSelectionFn)>,
    pub on_selected: MenuSelectionFn,
}

pub enum Delivery {
    Native(MenuSelectionFn),
    Js(JsMenuChannel),
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum DeliveryKind {
    Native,
    Js,
}

pub struct JfnContextMenuRequest {
    /// Logical (CEF view) coordinates of the click, not physical pixels.
    pub x: c_int,
    pub y: c_int,
    pub items: Vec<JfnMenuItem>,
    pub delivery: Delivery,
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum ContextMenuStyle {
    PlatformMenu,
    JsMenu,
}

pub fn context_menu_style(b: DisplayBackend) -> ContextMenuStyle {
    match b {
        DisplayBackend::Wayland => ContextMenuStyle::PlatformMenu,
        DisplayBackend::X11 => ContextMenuStyle::PlatformMenu,
        DisplayBackend::Windows => ContextMenuStyle::JsMenu,
        DisplayBackend::MacOS => ContextMenuStyle::PlatformMenu,
    }
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum ContextMenuScript {
    ContextMenu,
}

pub trait ContextMenuBackend: Send + Sync {
    fn scripts(&self) -> &'static [ContextMenuScript] {
        &[]
    }
    fn delivery_kind(&self) -> DeliveryKind {
        DeliveryKind::Native
    }
    fn show(&self, req: JfnContextMenuRequest);
}

pub struct JsMenuContextMenu;

impl ContextMenuBackend for JsMenuContextMenu {
    fn scripts(&self) -> &'static [ContextMenuScript] {
        &[ContextMenuScript::ContextMenu]
    }

    fn delivery_kind(&self) -> DeliveryKind {
        DeliveryKind::Js
    }

    fn show(&self, req: JfnContextMenuRequest) {
        let Delivery::Js(js) = req.delivery else {
            debug_assert!(false, "JsMenuContextMenu requires Delivery::Js");
            return;
        };
        (js.park_selection)(js.on_selected);
        (js.exec)(format!(
            "window._showContextMenu({},{},{})",
            items_json(&req.items),
            req.x,
            req.y
        ));
    }
}

fn items_json(items: &[JfnMenuItem]) -> String {
    let mut out = String::from("[");
    for (i, it) in items.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        if it.separator {
            out.push_str("{\"sep\":true}");
        } else {
            out.push_str("{\"id\":");
            out.push_str(&it.id.to_string());
            out.push_str(",\"label\":");
            push_js_string(&mut out, &it.label);
            out.push_str(",\"enabled\":");
            out.push_str(if it.enabled { "true" } else { "false" });
            out.push('}');
        }
    }
    out.push(']');
    out
}

/// JSON string escape, plus U+2028/U+2029 — valid in JSON but not in a JS
/// source literal, and this string is fed to `ExecuteJavaScript`.
fn push_js_string(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{2028}' => out.push_str("\\u2028"),
            '\u{2029}' => out.push_str("\\u2029"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}
