//! `cef::App` implementation. Owns both browser-process and render-process
//! handlers — CEF re-execs the same binary for child processes, so the same
//! App is constructed in every process and CEF dispatches based on the
//! `--type=` switch.

use cef::*;
use parking_lot::Mutex;
use std::collections::HashMap;

use crate::embedded_js;
use crate::injection::ExtraInfo;
use crate::paint_scheduler::PaintScheduler;
use crate::state;
use crate::v8_handler::NativeHandlerBuilder;

// `app://` scheme options. Match CEF_SCHEME_OPTION_* from
// include/internal/cef_types.h.
const SCHEME_OPTION_STANDARD: i32 = 1 << 0;
const SCHEME_OPTION_LOCAL: i32 = 1 << 1;
const SCHEME_OPTION_SECURE: i32 = 1 << 4;
const SCHEME_OPTION_CORS_ENABLED: i32 = 1 << 6;

// V8 property attribute. Equivalent to V8_PROPERTY_ATTRIBUTE_READONLY.
fn readonly_attr() -> V8Propertyattribute {
    V8Propertyattribute::from(sys::cef_v8_propertyattribute_t::V8_PROPERTY_ATTRIBUTE_READONLY)
}

// ----- App ------------------------------------------------------------------

// Shared profile map. CEF may call `App::render_process_handler()` more than
// once per process; each call must hand back a handler that shares the same
// browser-id → injection-profile map. Holding the inner state on JfnApp via
// Arc lets us clone-cheap on every render_process_handler() invocation while
// preserving the map across calls.
type ProfileMap = std::sync::Arc<Mutex<HashMap<i32, ExtraInfo>>>;

#[derive(Clone)]
pub struct JfnApp {
    profiles: ProfileMap,
}

impl JfnApp {
    pub fn new() -> Self {
        Self {
            profiles: std::sync::Arc::new(Mutex::new(HashMap::new())),
        }
    }
}

wrap_app! {
    pub struct JfnAppBuilder {
        inner: JfnApp,
    }

    impl App {
        fn on_before_command_line_processing(
            &self,
            process_type: Option<&CefStringUtf16>,
            command_line: Option<&mut CommandLine>,
        ) {
            let Some(cl) = command_line else { return };

            // Disable all Google services.
            for sw in [
                "disable-background-networking",
                "disable-client-side-phishing-detection",
                "disable-default-apps",
                "disable-extensions",
                "disable-component-update",
                "disable-sync",
                "disable-translate",
                "disable-domain-reliability",
                "disable-breakpad",
                "disable-notifications",
                "disable-spell-checking",
                "no-pings",
                "bwsi",
            ] {
                cl.append_switch(Some(&CefString::from(sw)));
            }

            for (name, value) in [
                (
                    "disable-features",
                    "PushMessaging,BackgroundSync,SafeBrowsing,Translate,OptimizationHints,\
                     MediaRouter,DialMediaRouteProvider,AcceptCHFrame,AutofillServerCommunication,\
                     CertificateTransparencyComponentUpdater,SyncNotificationServiceWhenSignedIn,\
                     SpellCheck,SpellCheckService,PasswordManager",
                ),
                ("google-api-key", ""),
                ("google-default-client-id", ""),
                ("google-default-client-secret", ""),
            ] {
                cl.append_switch_with_value(
                    Some(&CefString::from(name)),
                    Some(&CefString::from(value)),
                );
            }

            // Browser-process-only switches from CefRuntime::Set*().
            let is_browser_process = process_type
                .map(|s| s.to_string().is_empty())
                .unwrap_or(true);
            if is_browser_process {
                for sw in state::snapshot_switches() {
                    match sw.value {
                        None => cl.append_switch(Some(&CefString::from(sw.name.as_str()))),
                        Some(v) => cl.append_switch_with_value(
                            Some(&CefString::from(sw.name.as_str())),
                            Some(&CefString::from(v.as_str())),
                        ),
                    }
                }
            }
        }

        fn on_register_custom_schemes(&self, registrar: Option<&mut SchemeRegistrar>) {
            let Some(reg) = registrar else { return };
            let name = CefString::from("app");
            reg.add_custom_scheme(
                Some(&name),
                SCHEME_OPTION_STANDARD
                    | SCHEME_OPTION_SECURE
                    | SCHEME_OPTION_LOCAL
                    | SCHEME_OPTION_CORS_ENABLED,
            );
        }

        fn browser_process_handler(&self) -> Option<BrowserProcessHandler> {
            Some(BphBuilder::new(JfnBph))
        }

        fn render_process_handler(&self) -> Option<RenderProcessHandler> {
            Some(RphBuilder::new(JfnRph { profiles: self.inner.profiles.clone() }))
        }
    }
}

// ----- BrowserProcessHandler ------------------------------------------------

#[derive(Clone)]
struct JfnBph;

wrap_browser_process_handler! {
    struct BphBuilder { inner: JfnBph, }

    impl BrowserProcessHandler {
        fn on_context_initialized(&self) {
            jfn_logging::log(jfn_logging::CATEGORY_CEF, jfn_logging::LEVEL_INFO, "CEF context initialized");
            crate::resource::register();
            // Optional C-side callback (kept for any future C++ context-init
            // hooks; currently unused now that scheme registration is in Rust).
            if let Some(cb) = state::with_config(|c| c.on_context_initialized) {
                cb();
            }
        }

        fn on_schedule_message_pump_work(&self, delay_ms: i64) {
            #[cfg(target_os = "macos")]
            crate::pump::on_schedule(delay_ms);
            #[cfg(not(target_os = "macos"))]
            let _ = delay_ms;
        }
    }
}

// ----- RenderProcessHandler -------------------------------------------------
//
// Renderer-local map of browser identifier → injection profile passed through
// extra_info at CreateBrowser time. Populated in on_browser_created, consumed
// in on_context_created, erased in on_browser_destroyed.

#[derive(Default, Clone)]
struct JfnRph {
    profiles: ProfileMap,
}

wrap_render_process_handler! {
    struct RphBuilder { inner: JfnRph, }

    impl RenderProcessHandler {
        fn on_browser_created(
            &self,
            browser: Option<&mut Browser>,
            extra_info: Option<&mut DictionaryValue>,
        ) {
            let (Some(browser), Some(extra)) = (browser, extra_info) else { return };
            let id = browser.identifier();
            self.inner
                .profiles
                .lock()
                .insert(id, ExtraInfo::from_dictionary(extra.clone()));
        }

        fn on_browser_destroyed(&self, browser: Option<&mut Browser>) {
            let Some(browser) = browser else { return };
            let id = browser.identifier();
            self.inner.profiles.lock().remove(&id);
        }

        fn on_context_created(
            &self,
            browser: Option<&mut Browser>,
            frame: Option<&mut Frame>,
            context: Option<&mut V8Context>,
        ) {
            let (Some(browser), Some(frame), Some(ctx)) = (browser, frame, context) else {
                return;
            };
            // Top-frame only. jmpNative + player shim must not pollute iframes.
            if frame.is_main() != 1 {
                return;
            }

            let profile = {
                let id = browser.identifier();
                self.inner.profiles.lock().get(&id).cloned()
            };
            let Some(profile) = profile else { return };

            inject_jmp_native(browser, &profile, ctx);
            PaintScheduler::on_context_created(profile.shared_textures_enabled(), frame);
            run_user_scripts(&profile, frame);
        }

        fn on_process_message_received(
            &self,
            _browser: Option<&mut Browser>,
            frame: Option<&mut Frame>,
            _source_process: ProcessId,
            message: Option<&mut ProcessMessage>,
        ) -> ::std::os::raw::c_int {
            let (Some(frame), Some(msg)) = (frame, message) else { return 0 };
            let name = userfree_to_string(&msg.name());
            let args = msg.argument_list();

            match name.as_str() {
                "savedServerUrl" => {
                    let Some(args) = args else { return 1 };
                    let url = userfree_to_string(&args.string(0));
                    call_js_global_string(frame, "_onSavedServerUrl", &[Arg::Str(&url)]);
                    1
                }
                "serverConnectivityResult" => {
                    let Some(args) = args else { return 1 };
                    let url = userfree_to_string(&args.string(0));
                    let ok = args.bool(1) != 0;
                    let detail = userfree_to_string(&args.string(2));
                    call_js_global_string(
                        frame,
                        "_onServerConnectivityResult",
                        &[Arg::Str(&url), Arg::Bool(ok), Arg::Str(&detail)],
                    );
                    1
                }
                "getPopupOptions" => {
                    let popup = collect_popup_options(frame);
                    let Some(mut reply) = process_message_create(Some(&CefString::from("popupOptions")))
                    else { return 1 };
                    if let Some(reply_args) = reply.argument_list() {
                        if let Some(mut list) = list_value_create() {
                            for (i, opt) in popup.options.iter().enumerate() {
                                list.set_string(i, Some(&CefString::from(opt.as_str())));
                            }
                            reply_args.set_list(0, Some(&mut list));
                        }
                        reply_args.set_int(1, popup.selected);
                        if let Some(mut sel_list) = list_value_create() {
                            for (i, idx) in popup.selectable.iter().enumerate() {
                                sel_list.set_int(i, *idx);
                            }
                            reply_args.set_list(2, Some(&mut sel_list));
                        }
                        if let Some((ax, ay)) = popup.anchor {
                            reply_args.set_int(3, ax);
                            reply_args.set_int(4, ay);
                            reply_args.set_int(5, 1);
                        } else {
                            reply_args.set_int(5, 0);
                        }
                    }
                    frame.send_process_message(
                        ProcessId::from(sys::cef_process_id_t::PID_BROWSER),
                        Some(&mut reply),
                    );
                    1
                }
                _ => 0,
            }
        }
    }
}

enum Arg<'a> {
    Str(&'a str),
    Bool(bool),
}

fn call_js_global_string(frame: &Frame, fn_name: &str, args: &[Arg<'_>]) {
    let Some(ctx) = frame.v8_context() else {
        return;
    };
    if ctx.enter() != 1 {
        return;
    }
    let _drop = ContextExit(&ctx);
    let Some(global) = ctx.global() else { return };
    let fn_key = CefString::from(fn_name);
    let Some(fn_val) = global.value_bykey(Some(&fn_key)) else {
        return;
    };
    if fn_val.is_function() != 1 {
        return;
    }
    let v8_args: Vec<Option<V8Value>> = args
        .iter()
        .map(|a| match a {
            Arg::Str(s) => v8_value_create_string(Some(&CefString::from(*s))),
            Arg::Bool(b) => v8_value_create_bool(if *b { 1 } else { 0 }),
        })
        .collect();
    fn_val.execute_function(None, Some(&v8_args));
}

struct ContextExit<'a>(&'a V8Context);

impl Drop for ContextExit<'_> {
    fn drop(&mut self) {
        self.0.exit();
    }
}

struct PopupOptions {
    options: Vec<String>,
    selected: i32,
    selectable: Vec<i32>,
    anchor: Option<(i32, i32)>,
}

fn collect_popup_options(frame: &Frame) -> PopupOptions {
    let mut options = Vec::new();
    let mut selected = -1;
    let mut selectable = Vec::new();
    let mut anchor = None;
    let Some(ctx) = frame.v8_context() else {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    };
    if ctx.enter() != 1 {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    }
    let _drop = ContextExit(&ctx);
    let Some(global) = ctx.global() else {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    };
    let doc_key = CefString::from("document");
    let Some(doc) = global.value_bykey(Some(&doc_key)) else {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    };
    let active_key = CefString::from("activeElement");
    let Some(mut el) = doc.value_bykey(Some(&active_key)) else {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    };
    if el.is_object() != 1 {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    }
    let tag_key = CefString::from("tagName");
    let Some(tag) = el.value_bykey(Some(&tag_key)) else {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    };
    if tag.is_string() != 1 {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    }
    if userfree_to_string(&tag.string_value()) != "SELECT" {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    }
    let rect_key = CefString::from("getBoundingClientRect");
    if let Some(get_rect) = el.value_bykey(Some(&rect_key))
        && get_rect.is_function() == 1
        && let Some(rect) = get_rect.execute_function(Some(&mut el), Some(&[]))
        && rect.is_object() == 1
    {
        let read = |name: &str| -> Option<f64> {
            let key = CefString::from(name);
            rect.value_bykey(Some(&key))
                .filter(|v| v.is_double() == 1 || v.is_int() == 1)
                .map(|v| v.double_value())
        };
        if let (Some(left), Some(bottom)) = (read("left"), read("bottom")) {
            anchor = Some((left.round() as i32, bottom.round() as i32));
        }
    }
    let opts_key = CefString::from("options");
    let Some(opts) = el.value_bykey(Some(&opts_key)) else {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    };
    let len_key = CefString::from("length");
    let Some(len_val) = opts.value_bykey(Some(&len_key)) else {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    };
    if opts.is_object() != 1 || len_val.is_int() != 1 {
        return PopupOptions {
            options,
            selected,
            selectable,
            anchor,
        };
    }
    let len = len_val.int_value();
    let text_key = CefString::from("text");
    let disabled_key = CefString::from("disabled");
    let parent_key = CefString::from("parentNode");
    let tagname_key = CefString::from("tagName");
    let read_disabled = |o: &V8Value| -> bool {
        o.value_bykey(Some(&disabled_key))
            .is_some_and(|d| d.is_bool() == 1 && d.bool_value() != 0)
    };
    for i in 0..len {
        let Some(opt) = opts.value_byindex(i) else {
            options.push(String::new());
            continue;
        };
        let mut s = String::new();
        let mut disabled = false;
        if opt.is_object() == 1 {
            if let Some(t) = opt.value_bykey(Some(&text_key))
                && t.is_string() == 1
            {
                s = userfree_to_string(&t.string_value());
            }
            disabled = read_disabled(&opt);
            if !disabled
                && let Some(parent) = opt.value_bykey(Some(&parent_key))
                && parent.is_object() == 1
                && let Some(ptag) = parent.value_bykey(Some(&tagname_key))
                && ptag.is_string() == 1
                && userfree_to_string(&ptag.string_value()) == "OPTGROUP"
            {
                disabled = read_disabled(&parent);
            }
        }
        if !disabled {
            selectable.push(i);
        }
        options.push(s);
    }
    let sel_key = CefString::from("selectedIndex");
    if let Some(sel) = el.value_bykey(Some(&sel_key))
        && sel.is_int() == 1
    {
        selected = sel.int_value();
    }
    PopupOptions {
        options,
        selected,
        selectable,
        anchor,
    }
}

fn inject_jmp_native(browser: &mut Browser, profile: &ExtraInfo, context: &mut V8Context) {
    let Some(global) = context.global() else {
        return;
    };
    let Some(mut jmp_native) = v8_value_create_object(None, None) else {
        return;
    };
    let browser_id = browser.identifier();
    for function in profile.functions() {
        let cef_name = CefString::from(function.name());
        let _ = browser_id;
        let mut handler = NativeHandlerBuilder::new(crate::v8_handler::NativeHandler);
        let Some(mut fn_val) = v8_value_create_function(Some(&cef_name), Some(&mut handler)) else {
            continue;
        };
        jmp_native.set_value_bykey(Some(&cef_name), Some(&mut fn_val), readonly_attr());
    }
    let key = CefString::from("jmpNative");
    global.set_value_bykey(Some(&key), Some(&mut jmp_native), readonly_attr());
}

fn run_user_scripts(profile: &ExtraInfo, frame: &Frame) {
    let scripts = profile.scripts();
    if scripts.is_empty() {
        return;
    }

    // Renderer is a separate process; load settings here for placeholder
    // substitution.
    ensure_renderer_settings_loaded();

    let mut code = String::new();
    for (i, script) in scripts.iter().enumerate() {
        if i > 0 {
            code.push('\n');
        }
        if let Some(src) = embedded_js::get(script.file_name()) {
            code.push_str(src);
        }
    }

    fn replace_first(code: &mut String, ph: &str, value: &str) {
        if let Some(pos) = code.find(ph) {
            code.replace_range(pos..pos + ph.len(), value);
        }
    }
    replace_first(&mut code, "__SERVER_URL__", &jfn_config::server_url());
    replace_first(
        &mut code,
        "__SETTINGS_JSON__",
        &jfn_config::cli_json(&platform_device_name(), &hwdec_options()),
    );
    replace_first(&mut code, "__APP_VERSION__", crate::APP_VERSION);
    replace_first(
        &mut code,
        "__KDE_PALETTE_SUPPORTED__",
        if cfg!(all(target_os = "linux", feature = "kde-palette")) {
            "true"
        } else {
            "false"
        },
    );
    replace_first(
        &mut code,
        "__CSD_SUPPORTED__",
        if cfg!(target_os = "linux") {
            "true"
        } else {
            "false"
        },
    );

    if let Some(dp) = profile.device_profile_json() {
        replace_first(&mut code, "__DEVICE_PROFILE_JSON__", dp);
    }

    if let Some(wd) = profile.window_decorations() {
        replace_first(&mut code, "__WINDOW_DECORATIONS__", wd);
    }

    let url_uf = frame.url();
    let url = CefString::from(&url_uf);
    let code_cef = CefString::from(code.as_str());
    frame.execute_java_script(Some(&code_cef), Some(&url), 0);
}

fn ensure_renderer_settings_loaded() {
    use std::sync::OnceLock;
    static INITED: OnceLock<()> = OnceLock::new();
    INITED.get_or_init(|| {
        let path = jfn_paths::config_dir().join("settings.json");
        jfn_config::settings_init(&path);
        let _ = jfn_config::settings_load();
    });
}

// ----- helpers --------------------------------------------------------------

pub(crate) fn userfree_to_string(s: &CefStringUserfreeUtf16) -> String {
    let raw: Option<&sys::_cef_string_utf16_t> = s.into();
    raw.map(|r| {
        if r.str_.is_null() || r.length == 0 {
            String::new()
        } else {
            let slice = unsafe { std::slice::from_raw_parts(r.str_, r.length) };
            String::from_utf16_lossy(slice)
        }
    })
    .unwrap_or_default()
}

fn hwdec_options() -> Vec<&'static str> {
    let mut v: Vec<&'static str> = vec!["auto", "no"];
    #[cfg(target_os = "linux")]
    v.extend_from_slice(&["vaapi", "nvdec", "vulkan"]);
    #[cfg(target_os = "windows")]
    v.extend_from_slice(&["d3d11va", "nvdec", "vulkan"]);
    #[cfg(target_os = "macos")]
    v.extend_from_slice(&["videotoolbox", "vulkan"]);
    v
}

#[cfg(any(target_os = "linux", target_os = "macos"))]
fn platform_device_name() -> String {
    let mut buf = [0u8; 256];
    let rc = unsafe { libc::gethostname(buf.as_mut_ptr() as *mut _, buf.len()) };
    if rc != 0 {
        return String::new();
    }
    let len = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    let mut s = String::from_utf8_lossy(&buf[..len]).into_owned();
    s.truncate(64);
    s
}

#[cfg(target_os = "windows")]
fn platform_device_name() -> String {
    let mut s = std::env::var("COMPUTERNAME").unwrap_or_default();
    s.truncate(64);
    s
}
