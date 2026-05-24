//! OverlayBrowser business logic.
//!
//! The server-selection overlay loads `app://resources/overlay.html` over the
//! main browser, drives a two-phase HEAD→GET probe of user-entered server
//! URLs, and either hands input back to the main browser (dismiss) or kicks
//! the main browser into a fresh load (navigate). One process-wide instance
//! held in [`INSTANCE`]; lifetime mirrors the main browser.

use cef::rc::ConvertReturnValue;
use cef::*;
use std::ffi::CString;
use std::os::raw::c_void;
use parking_lot::Mutex;

use crate::client::JfnCefLayer;

const OVERLAY_FADE_DURATION_SEC: f32 = 0.25;

use crate::browsers::{jfn_browsers_create, jfn_browsers_set_active};
use crate::client::{
    jfn_cef_layer_create, jfn_cef_layer_fade, jfn_cef_layer_load_url, jfn_cef_layer_reset,
    jfn_cef_layer_set_name, jfn_cef_layer_set_visible,
};
use jfn_color::theme::jfn_theme_color_on_overlay_dismissed;
use jfn_jellyfin::{extract_base_url, is_valid_public_info, normalize_input};

struct OverlayState {
    layer: *mut JfnCefLayer,
    main_layer: *mut JfnCefLayer,
    active_probe: Option<Urlrequest>,
}

unsafe impl Send for OverlayState {}

static INSTANCE: Mutex<Option<OverlayState>> = Mutex::new(None);

#[derive(Clone, Copy)]
struct LayerPtr(*mut JfnCefLayer);
unsafe impl Send for LayerPtr {}
unsafe impl Sync for LayerPtr {}

/// Create the overlay layer over `main_layer`, install handlers, load the
/// overlay URL. Called once after the main browser is created.
pub fn jfn_overlay_init(main_layer: *mut JfnCefLayer) {
    let kind = CString::new("overlay").unwrap();
    let layer = unsafe { jfn_browsers_create(kind.as_ptr()) };
    if layer.is_null() {
        return;
    }

    let name = CString::new("overlay").unwrap();
    unsafe { jfn_cef_layer_set_name(layer, name.as_ptr()) };

    install_handlers(layer, main_layer);

    unsafe {
        jfn_cef_layer_set_visible(layer, true);
        let url = "app://resources/overlay.html";
        jfn_cef_layer_create(layer, url.as_ptr() as *const _, url.len());
    }

    *INSTANCE.lock() = Some(OverlayState {
        layer,
        main_layer,
        active_probe: None,
    });
}

fn install_handlers(layer: *mut JfnCefLayer, _main_layer: *mut JfnCefLayer) {
    let l = unsafe { &*layer };

    // Created → overlay wins input.
    let lp_created = LayerPtr(layer);
    l.set_created_callback_rust(Some(Box::new(move |_b: *mut c_void| {
        let lp = &lp_created;
        jfn_browsers_set_active(lp.0);
    })));

    // Message dispatch.
    l.set_message_handler_rust(Some(Box::new(
        move |name: &str, args_raw: *mut c_void, browser_raw: *mut c_void| -> bool {
            handle_message(name, args_raw, browser_raw)
        },
    )));

    // Context menu.
    l.set_context_menu_builder_rust(Some(crate::app_menu::build_closure()));
    l.set_context_menu_dispatcher_rust(Some(crate::app_menu::dispatch_closure()));
}

fn list_string(args: &ListValue, idx: usize) -> String {
    let userfree = args.string(idx);
    let cs: CefString = (&userfree).into();
    cs.to_string()
}

fn apply_setting_value(_section: &str, key: &str, value: &str) {
    match key {
        "hwdec" => jfn_config::set_hwdec(value),
        "audioPassthrough" => jfn_config::set_audio_passthrough(value),
        "audioExclusive" => jfn_config::set_audio_exclusive(value == "true"),
        "audioChannels" => jfn_config::set_audio_channels(value),
        "titlebarThemeColor" => jfn_config::set_titlebar_theme_color(value == "true"),
        "logLevel" => jfn_config::set_log_level(value),
        // Pass empty platform_default — Rust setter clears when raw equals
        // the empty string. The overlay path doesn't have the live hostname
        // handy; accepting empty matches the legacy behaviour for the
        // overlay (set what the user typed, no auto-clear).
        "deviceName" => jfn_config::set_device_name(value, ""),
        _ => jfn_logging::log(
            jfn_logging::CATEGORY_CEF,
            jfn_logging::LEVEL_WARN,
            &format!("Unknown setting key: {_section}.{key}"),
        ),
    }
    jfn_config::settings_save_async();
}

fn handle_message(name: &str, args_raw: *mut c_void, browser_raw: *mut c_void) -> bool {
    // Adopt the refs CEF added before invoke; release on function exit.
    let args = (!args_raw.is_null()).then(|| -> ListValue {
        (args_raw as *mut sys::_cef_list_value_t).wrap_result()
    });
    let browser = (!browser_raw.is_null())
        .then(|| -> Browser { (browser_raw as *mut sys::_cef_browser_t).wrap_result() });

    match name {
        "getSavedServerUrl" => {
            let Some(b) = browser else { return true };
            let Some(frame) = b.main_frame() else {
                return true;
            };
            let url = jfn_config::server_url();
            send_process_message(&frame, "savedServerUrl", |args| {
                args.set_string(0, Some(&CefString::from(url.as_str())));
            });
            true
        }
        "navigateMain" => {
            let Some(args) = args else { return true };
            let url = list_string(&args, 0);
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_INFO,
                &format!("Overlay: navigateMain {url}"),
            );
            jfn_config::set_server_url(&url);
            jfn_config::settings_save_async();
            let main_layer = INSTANCE.lock().as_ref().map(|s| s.main_layer);
            if let Some(ml) = main_layer {
                unsafe { jfn_cef_layer_load_url(ml, url.as_ptr() as *const _, url.len()) };
            }
            true
        }
        "dismissOverlay" => {
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_INFO,
                "Overlay: dismissOverlay",
            );
            let (overlay, main_layer) = match INSTANCE.lock().as_ref() {
                Some(s) => (s.layer, s.main_layer),
                None => return true,
            };
            jfn_browsers_set_active(main_layer);
            let browser_for_close = browser.clone();
            let on_start: Box<dyn FnOnce() + Send> =
                Box::new(jfn_theme_color_on_overlay_dismissed);
            let on_done: Box<dyn FnOnce() + Send> = Box::new(move || {
                if let Some(b) = browser_for_close.as_ref()
                    && let Some(host) = b.host()
                {
                    host.close_browser(0);
                }
            });
            unsafe {
                jfn_cef_layer_fade(
                    overlay,
                    OVERLAY_FADE_DURATION_SEC,
                    Some(on_start),
                    Some(on_done),
                );
            }
            true
        }
        "saveServerUrl" => {
            let Some(args) = args else { return true };
            let url = list_string(&args, 0);
            jfn_config::set_server_url(&url);
            jfn_config::settings_save_async();
            true
        }
        "setSettingValue" => {
            let Some(args) = args else { return true };
            let section = list_string(&args, 0);
            let key = list_string(&args, 1);
            let value = list_string(&args, 2);
            apply_setting_value(&section, &key, &value);
            true
        }
        "checkServerConnectivity" => {
            let Some(args) = args else { return true };
            let Some(b) = browser else { return true };
            let url = list_string(&args, 0);
            cancel_active_probe();
            let normalized = normalize_input(&url);
            start_probe(b, url, normalized);
            true
        }
        "cancelServerConnectivity" => {
            cancel_active_probe();
            // Kill the pre-load.
            let main_layer = INSTANCE.lock().as_ref().map(|s| s.main_layer);
            if let Some(ml) = main_layer {
                unsafe { jfn_cef_layer_reset(ml) };
            }
            true
        }
        _ => false,
    }
}

fn send_process_message<F: FnOnce(&ListValue)>(frame: &Frame, name: &str, fill: F) {
    let Some(mut msg) = cef::process_message_create(Some(&CefString::from(name))) else {
        return;
    };
    if let Some(args) = msg.argument_list() {
        fill(&args);
    }
    frame.send_process_message(
        ProcessId::from(sys::cef_process_id_t::PID_RENDERER),
        Some(&mut msg),
    );
}

fn cancel_active_probe() {
    let probe = INSTANCE
        .lock()
        .as_mut()
        .and_then(|s| s.active_probe.take());
    if let Some(p) = probe {
        p.cancel();
    }
}

fn start_probe(browser: Browser, user_url: String, normalized: String) {
    let probe = ServerProbeClient::new(
        normalized.clone(),
        Box::new(move |success, base_url| {
            let Some(frame) = browser.main_frame() else {
                return;
            };
            let reply_url = if success {
                base_url.clone()
            } else {
                user_url.clone()
            };
            send_process_message(&frame, "serverConnectivityResult", |args| {
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

use std::sync::Arc;

type ProbeCallback = Box<dyn FnMut(bool, String) + Send + Sync>;

struct ProbeInner {
    callback: Mutex<Option<ProbeCallback>>,
    state: Mutex<ProbeState>,
}

struct ProbeState {
    url: String,
    phase: Phase,
    base: String,
    body: Vec<u8>,
    current_request: Option<Urlrequest>,
}

#[derive(Copy, Clone, PartialEq)]
enum Phase {
    Head,
    Get,
}

struct ServerProbeClient {
    inner: Arc<ProbeInner>,
}

impl ServerProbeClient {
    fn new(url: String, callback: ProbeCallback) -> Self {
        Self {
            inner: Arc::new(ProbeInner {
                callback: Mutex::new(Some(callback)),
                state: Mutex::new(ProbeState {
                    url,
                    phase: Phase::Head,
                    base: String::new(),
                    body: Vec::new(),
                    current_request: None,
                }),
            }),
        }
    }

    fn start(&self) {
        let (url_clone, request) = {
            let st = self.inner.state.lock();
            (st.url.clone(), make_request("HEAD", &st.url, self.client()))
        };
        if let Some(r) = request {
            let mut st = self.inner.state.lock();
            // Store the cef::Urlrequest in the active slot too so cancel
            // semantics line up with the C++ original.
            if let Some(s) = INSTANCE.lock().as_mut() {
                s.active_probe = Some(r.clone());
            }
            st.current_request = Some(r);
            let _ = url_clone;
        }
    }

    fn client(&self) -> UrlrequestClient {
        let inner = Arc::clone(&self.inner);
        JfnServerProbeClient::new(inner)
    }
}

impl ProbeInner {
    fn on_complete(self: &Arc<Self>, request: &Urlrequest) {
        // Capture phase + URL, then either start GET or finish.
        let next_request = {
            let mut st = self.state.lock();
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
                let req = make_request(
                    "GET",
                    &next_url,
                    JfnServerProbeClient::new(Arc::clone(self)),
                );
                if let Some(r) = req.clone() {
                    st.current_request = Some(r);
                }
                req
            } else {
                None
            }
        };
        if next_request.is_some() {
            return;
        }

        let (success, base) = {
            let st = self.state.lock();
            let mut ok = false;
            let status = request.request_status();
            if status.as_ref() == &sys::cef_urlrequest_status_t::UR_SUCCESS
                && let Some(resp) = request.response()
                && resp.status() == 200
            {
                ok = is_valid_public_info(&st.body);
            }
            (ok, st.base.clone())
        };
        let cb = self.callback.lock().take();
        self.state.lock().current_request = None;
        if let Some(mut f) = cb {
            f(success, base);
        }
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
        inner: Arc<ProbeInner>,
    }

    impl UrlrequestClient {
        fn on_request_complete(&self, request: Option<&mut Urlrequest>) {
            let Some(req) = request else { return };
            self.inner.on_complete(req);
        }
        fn on_download_data(
            &self,
            _request: Option<&mut Urlrequest>,
            data: *const u8,
            data_length: usize,
        ) {
            let mut st = self.inner.state.lock();
            if st.phase == Phase::Get && !data.is_null() && data_length > 0 {
                let slice = unsafe { std::slice::from_raw_parts(data, data_length) };
                st.body.extend_from_slice(slice);
            }
        }
    }
}

