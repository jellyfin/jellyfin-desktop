use cef::*;
use std::os::raw::c_int;
use std::sync::Arc;

use crate::app::userfree_to_string;
use crate::client::Inner;

// ----- LoadHandler ---------------------------------------------------------

wrap_load_handler! {
    pub struct JfnLoadHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl LoadHandler {
        fn on_load_end(
            &self,
            _browser: Option<&mut Browser>,
            frame: Option<&mut Frame>,
            http_status_code: c_int,
        ) {
            let Some(f) = frame else { return };
            let is_main = f.is_main() == 1;
            let url = userfree_to_string(&f.url());
            self.inner.on_load_end(is_main, http_status_code, &url);
        }
        fn on_load_error(
            &self,
            _browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            error_code: Errorcode,
            error_text: Option<&CefString>,
            failed_url: Option<&CefString>,
        ) {
            let code: sys::cef_errorcode_t = error_code.into();
            let text = error_text.map(|s| s.to_string()).unwrap_or_default();
            let url = failed_url.map(|s| s.to_string()).unwrap_or_default();
            self.inner.on_load_error(code as c_int, &text, &url);
        }
    }
}
