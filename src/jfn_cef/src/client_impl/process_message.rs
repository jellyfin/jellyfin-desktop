use cef::{Browser, ImplListValue, ImplProcessMessage, ProcessMessage};
use std::os::raw::c_int;
use std::sync::Arc;

use crate::app::userfree_to_string;
use crate::client::Inner;
use crate::ipc::BrowserMessage;

pub(super) fn on_process_message_received(
    inner: &Arc<Inner>,
    browser: Option<&mut Browser>,
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
                inner.set_popup_options(opts, selected);
            }
            1
        }
        "menuItemSelected" => {
            if inner.resolve_pending_menu_session()
                && let Some(args) = args
            {
                let cmd = args.int(0);
                inner.close_pending_menu();
                inner.handle_menu_item_selected(cmd, browser);
            }
            1
        }
        "menuDismissed" => {
            if inner.resolve_pending_menu_session() {
                inner.handle_menu_dismissed();
            }
            1
        }
        // Window controls (CSD) are handled centrally for every layer,
        // so no per-layer message handler needs to know about them.
        n if crate::window_controls::is_window_message(n) => {
            crate::window_controls::handle_window_op(n, args.as_ref(), browser);
            1
        }
        _ => {
            let browser = browser.map(|b| b.clone());
            let message = BrowserMessage::new(name, args, browser);
            if inner.invoke_message_handler(message) {
                1
            } else {
                0
            }
        }
    }
}
