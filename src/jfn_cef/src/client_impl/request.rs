use cef::*;
use std::sync::Arc;

use crate::client::Inner;

use super::resource_request::JfnResourceRequestHandlerBuilder;

wrap_request_handler! {
    pub struct JfnRequestHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl RequestHandler {
        fn resource_request_handler(
            &self,
            _browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            _request: Option<&mut Request>,
            _is_navigation: ::std::os::raw::c_int,
            _is_download: ::std::os::raw::c_int,
            _request_initiator: Option<&CefString>,
            _disable_default_handling: Option<&mut ::std::os::raw::c_int>,
        ) -> Option<ResourceRequestHandler> {
            Some(JfnResourceRequestHandlerBuilder::new(self.inner.clone()))
        }
    }
}
