//! `NativeV8Handler`: generic IPC relay from page JS to the browser process.

use cef::*;

#[derive(Clone)]
pub(crate) struct NativeHandler;

wrap_v8_handler! {
    pub(crate) struct NativeHandlerBuilder { inner: NativeHandler, }

    impl V8Handler {
        fn execute(
            &self,
            name: Option<&CefString>,
            _object: Option<&mut V8Value>,
            arguments: Option<&[Option<V8Value>]>,
            _retval: Option<&mut Option<V8Value>>,
            _exception: Option<&mut CefString>,
        ) -> ::std::os::raw::c_int {
            let Some(msg_name) = name else { return 0 };
            let Some(mut msg) = process_message_create(Some(msg_name)) else { return 0 };

            if let Some(args_list) = msg.argument_list()
                && let Some(args) = arguments {
                    for (i, v) in args.iter().enumerate() {
                        let Some(v) = v else { continue };
                        if v.is_bool() == 1 {
                            args_list.set_bool(i, v.bool_value());
                        } else if v.is_int() == 1 {
                            args_list.set_int(i, v.int_value());
                        } else if v.is_double() == 1 {
                            args_list.set_double(i, v.double_value());
                        } else if v.is_string() == 1 {
                            let s = crate::app::userfree_to_string(&v.string_value());
                            let cs = CefString::from(s.as_str());
                            args_list.set_string(i, Some(&cs));
                        }
                    }
                }

            // Use current V8 context's frame so the message is associated
            // with the frame that executed the JS call.
            let Some(ctx) = v8_context_get_current_context() else { return 0 };
            let Some(frame) = ctx.frame() else { return 0 };
            frame.send_process_message(ProcessId::from(sys::cef_process_id_t::PID_BROWSER), Some(&mut msg));
            1
        }
    }
}
