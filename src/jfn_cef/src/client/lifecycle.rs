use cef::rc::Rc;
use cef::{Browser, CefString, ImplBrowser, ImplBrowserHost, ImplFrame};
use std::os::raw::c_void;
use std::sync::Arc;
use std::sync::atomic::Ordering;

use jfn_playback::shutdown::jfn_shutting_down;

use super::{Inner, STATE_NORMAL, STATE_PENDING_RESET, STATE_RECREATING, platform_ops, tasks};

impl Inner {
    fn on_after_created(&self) -> i32 {
        self.has_browser.store(true, Ordering::Release);
        match self.state.load(Ordering::Acquire) {
            STATE_PENDING_RESET => {
                self.state.store(STATE_RECREATING, Ordering::Release);
                1
            }
            STATE_RECREATING => {
                self.state.store(STATE_NORMAL, Ordering::Release);
                0
            }
            _ => 0,
        }
    }

    fn on_before_close(self: &Arc<Self>) {
        self.has_browser.store(false, Ordering::Release);
        if self.pending_internal_reset.swap(false, Ordering::AcqRel) {
            tasks::post_reset_create(Arc::clone(self));
        }
    }

    pub(crate) fn create(self: &Arc<Self>, url: &str) {
        self.cef_create_browser(url);
    }

    pub(crate) fn reset(&self) {
        if self.state.load(Ordering::Acquire) != STATE_NORMAL {
            return;
        }
        // Create must be deferred to OnBeforeClose: creating inline reenters
        // CEF while WebContents is mid-destroy and crashes inside libcef.
        self.pending_internal_reset.store(true, Ordering::Release);
        if self.has_browser.load(Ordering::Acquire) {
            self.state.store(STATE_RECREATING, Ordering::Release);
            self.cef_close_browser();
        } else {
            self.state.store(STATE_PENDING_RESET, Ordering::Release);
        }
    }

    pub(crate) fn load_url(&self, url: &str) {
        if self.state.load(Ordering::Acquire) != STATE_NORMAL
            || !self.has_browser.load(Ordering::Acquire)
        {
            *self.pending_url.lock() = url.to_string();
            return;
        }
        self.cef_load_url(url);
    }

    fn take_pending_url(&self) -> Option<String> {
        let mut g = self.pending_url.lock();
        if g.is_empty() {
            None
        } else {
            Some(std::mem::take(&mut *g))
        }
    }

    pub(crate) fn handle_on_after_created(self: &Arc<Self>, browser: Browser) {
        let formatted = format!("CefLayer::OnAfterCreated name={}", self.name_str());
        jfn_logging::log(
            jfn_logging::CATEGORY_CEF,
            jfn_logging::LEVEL_DEBUG,
            &formatted,
        );
        *self.browser.lock() = Some(browser.clone());
        {
            let _g = self.close_mtx.lock();
            self.closed.store(false, Ordering::Release);
            self.close_cv.notify_all();
        }
        {
            let _g = self.load_mtx.lock();
            self.loaded.store(false, Ordering::Release);
            self.load_cv.notify_all();
        }
        if jfn_shutting_down() {
            if let Some(h) = browser.host() {
                h.close_browser(1);
            }
            return;
        }
        self.paint_scheduler.during_resize(self, || {
            if let Some(h) = browser.host() {
                h.notify_screen_info_changed();
                h.was_resized();
            }
        });

        let action = self.on_after_created();
        if action == 1 {
            if let Some(h) = browser.host() {
                h.close_browser(1);
            }
            return;
        }

        let g = self.created_callback.lock();
        if let Some(f) = g.as_ref() {
            unsafe {
                browser.add_ref();
                let raw = ImplBrowser::get_raw(&browser) as *mut c_void;
                f(raw);
            }
        }
        drop(g);

        if let Some(url) = self.take_pending_url()
            && let Some(f) = browser.main_frame()
        {
            f.load_url(Some(&CefString::from(url.as_str())));
        }
    }

    pub(crate) fn wait_for_close(&self) {
        let mut g = self.close_mtx.lock();
        while !self.closed.load(Ordering::Acquire) {
            self.close_cv.wait(&mut g);
        }
    }

    pub(crate) fn handle_on_before_close(self: &Arc<Self>) {
        *self.browser.lock() = None;
        self.paint_scheduler.before_close();
        {
            let _g = self.close_mtx.lock();
            self.closed.store(true, Ordering::Release);
            self.close_cv.notify_all();
        }
        {
            let _g = self.load_mtx.lock();
            self.loaded.store(true, Ordering::Release);
            self.load_cv.notify_all();
        }
        self.on_before_close();
        // Remove from the registry BEFORE the before_close_callback: the
        // callback may clear an open-status flag and a racing re-open would
        // otherwise push a second layer onto active_stack while this one is
        // still mid-teardown.
        let lp = self.layer_ptr.swap(std::ptr::null_mut(), Ordering::AcqRel);
        if !lp.is_null() {
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_DEBUG,
                &format!(
                    "CefLayer::OnBeforeClose name={} -> auto-remove",
                    self.name_str()
                ),
            );
            crate::browsers::jfn_browsers_remove(lp);
        }
        // Take before invoking: the callback may install a new slot, which
        // deadlocks if the lock is still held across the call.
        let slot = self.before_close_callback.lock().take();
        if let Some(f) = slot {
            f();
        }
    }

    pub(crate) fn on_before_popup(&self, url: &str) -> bool {
        // Leading '-' guard blocks argv-style option smuggling into xdg-open.
        if url.is_empty() || url.starts_with('-') {
            return true;
        }
        if let Some(p) = platform_ops::ops() {
            p.open_external_url(url);
        }
        true
    }
}
