use cef::rc::Rc;
use cef::{
    CefString, ImplBrowserHost, ImplFrame, ImplListValue, ImplProcessMessage, ImplTask, MouseEvent,
    ProcessId, Task, ThreadId, WrapTask, post_task, process_message_create, sys, wrap_task,
};
use std::sync::Arc;
use std::sync::atomic::Ordering;

use crate::platform_ops;

use super::{Inner, PopupState};

impl Inner {
    fn reset_popup_state(p: &mut PopupState) {
        p.size_received = false;
        p.options_received = false;
        p.options.clear();
        p.selected_idx = -1;
    }

    pub(crate) fn on_popup_show(&self, show: bool) {
        {
            let mut p = self.popup.lock();
            p.visible = show;
            Self::reset_popup_state(&mut p);
        }
        if !show {
            let surface = self.surface_ptr();
            if !surface.is_null()
                && let Some(p) = platform_ops::ops()
            {
                p.popup_hide(surface);
            }
            return;
        }
        // Ask the renderer to walk the focused <select> and ship the option
        // list back via the "popupOptions" IPC. Reply lands in OnProcessMessage
        // which calls jfn_cef_layer_set_popup_options.
        self.send_process_message_named("getPopupOptions");
    }

    pub(crate) fn on_popup_size(self: &Arc<Self>, x: i32, y: i32, w: i32, h: i32) {
        {
            let mut p = self.popup.lock();
            p.x = x;
            p.y = y;
            p.w = w;
            p.h = h;
            p.size_received = true;
        }
        self.try_show_popup();
    }

    pub(crate) fn set_popup_options(self: &Arc<Self>, opts: Vec<String>, selected: i32) {
        {
            let mut p = self.popup.lock();
            p.options = opts;
            p.selected_idx = selected;
            p.options_received = true;
        }
        self.try_show_popup();
    }

    fn try_show_popup(self: &Arc<Self>) {
        let (x, y, w, h, opts, selected) = {
            let p = self.popup.lock();
            if !p.visible || !p.size_received || !p.options_received {
                return;
            }
            (p.x, p.y, p.w, p.h, p.options.clone(), p.selected_idx)
        };

        let surface = self.surface_ptr();
        if surface.is_null() {
            return;
        }
        let Some(p) = platform_ops::ops() else { return };

        let inner = Arc::clone(self);
        let req = platform_ops::JfnPopupRequest {
            x,
            y,
            lw: w,
            lh: h,
            options: opts,
            initial_highlight: selected,
            // Fires only on native-menu backends (macOS); compositor
            // backends (Wayland/X11/Windows) drop the closure — CEF
            // dispatches selection itself on click.
            on_selected: Some(Box::new(move |idx| {
                let mut task = DispatchPopupTask::new(inner, idx);
                let _ = post_task(ThreadId::UI, Some(&mut task));
            })),
        };
        p.popup_show(surface, req);
    }

    pub(super) fn on_deactivated(&self) {
        let was_visible = {
            let mut p = self.popup.lock();
            let was = p.visible;
            if was {
                p.visible = false;
                Self::reset_popup_state(&mut p);
            }
            was
        };
        if !was_visible {
            return;
        }
        let surface = self.surface_ptr();
        if surface.is_null() {
            return;
        }
        if let Some(p) = platform_ops::ops() {
            p.popup_hide(surface);
        }
    }

    pub(super) fn popup_rect(&self) -> (i32, i32) {
        let p = self.popup.lock();
        (p.w, p.h)
    }

    fn dispatch_popup_selection(&self, idx: i32) {
        if self.closed.load(Ordering::Acquire) {
            return;
        }
        if let Some(f) = self.focused_or_main()
            && let Some(mut msg) =
                process_message_create(Some(&CefString::from("applyPopupSelection")))
        {
            if let Some(args) = msg.argument_list() {
                args.set_int(0, idx);
            }
            f.send_process_message(
                ProcessId::from(sys::cef_process_id_t::PID_RENDERER),
                Some(&mut msg),
            );
        }
        // Only public path to CancelWidget on a CEF OSR popup is a mouse-wheel
        // event outside popup_position_ — render_widget_host_view_osr.cc:1337-1343.
        if let Some(h) = self.host() {
            let me = MouseEvent {
                x: -1,
                y: -1,
                modifiers: 0,
            };
            h.send_mouse_wheel_event(Some(&me), 0, 1);
        }
    }
}

wrap_task! {
    struct DispatchPopupTask {
        inner: Arc<Inner>,
        index: i32,
    }
    impl Task {
        fn execute(&self) {
            self.inner.dispatch_popup_selection(self.index);
        }
    }
}
