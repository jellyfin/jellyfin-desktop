use cef::*;
use std::sync::Arc;

use crate::app::userfree_to_string;
use crate::client::Inner;

wrap_resource_request_handler! {
    pub struct JfnResourceRequestHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl ResourceRequestHandler {
        fn on_before_resource_load(
            &self,
            _browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            request: Option<&mut Request>,
            _callback: Option<&mut Callback>,
        ) -> ReturnValue {
            let Some(req) = request else { return ReturnValue::CONTINUE };

            let server_url = jfn_config::server_url();
            if server_url.is_empty() {
                return ReturnValue::CONTINUE;
            }

            let url = userfree_to_string(&req.url());
            if !url.starts_with(&server_url) {
                return ReturnValue::CONTINUE;
            }

            for (key, value) in &jfn_config::custom_headers() {
                if !key.is_empty() {
                    req.set_header_by_name(
                        Some(&CefString::from(key.as_str())),
                        Some(&CefString::from(value.as_str())),
                        1,
                    );
                }
            }

            ReturnValue::CONTINUE
        }
    }
}
