mod lifecycle_fsm;

use std::sync::OnceLock;
use std::sync::mpsc::{Receiver, Sender, channel};

use x11rb::connection::Connection;
use x11rb::protocol::Event;
use x11rb::protocol::shm::ConnectionExt as ShmConnectionExt;
use x11rb::protocol::xproto::{
    ConnectionExt as XprotoConnectionExt, CreateGCAux, CreateWindowAux, EventMask, GrabMode,
    GrabStatus, ImageFormat, StackMode, WindowClass,
};
use x11rb::rust_connection::RustConnection;

use crate::shm::{shm_alloc, shm_free};
use crate::x11_state::{MUT, ShmBuffer};
use jfn_menu::interaction_fsm::{self, MenuEffect, MenuEvent, MenuState};
use jfn_menu::render::{self, Fonts, Layout};
use lifecycle_fsm::{Life, LifeEffect, LifeEvent};

pub use jfn_menu::MenuItem;

pub struct MenuRequest {
    /// CEF view (logical, unscaled) coordinates of the click.
    pub x: i32,
    pub y: i32,
    pub items: Vec<MenuItem>,
    pub on_selected: Option<Box<dyn FnOnce(i32) + Send>>,
}

static SENDER: OnceLock<Option<Sender<MenuRequest>>> = OnceLock::new();

pub fn show(req: MenuRequest) {
    tracing::debug!(
        target: "x11::menu",
        "show: {} items at view=({},{})",
        req.items.len(),
        req.x,
        req.y,
    );
    match SENDER.get_or_init(spawn_worker) {
        Some(tx) => {
            if let Err(e) = tx.send(req) {
                tracing::error!(target: "x11::menu", "show: worker gone; dismissing");
                fire(e.0.on_selected, -1);
            }
        }
        None => {
            tracing::error!(target: "x11::menu", "show: no worker thread; dismissing");
            fire(req.on_selected, -1);
        }
    }
}

fn fire(cb: Option<Box<dyn FnOnce(i32) + Send>>, result: i32) {
    if let Some(cb) = cb {
        cb(result);
    }
}

fn spawn_worker() -> Option<Sender<MenuRequest>> {
    let (tx, rx) = channel::<MenuRequest>();
    std::thread::Builder::new()
        .name("jfn-x11-menu".into())
        .spawn(move || worker(rx))
        .ok()?;
    Some(tx)
}

fn worker(rx: Receiver<MenuRequest>) {
    let Ok((conn, _screen)) = x11rb::connect(None) else {
        tracing::error!(target: "x11::menu", "worker: X11 connect failed; menus disabled");
        for req in rx {
            fire(req.on_selected, -1);
        }
        return;
    };
    tracing::debug!(target: "x11::menu", "worker: started");
    let keymap = Keymap::query(&conn);
    let mut fonts = Fonts::new();
    for mut req in rx {
        let cb = req.on_selected.take();
        let result = run_menu(&conn, &keymap, &mut fonts, &req);
        tracing::debug!(target: "x11::menu", "result: id={result}");
        fire(cb, result);
    }
}

struct Snap {
    visual: u32,
    depth: u8,
    colormap: u32,
    root: u32,
    parent_x: i32,
    parent_y: i32,
    scale: f32,
    root_w: i32,
    root_h: i32,
}

fn snapshot(conn: &RustConnection) -> Option<Snap> {
    let g = MUT.lock();
    let m = g.as_ref()?;
    let screen = conn
        .setup()
        .roots
        .iter()
        .find(|s| s.root == m.root)
        .or_else(|| conn.setup().roots.first())?;
    Some(Snap {
        visual: m.argb_visual,
        depth: m.argb_depth,
        colormap: m.colormap,
        root: m.root,
        parent_x: m.parent_x,
        parent_y: m.parent_y,
        scale: jfn_platform_abi::scale_get_or_one() as f32,
        root_w: screen.width_in_pixels as i32,
        root_h: screen.height_in_pixels as i32,
    })
}

fn place(snap: &Snap, cx: i32, cy: i32, w: i32, h: i32) -> (i32, i32) {
    let p: jfn_platform_abi::PhysicalPosition = jfn_platform_abi::LogicalPosition::new(cx, cy)
        .to_physical(jfn_platform_abi::scale_or_one(f64::from(snap.scale)));
    let mut x = snap.parent_x + p.x;
    let mut y = snap.parent_y + p.y;
    if x + w > snap.root_w {
        x = (snap.root_w - w).max(0);
    }
    if y + h > snap.root_h {
        let above = y - h;
        y = if above >= 0 {
            above
        } else {
            (snap.root_h - h).max(0)
        };
    }
    (x.max(0), y.max(0))
}

fn close(life: &mut Life, ev: LifeEvent) -> i32 {
    lifecycle_fsm::step(life, &ev)
        .into_iter()
        .find_map(|e| match e {
            LifeEffect::Fire(id) => Some(id),
            _ => None,
        })
        .unwrap_or(-1)
}

fn run_menu(conn: &RustConnection, keymap: &Keymap, fonts: &mut Fonts, req: &MenuRequest) -> i32 {
    let mut life = Life::default();
    let _ = lifecycle_fsm::step(&mut life, &LifeEvent::Show);

    let Some(snap) = snapshot(conn) else {
        tracing::warn!(target: "x11::menu", "run_menu: no X11 state snapshot; dismissing");
        return close(&mut life, LifeEvent::BuildFail);
    };
    let layout = render::layout(fonts, &req.items, snap.scale);
    if layout.selectable.is_empty() {
        tracing::debug!(target: "x11::menu", "run_menu: no selectable rows; dismissing");
        return close(&mut life, LifeEvent::BuildFail);
    }
    let (wx, wy) = place(&snap, req.x, req.y, layout.width, layout.height);
    tracing::debug!(
        target: "x11::menu",
        "run_menu: scale={:.2} parent=({},{}) root={}x{} menu={}x{} at=({},{})",
        snap.scale, snap.parent_x, snap.parent_y, snap.root_w, snap.root_h,
        layout.width, layout.height, wx, wy,
    );

    let Ok(win) = conn.generate_id() else {
        return close(&mut life, LifeEvent::BuildFail);
    };
    let aux = CreateWindowAux::new()
        .background_pixel(0)
        .border_pixel(0)
        .override_redirect(1)
        .event_mask(EventMask::EXPOSURE)
        .colormap(snap.colormap);
    if conn
        .create_window(
            snap.depth,
            win,
            snap.root,
            wx as i16,
            wy as i16,
            layout.width as u16,
            layout.height as u16,
            0,
            WindowClass::INPUT_OUTPUT,
            snap.visual,
            &aux,
        )
        .is_err()
    {
        tracing::error!(target: "x11::menu", "run_menu: create_window failed");
        return close(&mut life, LifeEvent::BuildFail);
    }
    tracing::debug!(target: "x11::menu", "run_menu: window 0x{win:x} created+mapped");

    let Ok(gc) = conn.generate_id() else {
        let _ = conn.destroy_window(win);
        return close(&mut life, LifeEvent::BuildFail);
    };
    let _ = conn.create_gc(gc, win, &CreateGCAux::new());
    let _ = conn.map_window(win);
    let _ = conn.configure_window(
        win,
        &x11rb::protocol::xproto::ConfigureWindowAux::new().stack_mode(StackMode::ABOVE),
    );
    // Round-trip on the grabbing connection before grabbing — the window must
    // be realized server-side or the grab races into a BadWindow.
    let _ = conn.get_geometry(win).ok().and_then(|c| c.reply().ok());

    let mut buf = ShmBuffer::empty();
    redraw(conn, win, gc, &mut buf, fonts, &layout, &req.items, -1);
    let _ = lifecycle_fsm::step(&mut life, &LifeEvent::BuildOk);

    if !grab(conn, win) {
        tracing::error!(target: "x11::menu", "run_menu: pointer grab failed; dismissing");
        let result = close(&mut life, LifeEvent::GrabFail);
        cleanup(conn, win, gc, &mut buf);
        return result;
    }
    let _ = lifecycle_fsm::step(&mut life, &LifeEvent::GrabOk);
    tracing::debug!(target: "x11::menu", "run_menu: grabbed; entering modal loop");

    let result = event_loop(conn, keymap, win, gc, &mut buf, fonts, &layout, &req.items);

    let _ = conn.ungrab_pointer(x11rb::CURRENT_TIME);
    let _ = conn.ungrab_keyboard(x11rb::CURRENT_TIME);
    let result = close(&mut life, LifeEvent::Result(result));
    cleanup(conn, win, gc, &mut buf);
    result
}

fn translate(keymap: &Keymap, ev: &Event) -> Option<MenuEvent> {
    match ev {
        Event::Expose(_) => Some(MenuEvent::Expose),
        Event::MotionNotify(e) => Some(MenuEvent::Motion {
            x: e.event_x as i32,
            y: e.event_y as i32,
        }),
        Event::ButtonPress(e) => Some(MenuEvent::Press {
            x: e.event_x as i32,
            y: e.event_y as i32,
        }),
        Event::KeyPress(e) => Some(MenuEvent::Key(keymap.lookup(e.detail))),
        _ => None,
    }
}

#[allow(clippy::too_many_arguments)]
fn event_loop(
    conn: &RustConnection,
    keymap: &Keymap,
    win: u32,
    gc: u32,
    buf: &mut ShmBuffer,
    fonts: &mut Fonts,
    layout: &Layout,
    items: &[MenuItem],
) -> i32 {
    let mut state = MenuState::default();
    loop {
        let Ok(ev) = conn.wait_for_event() else {
            return -1;
        };
        let Some(mev) = translate(keymap, &ev) else {
            continue;
        };
        for effect in interaction_fsm::step(&mut state, &mev, layout, items) {
            match effect {
                MenuEffect::Redraw => {
                    redraw(conn, win, gc, buf, fonts, layout, items, state.active);
                }
                MenuEffect::Close(id) => return id,
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn redraw(
    conn: &RustConnection,
    win: u32,
    gc: u32,
    buf: &mut ShmBuffer,
    fonts: &mut Fonts,
    layout: &Layout,
    items: &[MenuItem],
    active: i32,
) {
    let Some(pm) = render::paint(fonts, layout, items, active) else {
        return;
    };
    let w = layout.width;
    let h = layout.height;
    if !shm_alloc(buf, conn, w, h) {
        return;
    }
    // tiny-skia is premultiplied RGBA; the ARGB32 X visual wants premultiplied
    // BGRA, so swap R and B as we copy into the SHM segment.
    let src = pm.data();
    let dst = unsafe { std::slice::from_raw_parts_mut(buf.data, (w * h * 4) as usize) };
    let mut i = 0;
    while i + 3 < src.len() && i + 3 < dst.len() {
        dst[i] = src[i + 2];
        dst[i + 1] = src[i + 1];
        dst[i + 2] = src[i];
        dst[i + 3] = src[i + 3];
        i += 4;
    }
    let _ = conn.shm_put_image(
        win,
        gc,
        w as u16,
        h as u16,
        0,
        0,
        w as u16,
        h as u16,
        0,
        0,
        32,
        u8::from(ImageFormat::Z_PIXMAP),
        false,
        buf.seg,
        0,
    );
    let _ = conn.flush();
}

fn grab(conn: &RustConnection, win: u32) -> bool {
    let mut got_pointer = false;
    for _ in 0..40 {
        let ok = conn
            .grab_pointer(
                false,
                win,
                EventMask::BUTTON_PRESS | EventMask::BUTTON_RELEASE | EventMask::POINTER_MOTION,
                GrabMode::ASYNC,
                GrabMode::ASYNC,
                x11rb::NONE,
                x11rb::NONE,
                x11rb::CURRENT_TIME,
            )
            .ok()
            .and_then(|c| c.reply().ok())
            .map(|r| r.status == GrabStatus::SUCCESS)
            .unwrap_or(false);
        if ok {
            got_pointer = true;
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    if !got_pointer {
        return false;
    }
    let _ = conn
        .grab_keyboard(
            false,
            win,
            x11rb::CURRENT_TIME,
            GrabMode::ASYNC,
            GrabMode::ASYNC,
        )
        .ok()
        .and_then(|c| c.reply().ok());
    true
}

fn cleanup(conn: &RustConnection, win: u32, gc: u32, buf: &mut ShmBuffer) {
    shm_free(buf, Some(conn));
    let _ = conn.free_gc(gc);
    let _ = conn.destroy_window(win);
    let _ = conn.flush();
}

struct Keymap {
    min_keycode: u8,
    per: u8,
    syms: Vec<u32>,
}

impl Keymap {
    fn query(conn: &RustConnection) -> Self {
        let setup = conn.setup();
        let min = setup.min_keycode;
        let max = setup.max_keycode;
        let count = max - min + 1;
        let syms = conn
            .get_keyboard_mapping(min, count)
            .ok()
            .and_then(|c| c.reply().ok())
            .map(|r| (r.keysyms_per_keycode, r.keysyms))
            .unwrap_or((0, Vec::new()));
        Self {
            min_keycode: min,
            per: syms.0,
            syms: syms.1,
        }
    }

    fn lookup(&self, keycode: u8) -> u32 {
        if self.per == 0 || keycode < self.min_keycode {
            return 0;
        }
        let idx = (keycode - self.min_keycode) as usize * self.per as usize;
        self.syms.get(idx).copied().unwrap_or(0)
    }
}
