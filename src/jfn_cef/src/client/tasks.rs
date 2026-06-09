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
            // Creating a browser during shutdown races CefShutdown teardown
            // and hangs.
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
            let inners = crate::browsers::jfn_browsers_close_and_snapshot();
            if let Some(tx) = self.tx.lock().take() {
                let _ = tx.send(inners);
            }
        }
    }
}

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

pub(crate) fn jfn_cef_post_set_hidden_all(hidden: bool) {
    let mut task = SetHiddenAllTask::new(hidden);
    let _ = post_task(ThreadId::UI, Some(&mut task));
}
