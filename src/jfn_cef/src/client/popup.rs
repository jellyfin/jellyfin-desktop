use cef::rc::Rc;
use cef::{
    ImplBrowserHost, ImplTask, KeyEvent, Task, ThreadId, WrapTask, post_task, sys, wrap_task,
};
use std::sync::Arc;
use std::sync::atomic::Ordering;

use crate::platform_ops;

use super::{Inner, PopupState};

// Windows virtual-key codes CEF expects in KeyEvent::windows_key_code.
const VK_RETURN: i32 = 0x0D;
const VK_ESCAPE: i32 = 0x1B;
const VK_UP: i32 = 0x26;
const VK_DOWN: i32 = 0x28;

impl Inner {
    fn reset_popup_state(p: &mut PopupState) {
        p.size_received = false;
        p.options_received = false;
        p.options.clear();
        p.selected_idx = -1;
        p.selectable.clear();
        p.anchor = None;
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

    pub(crate) fn set_popup_options(
        self: &Arc<Self>,
        opts: Vec<String>,
        selected: i32,
        selectable: Vec<i32>,
        anchor: Option<(i32, i32)>,
    ) {
        {
            let mut p = self.popup.lock();
            p.options = opts;
            p.selected_idx = selected;
            p.selectable = selectable;
            p.anchor = anchor;
            p.options_received = true;
        }
        self.try_show_popup();
    }

    fn try_show_popup(self: &Arc<Self>) {
        let (x, y, w, h, opts, selected, selectable) = {
            let p = self.popup.lock();
            if !p.visible || !p.size_received || !p.options_received {
                return;
            }
            // Blink's popup rect (p.x/p.y) flips above the element near the
            // window bottom; the anchor keeps the menu under the box.
            let (x, y) = p.anchor.unwrap_or((p.x, p.y));
            (
                x,
                y,
                p.w,
                p.h,
                p.options.clone(),
                p.selected_idx,
                p.selectable.clone(),
            )
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
            // Native-menu backends (macOS, Wayland) fire this with the chosen
            // option index (or -1 to cancel); X11/Windows drop it and let CEF
            // dispatch selection itself.
            on_selected: Some(Box::new(move |idx| {
                let mut task = DispatchPopupTask::new(inner, idx, selected, selectable.clone());
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

    // CEF OSR has no "set selected index" API for <select>: the popup is a real
    // RenderWidget that must be driven by forwarded input so Blink commits and
    // closes it cleanly (which is what lets it reopen). We render the menu
    // ourselves, then replay the user's pick into CEF's still-open popup —
    // arrow-key to the chosen row + Enter to commit, or Escape to cancel.
    fn dispatch_popup_selection(&self, idx: i32, current: i32, selectable: &[i32]) {
        if self.closed.load(Ordering::Acquire) {
            return;
        }
        let Some(host) = self.host() else {
            return;
        };

        let send_key = |code: i32| {
            let down = KeyEvent {
                type_: sys::cef_key_event_type_t::KEYEVENT_RAWKEYDOWN.into(),
                windows_key_code: code,
                native_key_code: code,
                ..KeyEvent::default()
            };
            let up = KeyEvent {
                type_: sys::cef_key_event_type_t::KEYEVENT_KEYUP.into(),
                windows_key_code: code,
                native_key_code: code,
                ..KeyEvent::default()
            };
            host.send_key_event(Some(&down));
            host.send_key_event(Some(&up));
        };

        if idx < 0 {
            send_key(VK_ESCAPE);
            return;
        }

        // Arrow stepping is in selectable-option space (Blink skips disabled
        // rows), so map both the popup's current highlight and the target into
        // that space and step by the difference.
        let pos = |opt: i32| selectable.iter().position(|&v| v == opt);
        let from = pos(current).unwrap_or(0) as i32;
        let Some(to) = pos(idx) else {
            // Target isn't selectable (shouldn't happen) — just cancel cleanly.
            send_key(VK_ESCAPE);
            return;
        };
        let delta = to as i32 - from;
        let step = if delta >= 0 { VK_DOWN } else { VK_UP };
        for _ in 0..delta.abs() {
            send_key(step);
        }
        send_key(VK_RETURN);
    }
}

wrap_task! {
    struct DispatchPopupTask {
        inner: Arc<Inner>,
        index: i32,
        current: i32,
        selectable: Vec<i32>,
    }
    impl Task {
        fn execute(&self) {
            self.inner.dispatch_popup_selection(
                self.index,
                self.current,
                &self.selectable,
            );
        }
    }
}
