use parking_lot::Mutex;
use std::ffi::{c_int, c_void};
use std::sync::Arc;

use objc2::rc::Retained;
use objc2::runtime::AnyObject;
use objc2::{AnyThread, DefinedClass, define_class, msg_send};
use objc2_foundation::{NSObject, NSPoint, NSString};

use jfn_platform_abi::MenuSelectionFn;

use crate::init::{jfn_macos_get_input_view, jfn_macos_get_window};

pub(crate) struct MenuEntry {
    pub title: String,
    pub tag: c_int,
    pub enabled: bool,
    pub separator: bool,
    pub checked: bool,
}

pub(crate) struct MenuSpec {
    pub entries: Vec<MenuEntry>,
    pub x: c_int,
    pub y: c_int,
    pub positioning_tag: Option<c_int>,
    pub min_width: Option<c_int>,
}

struct SelectionCb {
    fired: Mutex<Option<MenuSelectionFn>>,
}

impl SelectionCb {
    fn fire(&self, id: c_int) {
        if let Some(cb) = self.fired.lock().take() {
            cb(id);
        }
    }
}

struct TargetIvars {
    cb: Arc<SelectionCb>,
}

define_class!(
    #[unsafe(super(NSObject))]
    #[name = "JfnMenuTarget"]
    #[ivars = TargetIvars]
    struct MenuTarget;

    impl MenuTarget {
        #[unsafe(method(itemPicked:))]
        fn item_picked(&self, item: &AnyObject) {
            let tag: isize = unsafe { msg_send![item, tag] };
            self.ivars().cb.fire(tag as c_int);
        }
    }
);

unsafe extern "C" {
    static _dispatch_main_q: c_void;
    fn dispatch_async_f(
        queue: *mut c_void,
        ctx: *mut c_void,
        work: unsafe extern "C" fn(*mut c_void),
    );
}

#[inline]
fn dispatch_get_main_queue() -> *mut c_void {
    std::ptr::addr_of!(_dispatch_main_q) as *mut c_void
}

/// Owns the spec so it outlives the caller across the async hop to the main queue.
struct MenuRun {
    cb: Arc<SelectionCb>,
    spec: MenuSpec,
}

unsafe extern "C" fn run_trampoline(ctx: *mut c_void) {
    let run: Box<MenuRun> = unsafe { Box::from_raw(ctx as *mut MenuRun) };
    unsafe { show_menu_on_main(*run) };
}

unsafe fn show_menu_on_main(run: MenuRun) {
    let window = jfn_macos_get_window();
    let input_view = jfn_macos_get_input_view();
    if window.is_null() || input_view.is_null() {
        // Report cancel so callers don't hang waiting on a selection.
        run.cb.fire(-1);
        return;
    }

    let menu_cls = objc2::class!(NSMenu);
    let menu: *mut AnyObject = unsafe { msg_send![menu_cls, alloc] };
    let empty = NSString::from_str("");
    let menu: *mut AnyObject = unsafe { msg_send![menu, initWithTitle: &*empty] };
    let _: () = unsafe { msg_send![menu, setAutoenablesItems: false] };

    let ivars = TargetIvars { cb: run.cb.clone() };
    let target = MenuTarget::alloc().set_ivars(ivars);
    let target: Retained<MenuTarget> = unsafe { msg_send![super(target), init] };

    let item_picked_sel = objc2::sel!(itemPicked:);
    let item_cls = objc2::class!(NSMenuItem);
    let key_equiv = NSString::from_str("");

    let mut positioning_item: *mut AnyObject = std::ptr::null_mut();
    for entry in &run.spec.entries {
        if entry.separator {
            let sep: *mut AnyObject = unsafe { msg_send![item_cls, separatorItem] };
            let _: () = unsafe { msg_send![menu, addItem: sep] };
            continue;
        }
        let title = NSString::from_str(&entry.title);
        let item: *mut AnyObject = unsafe { msg_send![item_cls, alloc] };
        let item: *mut AnyObject = unsafe {
            msg_send![
                item,
                initWithTitle: &*title,
                action: item_picked_sel,
                keyEquivalent: &*key_equiv,
            ]
        };
        let _: () = unsafe { msg_send![item, setTag: entry.tag as isize] };
        let _: () = unsafe { msg_send![item, setTarget: &*target] };
        let _: () = unsafe { msg_send![item, setEnabled: entry.enabled] };
        if entry.checked {
            // NSControlStateValueOn == 1
            let _: () = unsafe { msg_send![item, setState: 1isize] };
        }
        let _: () = unsafe { msg_send![menu, addItem: item] };
        if run.spec.positioning_tag == Some(entry.tag) {
            positioning_item = item;
        }
    }

    if let Some(w) = run.spec.min_width {
        let _: () = unsafe { msg_send![menu, setMinimumWidth: w as f64] };
    }

    let location = NSPoint {
        x: run.spec.x as f64,
        y: run.spec.y as f64,
    };
    let _: bool = unsafe {
        msg_send![
            menu,
            popUpMenuPositioningItem: positioning_item,
            atLocation: location,
            inView: input_view,
        ]
    };

    // Modal call; if no item was picked the target hasn't fired yet —
    // report cancel.
    run.cb.fire(-1);
    drop(target);
}

pub(crate) fn present_on_main(spec: MenuSpec, on_selected: Option<MenuSelectionFn>) {
    let cb = Arc::new(SelectionCb {
        fired: Mutex::new(on_selected),
    });
    let run = Box::new(MenuRun { cb, spec });
    let ctx = Box::into_raw(run) as *mut c_void;
    unsafe { dispatch_async_f(dispatch_get_main_queue(), ctx, run_trampoline) };
}
