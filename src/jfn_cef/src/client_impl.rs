//! `cef::Client` impl + 6 handler impls for `CefLayer`.
//!
//! Routes CEF callbacks into `client::Inner` methods. Browser ownership lives
//! here too: `LifeSpanHandler::on_after_created` stashes the `cef::Browser`
//! into Inner so subsequent host/frame ops dispatch via cef-rs instead of an
//! FFI vtable.

use cef::rc::Rc;
use cef::*;
use std::os::raw::{c_int, c_void};
use std::sync::Arc;

use crate::app::userfree_to_string;
use crate::client::Inner;
use crate::platform_ops;

const STRIP_ACCEL_KEEP: u8 = b'&';

fn strip_accelerator(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        if b != STRIP_ACCEL_KEEP {
            out.push(b as char);
        }
    }
    // Above only works for ASCII '&'; non-ASCII chars round-trip via str chars
    if s.is_ascii() {
        return out;
    }
    s.chars().filter(|c| *c != '&').collect()
}

#[cfg(target_os = "macos")]
const ACTION_MODIFIER: u32 = sys::cef_event_flags_t::EVENTFLAG_COMMAND_DOWN.0 as u32;
#[cfg(not(target_os = "macos"))]
const ACTION_MODIFIER: u32 = sys::cef_event_flags_t::EVENTFLAG_CONTROL_DOWN.0 as u32;
const ALT_FLAG: u32 = sys::cef_event_flags_t::EVENTFLAG_ALT_DOWN.0 as u32;

fn is_paste_shortcut(e: &KeyEvent) -> bool {
    let kt: sys::cef_key_event_type_t = e.type_.into();
    if kt != sys::cef_key_event_type_t::KEYEVENT_RAWKEYDOWN {
        return false;
    }
    if (e.modifiers & ACTION_MODIFIER) == 0 {
        return false;
    }
    if (e.modifiers & ALT_FLAG) != 0 {
        return false;
    }
    e.windows_key_code == b'V' as i32
}

pub fn make_client(inner: Arc<Inner>) -> Client {
    JfnClientBuilder::new(inner)
}

wrap_client! {
    pub struct JfnClientBuilder {
        inner: Arc<Inner>,
    }

    impl Client {
        fn render_handler(&self) -> Option<RenderHandler> {
            Some(JfnRenderHandlerBuilder::new(self.inner.clone()))
        }
        fn life_span_handler(&self) -> Option<LifeSpanHandler> {
            Some(JfnLifeSpanHandlerBuilder::new(self.inner.clone()))
        }
        fn load_handler(&self) -> Option<LoadHandler> {
            Some(JfnLoadHandlerBuilder::new(self.inner.clone()))
        }
        fn context_menu_handler(&self) -> Option<ContextMenuHandler> {
            Some(JfnContextMenuHandlerBuilder::new(self.inner.clone()))
        }
        fn display_handler(&self) -> Option<DisplayHandler> {
            Some(JfnDisplayHandlerBuilder::new(self.inner.clone()))
        }
        fn keyboard_handler(&self) -> Option<KeyboardHandler> {
            Some(JfnKeyboardHandlerBuilder::new(self.inner.clone()))
        }
        fn on_process_message_received(
            &self,
            browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            _source_process: ProcessId,
            message: Option<&mut ProcessMessage>,
        ) -> c_int {
            let Some(msg) = message else { return 0 };
            let name = userfree_to_string(&msg.name());
            let args = msg.argument_list();
            match name.as_str() {
                "popupOptions" => {
                    if let Some(args) = args {
                        let list = args.list(0);
                        let (opts, selected) = if let Some(list) = list {
                            let n = list.size();
                            let mut v = Vec::with_capacity(n);
                            for i in 0..n {
                                v.push(userfree_to_string(&list.string(i)));
                            }
                            (v, args.int(1))
                        } else {
                            (Vec::new(), args.int(1))
                        };
                        self.inner.set_popup_options(opts, selected);
                    }
                    1
                }
                "menuItemSelected" => {
                    if let Some(args) = args {
                        let cmd = args.int(0);
                        self.inner.handle_menu_item_selected(cmd, browser);
                    }
                    1
                }
                "menuDismissed" => {
                    self.inner.handle_menu_dismissed();
                    1
                }
                _ => {
                    // C++ side calls CefBrowserCToCpp::Wrap / CefListValueCToCpp::Wrap which
                    // adopt one owning reference. Rust still holds its own ref via the
                    // Browser/ListValue wrappers, so add_ref before transferring ownership.
                    let browser_raw = browser
                        .map(|b| {
                            unsafe { Rc::add_ref(b) };
                            ImplBrowser::get_raw(b) as *mut c_void
                        })
                        .unwrap_or(std::ptr::null_mut());
                    let args_raw = args
                        .as_ref()
                        .map(|a| {
                            unsafe { Rc::add_ref(a) };
                            ImplListValue::get_raw(a) as *mut c_void
                        })
                        .unwrap_or(std::ptr::null_mut());
                    if self.inner.invoke_message_handler(&name, args_raw, browser_raw) {
                        1
                    } else {
                        0
                    }
                }
            }
        }
    }
}

// ----- RenderHandler -------------------------------------------------------

wrap_render_handler! {
    pub struct JfnRenderHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl RenderHandler {
        fn view_rect(&self, _browser: Option<&mut Browser>, rect: Option<&mut Rect>) {
            let Some(r) = rect else { return };
            let (w, h) = self.inner.view_size();
            r.x = 0;
            r.y = 0;
            r.width = w;
            r.height = h;
        }
        fn screen_info(
            &self,
            _browser: Option<&mut Browser>,
            screen_info: Option<&mut ScreenInfo>,
        ) -> c_int {
            let Some(si) = screen_info else { return 0 };
            let (scale, w, h) = self.inner.screen_info_values();
            si.device_scale_factor = scale;
            si.rect = Rect { x: 0, y: 0, width: w, height: h };
            si.available_rect = si.rect.clone();
            1
        }
        fn on_popup_show(&self, _browser: Option<&mut Browser>, show: c_int) {
            self.inner.on_popup_show(show != 0);
        }
        fn on_popup_size(&self, _browser: Option<&mut Browser>, rect: Option<&Rect>) {
            let Some(r) = rect else { return };
            self.inner.on_popup_size(r.x, r.y, r.width, r.height);
        }
        fn on_paint(
            &self,
            _browser: Option<&mut Browser>,
            type_: PaintElementType,
            dirty_rects: Option<&[Rect]>,
            buffer: *const u8,
            width: c_int,
            height: c_int,
        ) {
            let kind: sys::cef_paint_element_type_t = type_.into();
            let is_popup = match kind {
                sys::cef_paint_element_type_t::PET_POPUP => true,
                sys::cef_paint_element_type_t::PET_VIEW => false,
                _ => return,
            };
            let rects: Vec<platform_ops::JfnRect> = dirty_rects
                .map(|d| {
                    d.iter()
                        .map(|r| platform_ops::JfnRect { x: r.x, y: r.y, w: r.width, h: r.height })
                        .collect()
                })
                .unwrap_or_default();
            self.inner.on_paint(
                is_popup,
                if rects.is_empty() { std::ptr::null() } else { rects.as_ptr() },
                rects.len(),
                buffer as *const c_void,
                width,
                height,
            );
        }
        fn on_accelerated_paint(
            &self,
            _browser: Option<&mut Browser>,
            type_: PaintElementType,
            _dirty_rects: Option<&[Rect]>,
            info: Option<&AcceleratedPaintInfo>,
        ) {
            let kind: sys::cef_paint_element_type_t = type_.into();
            let is_popup = match kind {
                sys::cef_paint_element_type_t::PET_POPUP => true,
                sys::cef_paint_element_type_t::PET_VIEW => false,
                _ => return,
            };
            let Some(info) = info else { return };
            // Convert back to the C-layout struct so the platform vtable can
            // cast `const void*` to `CefAcceleratedPaintInfo*`.
            let raw: sys::_cef_accelerated_paint_info_t = info.clone().into();
            self.inner.on_accelerated_paint(is_popup, &raw as *const _ as *const c_void);
        }
    }
}

// ----- LifeSpanHandler -----------------------------------------------------

wrap_life_span_handler! {
    pub struct JfnLifeSpanHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl LifeSpanHandler {
        fn on_after_created(&self, browser: Option<&mut Browser>) {
            let Some(b) = browser else { return };
            self.inner.handle_on_after_created(b.clone());
        }
        fn on_before_close(&self, _browser: Option<&mut Browser>) {
            self.inner.handle_on_before_close();
        }
        fn on_before_popup(
            &self,
            _browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            _popup_id: c_int,
            target_url: Option<&CefString>,
            _target_frame_name: Option<&CefString>,
            _target_disposition: WindowOpenDisposition,
            _user_gesture: c_int,
            _popup_features: Option<&PopupFeatures>,
            _window_info: Option<&mut WindowInfo>,
            _client: Option<&mut Option<Client>>,
            _settings: Option<&mut BrowserSettings>,
            _extra_info: Option<&mut Option<DictionaryValue>>,
            _no_javascript_access: Option<&mut c_int>,
        ) -> c_int {
            let url = target_url.map(|s| s.to_string()).unwrap_or_default();
            if self.inner.on_before_popup(&url) { 1 } else { 0 }
        }
    }
}

// ----- LoadHandler ---------------------------------------------------------

wrap_load_handler! {
    pub struct JfnLoadHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl LoadHandler {
        fn on_load_end(
            &self,
            _browser: Option<&mut Browser>,
            frame: Option<&mut Frame>,
            http_status_code: c_int,
        ) {
            let Some(f) = frame else { return };
            let is_main = f.is_main() == 1;
            let url = userfree_to_string(&f.url());
            self.inner.on_load_end(is_main, http_status_code, &url);
        }
        fn on_load_error(
            &self,
            _browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            error_code: Errorcode,
            error_text: Option<&CefString>,
            failed_url: Option<&CefString>,
        ) {
            let code: sys::cef_errorcode_t = error_code.into();
            let text = error_text.map(|s| s.to_string()).unwrap_or_default();
            let url = failed_url.map(|s| s.to_string()).unwrap_or_default();
            self.inner.on_load_error(code as c_int, &text, &url);
        }
    }
}

// ----- ContextMenuHandler --------------------------------------------------

wrap_context_menu_handler! {
    pub struct JfnContextMenuHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl ContextMenuHandler {
        fn on_before_context_menu(
            &self,
            _browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            _params: Option<&mut ContextMenuParams>,
            model: Option<&mut MenuModel>,
        ) {
            let Some(m) = model else { return };
            m.remove(MenuId::PRINT.get_raw() as c_int);
            m.remove(MenuId::VIEW_SOURCE.get_raw() as c_int);
            let reload_id: c_int = MenuId::RELOAD.get_raw() as c_int;
            if m.index_of(reload_id) < 0 {
                m.add_item(reload_id, Some(&CefString::from("Reload")));
            }
            // Trim trailing separators left after removals.
            loop {
                let n = m.count();
                if n == 0 {
                    break;
                }
                let t: sys::cef_menu_item_type_t = m.type_at(n - 1).into();
                if t == sys::cef_menu_item_type_t::MENUITEMTYPE_SEPARATOR {
                    m.remove_at(n - 1);
                } else {
                    break;
                }
            }
            if self.inner.has_context_menu_builder() {
                m.add_separator();
                // C++ thunk uses CefMenuModelCToCpp::Wrap which adopts one ref.
                unsafe { Rc::add_ref(m) };
                let raw = ImplMenuModel::get_raw(m) as *mut c_void;
                self.inner.invoke_context_menu_builder(raw);
            }
        }
        fn run_context_menu(
            &self,
            browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            params: Option<&mut ContextMenuParams>,
            model: Option<&mut MenuModel>,
            callback: Option<&mut RunContextMenuCallback>,
        ) -> c_int {
            let (Some(browser), Some(params), Some(model), Some(callback)) =
                (browser, params, model, callback)
            else {
                return 0;
            };
            if model.count() == 0 {
                callback.cancel();
                return 1;
            }
            self.inner.store_pending_menu_callback(callback.clone());

            // Serialize menu model via CEF's value/list/json APIs (never hand-rolled).
            let Some(arr) = list_value_create() else { return 1 };
            for i in 0..model.count() {
                let Some(item) = dictionary_value_create() else { continue };
                let t: sys::cef_menu_item_type_t = model.type_at(i).into();
                if t == sys::cef_menu_item_type_t::MENUITEMTYPE_SEPARATOR {
                    item.set_bool(Some(&CefString::from("sep")), 1);
                } else {
                    let id = model.command_id_at(i);
                    let label_uf = model.label_at(i);
                    let raw_label = userfree_to_string(&label_uf);
                    let label = strip_accelerator(&raw_label);
                    item.set_int(Some(&CefString::from("id")), id);
                    item.set_string(
                        Some(&CefString::from("label")),
                        Some(&CefString::from(label.as_str())),
                    );
                    item.set_bool(
                        Some(&CefString::from("enabled")),
                        if model.is_enabled_at(i) != 0 { 1 } else { 0 },
                    );
                }
                let mut item = item;
                let idx = arr.size();
                arr.set_dictionary(idx, Some(&mut item));
            }
            let Some(call_args) = list_value_create() else { return 1 };
            let mut arr = arr;
            call_args.set_list(0, Some(&mut arr));
            call_args.set_int(1, params.xcoord());
            call_args.set_int(2, params.ycoord());
            let Some(root) = value_create() else { return 1 };
            let mut call_args = call_args;
            root.set_list(Some(&mut call_args));
            let mut root = root;
            let json_uf = write_json(Some(&mut root), JsonWriterOptions::DEFAULT);
            let json = userfree_to_string(&json_uf);
            let js = format!("window._showContextMenu.apply(null,{})", json);
            if let Some(frame) = browser.main_frame() {
                let code = CefString::from(js.as_str());
                frame.execute_java_script(Some(&code), Some(&CefString::from("")), 0);
            }
            1
        }
    }
}

// ----- DisplayHandler ------------------------------------------------------

wrap_display_handler! {
    pub struct JfnDisplayHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl DisplayHandler {
        fn on_fullscreen_mode_change(
            &self,
            _browser: Option<&mut Browser>,
            fullscreen: c_int,
        ) {
            self.inner.on_fullscreen_mode_change(fullscreen != 0);
        }
        fn on_cursor_change(
            &self,
            _browser: Option<&mut Browser>,
            _cursor: ::std::os::raw::c_ulong,
            type_: CursorType,
            _custom_cursor_info: Option<&CursorInfo>,
        ) -> c_int {
            let t: sys::cef_cursor_type_t = type_.into();
            self.inner.on_cursor_change(t as c_int);
            1
        }
        fn on_console_message(
            &self,
            _browser: Option<&mut Browser>,
            level: LogSeverity,
            message: Option<&CefString>,
            source: Option<&CefString>,
            line: c_int,
        ) -> c_int {
            let lvl: sys::cef_log_severity_t = level.into();
            let msg = message.map(|s| s.to_string()).unwrap_or_default();
            let src = source.map(|s| s.to_string()).unwrap_or_default();
            self.inner.on_console_message(lvl as c_int, &msg, &src, line);
            1
        }
    }
}

// ----- KeyboardHandler -----------------------------------------------------

wrap_keyboard_handler! {
    pub struct JfnKeyboardHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl KeyboardHandler {
        fn on_pre_key_event(
            &self,
            _browser: Option<&mut Browser>,
            event: Option<&KeyEvent>,
            _os_event: Option<&mut sys::XEvent>,
            _is_keyboard_shortcut: Option<&mut c_int>,
        ) -> c_int {
            let Some(e) = event else { return 0 };
            if !is_paste_shortcut(e) {
                return 0;
            }
            if self.inner.try_paste() { 1 } else { 0 }
        }
    }
}
