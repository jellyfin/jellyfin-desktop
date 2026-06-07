use cef::rc::Rc;
use cef::{ImplTask, Task, ThreadId, WrapTask, post_delayed_task, post_task, wrap_task};
use parking_lot::Mutex;
use std::sync::Arc;
use std::sync::mpsc::SyncSender;

use super::Inner;
use jfn_playback::shutdown::jfn_shutting_down;

wrap_task! {
    struct ApplyResizeTask {
        inner: Arc<Inner>,
    }
    impl Task {
        fn execute(&self) {
            self.inner.apply_pending_resize();
        }
    }
}

pub(super) fn post_apply_resize(inner: Arc<Inner>, delay_ms: i64) {
    let mut task = ApplyResizeTask::new(inner);
    let _ = post_delayed_task(ThreadId::UI, Some(&mut task), delay_ms);
}

wrap_task! {
    struct SetRefreshTask {
        inner: Arc<Inner>,
        target: i32,
    }
    impl Task {
        fn execute(&self) {
            self.inner.apply_set_refresh(self.target);
        }
    }
}

pub(super) fn post_set_refresh(inner: Arc<Inner>, target: i32) {
    let mut task = SetRefreshTask::new(inner, target);
    let _ = post_task(ThreadId::UI, Some(&mut task));
}

wrap_task! {
    struct ResetCreateTask {
        inner: Arc<Inner>,
    }
    impl Task {
        fn execute(&self) {
            // CefShutdown drains pending tasks; creating a browser here would
            // race with the shutdown teardown and cause a hang.
            if jfn_shutting_down() {
                return;
            }
            self.inner.create("");
        }
    }
}

pub(super) fn post_reset_create(inner: Arc<Inner>) {
    let mut task = ResetCreateTask::new(inner);
    let _ = post_task(ThreadId::UI, Some(&mut task));
}

wrap_task! {
    struct PasteJsTask {
        inner: Arc<Inner>,
        text: String,
    }
    impl Task {
        fn execute(&self) {
            let escaped = serde_json::to_string(&self.text).unwrap_or_else(|_| "\"\"".to_string());
            let js = format!("document.execCommand('insertText',false,{});", escaped);
            self.inner.exec_js_focused(&js);
        }
    }
}

pub(super) fn post_paste_js(inner: Arc<Inner>, text: String) {
    let mut task = PasteJsTask::new(inner, text);
    let _ = post_task(ThreadId::UI, Some(&mut task));
}

type CloseCollectTx = Arc<Mutex<Option<SyncSender<Vec<Arc<Inner>>>>>>;

wrap_task! {
    struct CloseAndCollectTask {
        tx: CloseCollectTx,
    }
    impl Task {
        fn execute(&self) {
            // Single snapshot: take Arc<Inner> + force-close every layer
            // under one INSTANCE lock, on TID_UI. Holding the lock across
            // close_browser_force is safe — that call only schedules close;
            // OnBeforeClose → jfn_browsers_remove fires later on TID_UI
            // after this task unwinds, so no reentrant INSTANCE.lock().
            let inners = crate::browsers::jfn_browsers_close_and_snapshot();
            if let Some(tx) = self.tx.lock().take() {
                let _ = tx.send(inners);
            }
        }
    }
}

/// Post a single-snapshot close-and-collect onto TID_UI. The posted task
/// closes every layer's browser and ships the corresponding `Arc<Inner>`
/// list back via `tx`, so the caller can wait on exactly the set that was
/// closed (no second-snapshot race). Asserts the post: this site is
/// load-bearing for shutdown — silent post loss = process hang.
pub(crate) fn jfn_cef_post_close_and_collect(tx: SyncSender<Vec<Arc<Inner>>>) {
    let mut task = CloseAndCollectTask::new(Arc::new(Mutex::new(Some(tx))));
    assert!(
        post_task(ThreadId::UI, Some(&mut task)) != 0,
        "TID_UI post during shutdown — CEF UI thread invariant broken"
    );
}

wrap_task! {
    struct SetHiddenAllTask {
        hidden: bool,
    }
    impl Task {
        fn execute(&self) {
            crate::browsers::jfn_browsers_apply_hidden_all(self.hidden);
        }
    }
}

/// Post a task onto TID_UI that calls `WasHidden(hidden)` on every live
/// layer's BrowserHost. Caller-agnostic — any thread can request a
/// visibility change; CEF requires the call on TID_UI.
pub(crate) fn jfn_cef_post_set_hidden_all(hidden: bool) {
    let mut task = SetHiddenAllTask::new(hidden);
    let _ = post_task(ThreadId::UI, Some(&mut task));
}
