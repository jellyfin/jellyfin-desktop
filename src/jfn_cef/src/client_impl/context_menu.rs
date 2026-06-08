use cef::rc::Rc;
use cef::*;
use std::os::raw::{c_int, c_void};
use std::sync::Arc;

use crate::app::userfree_to_string;
use crate::client::Inner;
use crate::platform_ops::{self, DisplayBackend, JfnContextMenuRequest, JfnMenuItem};

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
            let Some(session) = crate::browsers::jfn_browsers_menu_open() else {
                callback.cancel();
                return 1;
            };
            self.inner.store_pending_menu_callback(callback.clone());

            let native = platform_ops::ops()
                .map(|p| p.display() == DisplayBackend::X11)
                .unwrap_or(false);
            if native {
                let mut items = Vec::with_capacity(model.count());
                for i in 0..model.count() {
                    let t: sys::cef_menu_item_type_t = model.type_at(i).into();
                    if t == sys::cef_menu_item_type_t::MENUITEMTYPE_SEPARATOR {
                        items.push(JfnMenuItem {
                            id: 0,
                            label: String::new(),
                            enabled: false,
                            separator: true,
                        });
                    } else {
                        let raw_label = userfree_to_string(&model.label_at(i));
                        items.push(JfnMenuItem {
                            id: model.command_id_at(i),
                            label: strip_accelerator(&raw_label),
                            enabled: model.is_enabled_at(i) != 0,
                            separator: false,
                        });
                    }
                }
                if let Some(p) = platform_ops::ops() {
                    p.context_menu_show(
                        std::ptr::null_mut(),
                        JfnContextMenuRequest {
                            x: params.xcoord(),
                            y: params.ycoord(),
                            items,
                            on_selected: Some(self.inner.native_menu_callback(session)),
                        },
                    );
                }
                return 1;
            }

            self.inner.store_pending_menu_session(session);

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
