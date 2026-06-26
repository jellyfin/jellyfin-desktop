use cef::rc::Rc;
use cef::{
    EventFlags, ImplRunContextMenuCallback, ImplTask, MenuId, RunContextMenuCallback, Task,
    ThreadId, WrapTask, post_task, wrap_task,
};
use std::os::raw::{c_int, c_void};
use std::sync::Arc;

use crate::ipc::BrowserMessage;
use crate::sink_routing::Handle;

use super::Inner;

impl Inner {
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
        let pending = self.take_pending_menu_callback();
        // Ids below USER_FIRST are CEF built-in commands; only cont() executes them.
        if id >= 0 && id < MenuId::USER_FIRST.get_raw() as c_int {
            if let Some(cb) = pending {
                cb.cont(id, EventFlags::default());
            }
            return;
        }
        if let Some(cb) = pending {
            cb.cancel();
        }
        if id < 0 {
            return;
        }
        let inner = Arc::clone(self);
        let mut task = DispatchMenuCommandTask::new(inner, id);
        let _ = post_task(ThreadId::UI, Some(&mut task));
    }

    fn dispatch_menu_command(&self, id: c_int) {
        self.invoke_context_menu_dispatcher(id);
    }

    pub(crate) fn park_menu_selection(&self, cb: Box<dyn FnOnce(c_int) + Send>) {
        *self.pending_menu_on_selected.lock() = Some(cb);
    }

    pub(crate) fn take_parked_menu_selection(&self) -> Option<Box<dyn FnOnce(c_int) + Send>> {
        self.pending_menu_on_selected.lock().take()
    }

    fn take_pending_menu_callback(&self) -> Option<RunContextMenuCallback> {
        self.pending_menu_callback.lock().take()
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
