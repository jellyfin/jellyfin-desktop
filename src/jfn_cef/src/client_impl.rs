use cef::*;
use std::os::raw::c_int;
use std::sync::Arc;

use crate::client::Inner;

mod context_menu;
mod display;
mod keyboard;
mod lifespan;
mod load;
mod process_message;
mod render;
use context_menu::JfnContextMenuHandlerBuilder;
use display::JfnDisplayHandlerBuilder;
use keyboard::JfnKeyboardHandlerBuilder;
use lifespan::JfnLifeSpanHandlerBuilder;
use load::JfnLoadHandlerBuilder;
use render::JfnRenderHandlerBuilder;

pub fn make_client(inner: Arc<Inner>) -> Client {
    JfnClientBuilder::new(inner)
}

wrap_client! {
    pub struct JfnClientBuilder {
        inner: Arc<Inner>,
    }

    impl Client {
        fn render_handler(&self) -> Option<RenderHandler> {
            Some(JfnRenderHandlerBuilder::new(self.inner.clone()))
        }
        fn life_span_handler(&self) -> Option<LifeSpanHandler> {
            Some(JfnLifeSpanHandlerBuilder::new(self.inner.clone()))
        }
        fn load_handler(&self) -> Option<LoadHandler> {
            Some(JfnLoadHandlerBuilder::new(self.inner.clone()))
        }
        fn context_menu_handler(&self) -> Option<ContextMenuHandler> {
            Some(JfnContextMenuHandlerBuilder::new(self.inner.clone()))
        }
        fn display_handler(&self) -> Option<DisplayHandler> {
            Some(JfnDisplayHandlerBuilder::new(self.inner.clone()))
        }
        fn keyboard_handler(&self) -> Option<KeyboardHandler> {
            Some(JfnKeyboardHandlerBuilder::new(self.inner.clone()))
        }
        fn on_process_message_received(
            &self,
            browser: Option<&mut Browser>,
            _frame: Option<&mut Frame>,
            _source_process: ProcessId,
            message: Option<&mut ProcessMessage>,
        ) -> c_int {
            process_message::on_process_message_received(&self.inner, browser, message)
        }
    }
}
