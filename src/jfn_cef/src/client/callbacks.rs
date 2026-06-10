use cef::rc::Rc;
use cef::{
    Browser, ImplBrowser, ImplFrame, ImplRunContextMenuCallback, ImplTask, MenuId,
    RunContextMenuCallback, Task, ThreadId, WrapTask, post_task, wrap_task,
};
use std::os::raw::{c_int, c_void};
use std::sync::Arc;

use crate::ipc::BrowserMessage;
use crate::sink_routing::Handle;

use super::Inner;

impl Inner {
    pub(crate) fn handle_menu_item_selected(&self, cmd: c_int, browser: Option<&mut Browser>) {
        let Some(b) = browser else { return };
        let frame = b.focused_frame().or_else(|| b.main_frame());
        let menu_back = MenuId::BACK.get_raw() as c_int;
        let menu_forward = MenuId::FORWARD.get_raw() as c_int;
        let menu_reload = MenuId::RELOAD.get_raw() as c_int;
        let menu_reload_nocache = MenuId::RELOAD_NOCACHE.get_raw() as c_int;
        let menu_stop = MenuId::STOPLOAD.get_raw() as c_int;
        let menu_undo = MenuId::UNDO.get_raw() as c_int;
        let menu_redo = MenuId::REDO.get_raw() as c_int;
        let menu_cut = MenuId::CUT.get_raw() as c_int;
        let menu_copy = MenuId::COPY.get_raw() as c_int;
        let menu_paste = MenuId::PASTE.get_raw() as c_int;
        let menu_select_all = MenuId::SELECT_ALL.get_raw() as c_int;
        if cmd == menu_back {
            b.go_back();
        } else if cmd == menu_forward {
            b.go_forward();
        } else if cmd == menu_reload {
            b.reload();
        } else if cmd == menu_reload_nocache {
            b.reload_ignore_cache();
        } else if cmd == menu_stop {
            b.stop_load();
        } else if cmd == menu_undo {
            if let Some(f) = frame {
                f.undo()
            }
        } else if cmd == menu_redo {
            if let Some(f) = frame {
                f.redo()
            }
        } else if cmd == menu_cut {
            if let Some(f) = frame {
                f.cut()
            }
        } else if cmd == menu_copy {
            if let Some(f) = frame {
                f.copy()
            }
        } else if cmd == menu_paste {
            if let Some(f) = frame {
                f.paste()
            }
        } else if cmd == menu_select_all {
            if let Some(f) = frame {
                f.select_all()
            }
        } else {
            self.invoke_context_menu_dispatcher(cmd);
        }
    }

    pub(crate) fn menu_selection_callback(
        self: &Arc<Self>,
        session: Handle,
    ) -> Box<dyn FnOnce(c_int) + Send> {
        let inner = Arc::clone(self);
        Box::new(move |id| {
            let mut task = DispatchMenuResultTask::new(inner, session, id);
            let _ = post_task(ThreadId::UI, Some(&mut task));
        })
    }

    fn dispatch_menu_result(self: &Arc<Self>, session: Handle, id: c_int) {
        if !crate::browsers::jfn_browsers_menu_resolve(session) {
            return;
        }
        self.close_pending_menu();
        if id < 0 {
            return;
        }
        let inner = Arc::clone(self);
        let mut task = DispatchMenuCommandTask::new(inner, id);
        let _ = post_task(ThreadId::UI, Some(&mut task));
    }

    fn dispatch_menu_command(&self, id: c_int) {
        let mut browser = self.browser_clone();
        self.handle_menu_item_selected(id, browser.as_mut());
    }

    pub(crate) fn park_menu_selection(&self, cb: Box<dyn FnOnce(c_int) + Send>) {
        *self.pending_menu_on_selected.lock() = Some(cb);
    }

    pub(crate) fn take_parked_menu_selection(&self) -> Option<Box<dyn FnOnce(c_int) + Send>> {
        self.pending_menu_on_selected.lock().take()
    }

    pub(crate) fn close_pending_menu(&self) {
        let mut g = self.pending_menu_callback.lock();
        if let Some(cb) = g.take() {
            cb.cancel();
        }
    }

    pub(crate) fn store_pending_menu_callback(&self, cb: RunContextMenuCallback) {
        let mut g = self.pending_menu_callback.lock();
        if let Some(prev) = g.take() {
            prev.cancel();
        }
        *g = Some(cb);
    }

    pub(crate) fn invoke_message_handler(&self, message: BrowserMessage) -> bool {
        let g = self.message_handler.lock();
        g.as_ref().map(|f| f(message)).unwrap_or(false)
    }

    pub(crate) fn has_context_menu_builder(&self) -> bool {
        self.context_menu_builder.lock().is_some()
    }

    pub(crate) fn invoke_context_menu_builder(&self, menu_model_raw: *mut c_void) {
        let g = self.context_menu_builder.lock();
        if let Some(f) = g.as_ref() {
            f(menu_model_raw);
        }
    }

    fn invoke_context_menu_dispatcher(&self, command_id: c_int) -> bool {
        let g = self.context_menu_dispatcher.lock();
        g.as_ref().map(|f| f(command_id)).unwrap_or(false)
    }
}

wrap_task! {
    struct DispatchMenuResultTask {
        inner: Arc<Inner>,
        session: Handle,
        id: c_int,
    }
    impl Task {
        fn execute(&self) {
            self.inner.dispatch_menu_result(self.session, self.id);
        }
    }
}

wrap_task! {
    struct DispatchMenuCommandTask {
        inner: Arc<Inner>,
        id: c_int,
    }
    impl Task {
        fn execute(&self) {
            self.inner.dispatch_menu_command(self.id);
        }
    }
}
