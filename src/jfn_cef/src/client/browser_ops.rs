use cef::{
    Browser, BrowserHost, BrowserSettings, CefString, Frame, ImplBrowser, ImplBrowserHost,
    ImplFrame, PaintElementType, ProcessId, WindowInfo, browser_host_create_browser,
    process_message_create, sys,
};
use std::sync::Arc;
use std::sync::atomic::Ordering;

use crate::paint_scheduler::PaintMode;

use super::{DEFAULT_FRAME_RATE, Inner, PAINT_MODE};

impl Inner {
    pub(super) fn browser_clone(&self) -> Option<Browser> {
        self.browser.lock().clone()
    }

    pub(super) fn host(&self) -> Option<BrowserHost> {
        self.browser_clone().and_then(|b| b.host())
    }

    pub(crate) fn close_browser_force(&self) {
        if let Some(host) = self.host() {
            host.close_browser(1);
        }
    }

    pub(super) fn focused_or_main(&self) -> Option<Frame> {
        let b = self.browser_clone()?;
        b.focused_frame().or_else(|| b.main_frame())
    }

    pub(super) fn notify_screen_info_changed(&self) {
        if let Some(h) = self.host() {
            h.notify_screen_info_changed();
        }
    }

    pub(super) fn cef_was_resized(&self) {
        if let Some(h) = self.host() {
            h.was_resized();
        }
    }

    pub(crate) fn invalidate_view(&self) {
        if let Some(h) = self.host() {
            h.invalidate(PaintElementType::VIEW);
        }
    }

    #[cfg(target_os = "macos")]
    pub(crate) fn send_external_begin_frame(&self) {
        if let Some(h) = self.host() {
            h.send_external_begin_frame();
        }
    }

    pub(super) fn cef_set_windowless_frame_rate(&self, hz: i32) {
        if let Some(h) = self.host() {
            h.set_windowless_frame_rate(hz);
        }
    }

    pub(crate) fn cef_was_hidden(&self, hidden: bool) {
        if let Some(h) = self.host() {
            h.was_hidden(if hidden { 1 } else { 0 });
        }
    }

    pub(crate) fn exec_js(&self, js: &str) {
        let Some(b) = self.browser_clone() else {
            return;
        };
        let Some(f) = b.main_frame() else { return };
        let code = CefString::from(js);
        f.execute_java_script(Some(&code), Some(&CefString::from("")), 0);
    }

    pub(super) fn send_process_message_named(&self, name: &str) {
        let Some(f) = self.focused_or_main() else {
            return;
        };
        let Some(mut msg) = process_message_create(Some(&CefString::from(name))) else {
            return;
        };
        f.send_process_message(
            ProcessId::from(sys::cef_process_id_t::PID_RENDERER),
            Some(&mut msg),
        );
    }

    pub(super) fn cef_create_browser(self: &Arc<Self>, url: &str) {
        let shared = PAINT_MODE
            .get_or_init(|| PaintMode::new(false))
            .shared_textures();
        let parent: sys::cef_window_handle_t = unsafe { std::mem::zeroed() };
        let mut wi = WindowInfo::default().set_as_windowless(parent);
        wi.shared_texture_enabled = if shared { 1 } else { 0 };
        #[cfg(target_os = "macos")]
        {
            wi.external_begin_frame_enabled = 1;
        }
        #[cfg(not(target_os = "macos"))]
        {
            wi.external_begin_frame_enabled = 0;
        }

        let fr_layer = self.frame_rate.load(Ordering::Acquire);
        let fr_default = DEFAULT_FRAME_RATE.load(Ordering::Acquire);
        let fr = if fr_layer > 0 { fr_layer } else { fr_default };
        let bs = BrowserSettings {
            background_color: 0,
            windowless_frame_rate: if fr > 0 { fr } else { 60 },
            ..BrowserSettings::default()
        };

        let kind = self.injection_kind.lock().clone();
        let add_ctx_menu = self.context_menu_builder.lock().is_some();
        let extra = crate::injection::build_for_kind(
            &kind,
            add_ctx_menu,
            shared,
            self.dropdown,
            self.context_menu,
        );

        let mut client = crate::client_impl::make_client(Arc::clone(self));
        let url_cef = CefString::from(url);
        let mut extra_opt = extra.and_then(crate::injection::ExtraInfo::into_dictionary);
        let _ = browser_host_create_browser(
            Some(&wi),
            Some(&mut client),
            Some(&url_cef),
            Some(&bs),
            extra_opt.as_mut(),
            None,
        );
    }

    pub(super) fn cef_close_browser(&self) {
        if let Some(h) = self.host() {
            h.close_browser(1);
        }
    }

    pub(super) fn cef_load_url(&self, url: &str) {
        let Some(b) = self.browser_clone() else {
            return;
        };
        let Some(f) = b.main_frame() else { return };
        f.load_url(Some(&CefString::from(url)));
    }

    pub(super) fn exec_js_focused(&self, js: &str) {
        let Some(f) = self.focused_or_main() else {
            return;
        };
        let code = CefString::from(js);
        let url_uf = f.url();
        let url = CefString::from(&url_uf);
        f.execute_java_script(Some(&code), Some(&url), 0);
    }

    pub(super) fn frame_paste(&self) {
        if let Some(f) = self.focused_or_main() {
            f.paste();
        }
    }

    pub(super) fn frame_undo(&self) {
        if let Some(f) = self.focused_or_main() {
            f.undo();
        }
    }

    pub(super) fn frame_redo(&self) {
        if let Some(f) = self.focused_or_main() {
            f.redo();
        }
    }

    pub(super) fn frame_cut(&self) {
        if let Some(f) = self.focused_or_main() {
            f.cut();
        }
    }

    pub(super) fn frame_copy(&self) {
        if let Some(f) = self.focused_or_main() {
            f.copy();
        }
    }

    pub(super) fn frame_select_all(&self) {
        if let Some(f) = self.focused_or_main() {
            f.select_all();
        }
    }

    pub(crate) fn browser_alive(&self) -> bool {
        self.browser.lock().is_some() && !self.closed.load(Ordering::Acquire)
    }
}
