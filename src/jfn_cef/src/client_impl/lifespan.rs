use cef::*;
use std::os::raw::c_int;
use std::sync::Arc;

use crate::client::Inner;

wrap_life_span_handler! {
    pub struct JfnLifeSpanHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl LifeSpanHandler {
        fn on_after_created(&self, browser: Option<&mut Browser>) {
            let Some(b) = browser else { return };
            self.inner.handle_on_after_created(b.clone());
        }
        fn on_before_close(&self, _browser: Option<&mut Browser>) {
            self.inner.handle_on_before_close();
        }
        fn on_before_popup(
            &self,
            _browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            _popup_id: c_int,
            target_url: Option<&CefString>,
            _target_frame_name: Option<&CefString>,
            _target_disposition: WindowOpenDisposition,
            _user_gesture: c_int,
            _popup_features: Option<&PopupFeatures>,
            _window_info: Option<&mut WindowInfo>,
            _client: Option<&mut Option<Client>>,
            _settings: Option<&mut BrowserSettings>,
            _extra_info: Option<&mut Option<DictionaryValue>>,
            _no_javascript_access: Option<&mut c_int>,
        ) -> c_int {
            let url = target_url.map(|s| s.to_string()).unwrap_or_default();
            if self.inner.on_before_popup(&url) { 1 } else { 0 }
        }
    }
}
