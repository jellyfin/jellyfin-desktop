// JfnCefLayer is an opaque internal handle; callers within this crate
// pass it back unchanged. Marking each consumer unsafe would cascade
// without adding type safety, so the lint is suppressed module-wide.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

//! OverlayBrowser business logic.
//!
//! The server-selection overlay loads `app://resources/overlay.html` over the
//! main browser, drives a two-phase HEAD→GET probe of user-entered server
//! URLs, and either hands input back to the main browser (dismiss) or kicks
//! the main browser into a fresh load (navigate). One process-wide instance
//! held in [`INSTANCE`]; lifetime mirrors the main browser.

use cef::*;
use parking_lot::Mutex;
use std::os::raw::c_void;
use std::sync::Arc;

use crate::browsers::{jfn_browsers_create, jfn_browsers_set_active};
use crate::business_common::{apply_setting_value, reject_double_init};
use crate::client::{
    Inner, JfnCefLayer, jfn_cef_layer_create, jfn_cef_layer_inner, jfn_cef_layer_set_name,
    jfn_cef_layer_set_visible,
};
use crate::ipc::{BrowserMessage, list_opt_string, list_string, send_to_renderer};
use jfn_color::theme::jfn_theme_color_on_overlay_dismissed;
use jfn_jellyfin::{extract_base_url, is_valid_public_info, normalize_input};

struct OverlayState {
    main_layer: Arc<Inner>,
    active_probe: Option<Urlrequest>,
}

static INSTANCE: Mutex<Option<OverlayState>> = Mutex::new(None);

/// Create the overlay layer over `main_layer`, install handlers, load the
/// overlay URL. Called once after the main browser is created.
pub fn jfn_overlay_init(main_layer: *mut JfnCefLayer) {
    if main_layer.is_null() {
        return;
    }
    if reject_double_init(&INSTANCE.lock(), "jfn_overlay_init") {
        return;
    }

    let kind = c"overlay";
    let layer = unsafe { jfn_browsers_create(kind.as_ptr()) };
    if layer.is_null() {
        return;
    }

    let name = c"overlay";
    unsafe { jfn_cef_layer_set_name(layer, name.as_ptr()) };

    let inner = unsafe { jfn_cef_layer_inner(layer) };
    install_handlers(layer, Arc::clone(&inner));

    unsafe {
        jfn_cef_layer_set_visible(layer, true);
        let url = "app://resources/overlay.html";
        jfn_cef_layer_create(layer, url.as_ptr() as *const _, url.len());
    }

    let main_inner = unsafe { jfn_cef_layer_inner(main_layer) };
    *INSTANCE.lock() = Some(OverlayState {
        main_layer: main_inner,
        active_probe: None,
    });
}

fn install_handlers(layer: *mut JfnCefLayer, inner_for_created: Arc<Inner>) {
    let l = unsafe { &*layer };

    // Created → overlay wins input.
    l.set_created_callback_rust(Some(Box::new(move |_b: *mut c_void| {
        let p = inner_for_created.layer_ptr();
        if !p.is_null() {
            jfn_browsers_set_active(p);
        }
    })));

    l.set_message_handler_rust(Some(Box::new(handle_message)));

    // BeforeClose: clear INSTANCE so post-close cancel/IPC paths no-op
    // instead of touching a torn-down main browser handle.
    l.set_before_close_callback_rust(Some(Box::new(|| {
        cancel_active_probe();
        *INSTANCE.lock() = None;
    })));

    l.set_context_menu_builder_rust(Some(crate::app_menu::build_closure()));
    l.set_context_menu_dispatcher_rust(Some(crate::app_menu::dispatch_closure()));
}

fn main_layer_arc() -> Option<Arc<Inner>> {
    INSTANCE.lock().as_ref().map(|s| Arc::clone(&s.main_layer))
}

fn handle_message(message: BrowserMessage) -> bool {
    let args = message.args();

    match message.name() {
        "getSavedServerUrl" => {
            let Some(frame) = message.main_frame() else {
                return true;
            };
            let url = jfn_config::server_url();
            send_to_renderer(&frame, "savedServerUrl", |args| {
                args.set_string(0, Some(&CefString::from(url.as_str())));
            });
            true
        }
        "navigateMain" => {
            let Some(args) = args else { return true };
            let url = list_string(args, 0);
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_INFO,
                &format!("Overlay: navigateMain {url}"),
            );
            jfn_config::set_server_url(&url);
            jfn_config::settings_save_async();
            if let Some(ml) = main_layer_arc() {
                ml.load_url(&url);
            }
            true
        }
        "dismissOverlay" => {
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_INFO,
                "Overlay: dismissOverlay",
            );
            let Some(ml) = main_layer_arc() else {
                return true;
            };
            let p = ml.layer_ptr();
            if !p.is_null() {
                jfn_browsers_set_active(p);
            }
            jfn_theme_color_on_overlay_dismissed();
            true
        }
        "saveServerUrl" => {
            let Some(args) = args else { return true };
            let url = list_string(args, 0);
            jfn_config::set_server_url(&url);
            jfn_config::settings_save_async();
            true
        }
        "setSettingValue" => {
            let Some(args) = args else { return true };
            let section = list_string(args, 0);
            let key = list_string(args, 1);
            let value = list_opt_string(args, 2);
            apply_setting_value(&section, &key, value.as_deref());
            true
        }
        "checkServerConnectivity" => {
            let Some(args) = args else { return true };
            let Some(b) = message.browser().cloned() else {
                return true;
            };
            let url = list_string(args, 0);
            cancel_active_probe();
            let normalized = normalize_input(&url);
            start_probe(b, url, normalized);
            true
        }
        "cancelServerConnectivity" => {
            cancel_active_probe();
            // Kill the pre-load.
            if let Some(ml) = main_layer_arc() {
                ml.reset();
            }
            true
        }
        _ => false,
    }
}

fn cancel_active_probe() {
    let probe = INSTANCE.lock().as_mut().and_then(|s| s.active_probe.take());
    if let Some(p) = probe {
        p.cancel();
    }
}

fn start_probe(browser: Browser, user_url: String, normalized: String) {
    let probe = ServerProbeClient::new(
        normalized,
        Box::new(move |success, base_url| {
            let Some(frame) = browser.main_frame() else {
                return;
            };
            let reply_url = if success {
                base_url.clone()
            } else {
                user_url.clone()
            };
            send_to_renderer(&frame, "serverConnectivityResult", |args| {
                args.set_string(0, Some(&CefString::from(user_url.as_str())));
                args.set_bool(1, if success { 1 } else { 0 });
                args.set_string(2, Some(&CefString::from(reply_url.as_str())));
            });
            // Clear the slot under lock so cancel() after completion is a no-op.
            if let Some(s) = INSTANCE.lock().as_mut() {
                s.active_probe = None;
            }
        }),
    );
    probe.start();
}

// ---- ServerProbeClient ----------------------------------------------------
//
// HEAD with redirect-follow to find the canonical base URL, then GET
// {base}/System/Info/Public to confirm it's a Jellyfin server. Cancellable:
// .cancel() aborts the active CefURLRequest; a late OnRequestComplete with
// the slot cleared is harmless.

type ProbeCallback = Box<dyn FnMut(bool, String) + Send + Sync>;

struct ProbeState {
    url: String,
    phase: Phase,
    base: String,
    body: Vec<u8>,
    callback: Option<ProbeCallback>,
}

#[derive(Copy, Clone, PartialEq)]
enum Phase {
    Head,
    Get,
}

struct ServerProbeClient {
    state: Arc<Mutex<ProbeState>>,
}

impl ServerProbeClient {
    fn new(url: String, callback: ProbeCallback) -> Self {
        Self {
            state: Arc::new(Mutex::new(ProbeState {
                url,
                phase: Phase::Head,
                base: String::new(),
                body: Vec::new(),
                callback: Some(callback),
            })),
        }
    }

    fn start(&self) {
        let url = self.state.lock().url.clone();
        let request = make_request("HEAD", &url, self.client());
        if let Some(r) = request
            && let Some(s) = INSTANCE.lock().as_mut()
        {
            s.active_probe = Some(r);
        }
    }

    fn client(&self) -> UrlrequestClient {
        JfnServerProbeClient::new(Arc::clone(&self.state))
    }
}

fn on_complete(state: &Arc<Mutex<ProbeState>>, request: &Urlrequest) {
    // HEAD phase: extract resolved base URL, post GET on /System/Info/Public.
    let next_request = {
        let mut st = state.lock();
        if st.phase == Phase::Head {
            let mut resolved = st.url.clone();
            if let Some(resp) = request.response() {
                let url_uf = resp.url();
                let cs: CefString = (&url_uf).into();
                let s = cs.to_string();
                if !s.is_empty() {
                    resolved = s;
                }
            }
            st.base = extract_base_url(&resolved);
            st.phase = Phase::Get;
            let next_url = format!("{}/System/Info/Public", st.base);
            let client = JfnServerProbeClient::new(Arc::clone(state));
            make_request("GET", &next_url, client)
        } else {
            None
        }
    };
    if next_request.is_some() {
        return;
    }

    // GET phase complete: validate body, then invoke caller.
    let (success, base, cb) = {
        let mut st = state.lock();
        let mut ok = false;
        let status = request.request_status();
        if status.as_ref() == &sys::cef_urlrequest_status_t::UR_SUCCESS
            && let Some(resp) = request.response()
            && resp.status() == 200
        {
            ok = is_valid_public_info(&st.body);
        }
        let base = st.base.clone();
        let cb = st.callback.take();
        (ok, base, cb)
    };
    if let Some(mut f) = cb {
        f(success, base);
    }
}

fn make_request(method: &str, url: &str, client: UrlrequestClient) -> Option<Urlrequest> {
    let req: Request = request_create()?;
    req.set_url(Some(&CefString::from(url)));
    req.set_method(Some(&CefString::from(method)));
    let mut req_arg = req;
    let mut client_arg = client;
    urlrequest_create(Some(&mut req_arg), Some(&mut client_arg), None)
}

cef::wrap_urlrequest_client! {
    struct JfnServerProbeClient {
        state: Arc<Mutex<ProbeState>>,
    }

    impl UrlrequestClient {
        fn on_request_complete(&self, request: Option<&mut Urlrequest>) {
            let Some(req) = request else { return };
            on_complete(&self.state, req);
        }
        fn on_download_data(
            &self,
            _request: Option<&mut Urlrequest>,
            data: *const u8,
            data_length: usize,
        ) {
            let mut st = self.state.lock();
            if st.phase == Phase::Get && !data.is_null() && data_length > 0 {
                let slice = unsafe { std::slice::from_raw_parts(data, data_length) };
                st.body.extend_from_slice(slice);
            }
        }
    }
}
