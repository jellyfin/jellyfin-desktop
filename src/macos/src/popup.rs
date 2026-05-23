//! NSMenu-based popup for `<select>` widgets. Replaces CEF's Alloy OSR
//! popup widget which renders hover/selection highlights as opaque black
//! on macOS. CEF's popup widget runs invisibly in the background; we
//! display a native NSMenu in its place. The selected index (or -1 for
//! cancel) is reported back via the JfnPopupRequest::on_selected
//! callback.

use std::ffi::{CStr, c_int, c_void};
use std::sync::{Arc, Mutex};

use objc2::rc::Retained;
use objc2::runtime::AnyObject;
use objc2::{DefinedClass, define_class, msg_send};
use objc2_foundation::{NSObject, NSPoint, NSString};

use jfn_platform_abi::JfnPopupRequest;

/// Owns the on_selected (fn, ctx, dtor) triple. The dtor fires once when
/// the last Arc reference is dropped.
struct PopupCb {
    fn_: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
    ctx: *mut c_void,
    dtor: Option<unsafe extern "C" fn(*mut c_void)>,
}

// PopupCb owns a `*mut c_void` ctx + raw fn pointers. We hand it off to
// the AppKit main queue across thread boundaries; the underlying C
// callback contract is responsible for its own thread-safety.
unsafe impl Send for PopupCb {}
unsafe impl Sync for PopupCb {}

impl Drop for PopupCb {
    fn drop(&mut self) {
        if let Some(d) = self.dtor {
            unsafe { d(self.ctx) };
        }
    }
}

impl PopupCb {
    fn fire(&self, idx: c_int) {
        if let Some(f) = self.fn_ {
            unsafe { f(self.ctx, idx) };
        }
    }
}

/// Ivar storage for the NSMenu target — holds the callback Arc plus a
/// "fired" flag so we report cancel only when nothing was picked.
struct TargetIvars {
    cb: Arc<PopupCb>,
    fired: Mutex<bool>,
}

impl TargetIvars {
    fn fire_if_first(&self, idx: c_int) {
        let mut g = self.fired.lock().unwrap();
        if *g {
            return;
        }
        *g = true;
        self.cb.fire(idx);
    }
}

define_class!(
    #[unsafe(super(NSObject))]
    #[name = "JfnPopupMenuTarget"]
    #[ivars = TargetIvars]
    struct PopupTarget;

    impl PopupTarget {
        #[unsafe(method(itemPicked:))]
        fn item_picked(&self, item: &AnyObject) {
            let tag: isize = unsafe { msg_send![item, tag] };
            self.ivars().fire_if_first(tag as c_int);
        }
    }
);

unsafe extern "C" {
    fn dispatch_get_main_queue() -> *mut c_void;
    fn dispatch_async_f(
        queue: *mut c_void,
        ctx: *mut c_void,
        work: unsafe extern "C" fn(*mut c_void),
    );

    fn jfn_macos_get_window() -> *mut AnyObject;
    fn jfn_macos_get_input_view() -> *mut AnyObject;
}

/// Heap-allocated state delivered to the main-queue trampoline. Owns the
/// option titles (so they outlive the caller's req buffer) and the
/// callback Arc.
struct PopupRun {
    cb: Arc<PopupCb>,
    options: Vec<String>,
    initial_highlight: c_int,
    x: c_int,
    y: c_int,
    min_width: c_int,
}

unsafe extern "C" fn run_trampoline(ctx: *mut c_void) {
    let run: Box<PopupRun> = unsafe { Box::from_raw(ctx as *mut PopupRun) };
    unsafe { show_menu_on_main(*run) };
}

unsafe fn show_menu_on_main(run: PopupRun) {
    let window = unsafe { jfn_macos_get_window() };
    let input_view = unsafe { jfn_macos_get_input_view() };
    if window.is_null() || input_view.is_null() {
        // Drop fires the on_selected_dtor automatically; no fire() so the
        // JS side gets no spurious selection. Matches the C++ guard.
        return;
    }

    let menu_cls = objc2::class!(NSMenu);
    let menu: *mut AnyObject = unsafe { msg_send![menu_cls, alloc] };
    let empty = NSString::from_str("");
    let menu: *mut AnyObject = unsafe { msg_send![menu, initWithTitle: &*empty] };
    let _: () = unsafe { msg_send![menu, setAutoenablesItems: false] };

    let ivars = TargetIvars {
        cb: run.cb.clone(),
        fired: Mutex::new(false),
    };
    let target = PopupTarget::alloc().set_ivars(ivars);
    let target: Retained<PopupTarget> = unsafe { msg_send![super(target), init] };

    let item_picked_sel = objc2::sel!(itemPicked:);

    let item_cls = objc2::class!(NSMenuItem);
    let key_equiv = NSString::from_str("");
    for (i, opt) in run.options.iter().enumerate() {
        let title = NSString::from_str(opt);
        let item: *mut AnyObject = unsafe { msg_send![item_cls, alloc] };
        let item: *mut AnyObject = unsafe {
            msg_send![
                item,
                initWithTitle: &*title,
                action: item_picked_sel,
                keyEquivalent: &*key_equiv,
            ]
        };
        let _: () = unsafe { msg_send![item, setTag: i as isize] };
        let _: () = unsafe { msg_send![item, setTarget: &*target] };
        if i as c_int == run.initial_highlight {
            // NSControlStateValueOn == 1
            let _: () = unsafe { msg_send![item, setState: 1isize] };
        }
        let _: () = unsafe { msg_send![menu, addItem: item] };
    }

    let location = NSPoint {
        x: run.x as f64,
        y: run.y as f64,
    };

    let initial: *mut AnyObject = if run.initial_highlight >= 0
        && (run.initial_highlight as usize) < run.options.len()
    {
        unsafe { msg_send![menu, itemAtIndex: run.initial_highlight as isize] }
    } else {
        std::ptr::null_mut()
    };

    let _: () = unsafe { msg_send![menu, setMinimumWidth: run.min_width as f64] };

    let _: bool = unsafe {
        msg_send![
            menu,
            popUpMenuPositioningItem: initial,
            atLocation: location,
            inView: input_view,
        ]
    };

    // Modal call; if no item was picked the target hasn't fired yet —
    // report cancel.
    target.ivars().fire_if_first(-1);
    drop(target);
}

/// `macos_popup_show` — anchored in the input NSView (isFlipped == YES)
/// so (x, y) are layout coordinates that map directly to AppKit menu
/// placement. Schedules the actual NSMenu work onto the main queue.
#[unsafe(no_mangle)]
pub extern "C" fn macos_popup_show(_s: *mut c_void, req: *const JfnPopupRequest) {
    if req.is_null() {
        return;
    }
    let req = unsafe { &*req };
    let cb = Arc::new(PopupCb {
        fn_: req.on_selected,
        ctx: req.on_selected_ctx,
        dtor: req.on_selected_dtor,
    });
    if req.options_len == 0 {
        // Drop cb → fires the dtor; we never call on_selected. Matches
        // the C++ early-return.
        return;
    }

    // Copy option strings out of the caller's buffer before scheduling —
    // the caller's req buffer is gone by the time the main queue runs.
    let mut opts: Vec<String> = Vec::with_capacity(req.options_len);
    for i in 0..req.options_len {
        let p = if req.options.is_null() {
            std::ptr::null()
        } else {
            unsafe { *req.options.add(i) }
        };
        let s = if p.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
        };
        opts.push(s);
    }

    let run = Box::new(PopupRun {
        cb,
        options: opts,
        initial_highlight: req.initial_highlight,
        x: req.x,
        y: req.y,
        min_width: req.lw,
    });
    let ctx = Box::into_raw(run) as *mut c_void;
    unsafe { dispatch_async_f(dispatch_get_main_queue(), ctx, run_trampoline) };
}
