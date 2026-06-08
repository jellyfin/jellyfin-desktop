//! Pure decision logic for the context menu's modal loop.

use super::MenuItem;
use super::render::Layout;

pub const XK_ESCAPE: u32 = 0xff1b;
pub const XK_RETURN: u32 = 0xff0d;
pub const XK_KP_ENTER: u32 = 0xff8d;
pub const XK_TAB: u32 = 0xff09;
pub const XK_UP: u32 = 0xff52;
pub const XK_DOWN: u32 = 0xff54;
pub const XK_SPACE: u32 = 0x0020;

/// Highlighted row, as an item index; `-1` means nothing is hovered.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct MenuState {
    pub active: i32,
}

impl Default for MenuState {
    fn default() -> Self {
        Self { active: -1 }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MenuEvent {
    Expose,
    Motion { x: i32, y: i32 },
    Press { x: i32, y: i32 },
    Key(u32),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MenuEffect {
    Redraw,
    /// Command id of the chosen item, or `-1` to dismiss.
    Close(i32),
}

pub fn step(
    s: &mut MenuState,
    ev: &MenuEvent,
    layout: &Layout,
    items: &[MenuItem],
) -> Vec<MenuEffect> {
    match *ev {
        MenuEvent::Expose => vec![MenuEffect::Redraw],
        MenuEvent::Motion { x, y } => {
            let hit = layout.row_at(x, y).map_or(-1, |i| i as i32);
            if hit != s.active {
                s.active = hit;
                vec![MenuEffect::Redraw]
            } else {
                vec![]
            }
        }
        MenuEvent::Press { x, y } => {
            if !layout.contains(x, y) {
                return vec![MenuEffect::Close(-1)];
            }
            // In-bounds press on a separator/disabled row or the padding band is
            // ignored, not a dismiss.
            match layout.row_at(x, y) {
                Some(idx) => vec![MenuEffect::Close(items[idx].id)],
                None => vec![],
            }
        }
        MenuEvent::Key(keysym) => match keysym {
            XK_ESCAPE | XK_TAB => vec![MenuEffect::Close(-1)],
            XK_RETURN | XK_KP_ENTER | XK_SPACE => {
                if s.active >= 0 {
                    vec![MenuEffect::Close(items[s.active as usize].id)]
                } else {
                    vec![]
                }
            }
            XK_DOWN | XK_UP => {
                let next = layout.step(s.active, keysym == XK_DOWN);
                if next != s.active {
                    s.active = next;
                    vec![MenuEffect::Redraw]
                } else {
                    vec![]
                }
            }
            _ => vec![],
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::menu::render::Row;

    fn item(id: i32) -> MenuItem {
        MenuItem {
            id,
            label: String::new(),
            enabled: true,
            separator: false,
        }
    }

    fn sep() -> MenuItem {
        MenuItem {
            id: 0,
            label: String::new(),
            enabled: false,
            separator: true,
        }
    }

    fn disabled(id: i32) -> MenuItem {
        MenuItem {
            id,
            label: String::new(),
            enabled: false,
            separator: false,
        }
    }

    fn fixture() -> (Vec<MenuItem>, Layout) {
        let items = vec![item(10), sep(), item(20)];
        let rows = vec![
            Row {
                item: 0,
                y: 4,
                h: 10,
                separator: false,
                enabled: true,
            },
            Row {
                item: 1,
                y: 14,
                h: 6,
                separator: true,
                enabled: false,
            },
            Row {
                item: 2,
                y: 20,
                h: 10,
                separator: false,
                enabled: true,
            },
        ];
        let layout = Layout::for_test(100, 34, rows, vec![0, 2]);
        (items, layout)
    }

    fn run(active: i32, ev: MenuEvent) -> (i32, Vec<MenuEffect>) {
        let (items, layout) = fixture();
        let mut s = MenuState { active };
        let e = step(&mut s, &ev, &layout, &items);
        (s.active, e)
    }

    #[test]
    fn expose_always_redraws() {
        let (active, e) = run(2, MenuEvent::Expose);
        assert_eq!(active, 2);
        assert_eq!(e, vec![MenuEffect::Redraw]);
    }

    #[test]
    fn motion_into_row_sets_active() {
        let (active, e) = run(-1, MenuEvent::Motion { x: 50, y: 5 });
        assert_eq!(active, 0);
        assert_eq!(e, vec![MenuEffect::Redraw]);
    }

    #[test]
    fn motion_same_row_noop() {
        let (active, e) = run(0, MenuEvent::Motion { x: 50, y: 5 });
        assert_eq!(active, 0);
        assert_eq!(e, vec![]);
    }

    #[test]
    fn motion_onto_separator_clears() {
        let (active, e) = run(0, MenuEvent::Motion { x: 50, y: 15 });
        assert_eq!(active, -1);
        assert_eq!(e, vec![MenuEffect::Redraw]);
    }

    #[test]
    fn motion_outside_clears() {
        let (active, e) = run(0, MenuEvent::Motion { x: -5, y: -5 });
        assert_eq!(active, -1);
        assert_eq!(e, vec![MenuEffect::Redraw]);
    }

    #[test]
    fn press_outside_dismisses() {
        let (_, e) = run(0, MenuEvent::Press { x: 9999, y: 0 });
        assert_eq!(e, vec![MenuEffect::Close(-1)]);
    }

    #[test]
    fn press_on_row_closes_with_id() {
        let (_, e) = run(-1, MenuEvent::Press { x: 50, y: 25 });
        assert_eq!(e, vec![MenuEffect::Close(20)]);
    }

    #[test]
    fn press_on_separator_ignored() {
        let (_, e) = run(0, MenuEvent::Press { x: 50, y: 15 });
        assert_eq!(e, vec![]);
    }

    #[test]
    fn press_in_top_padding_ignored() {
        let (_, e) = run(0, MenuEvent::Press { x: 50, y: 1 });
        assert_eq!(e, vec![]);
    }

    #[test]
    fn press_on_disabled_ignored() {
        let items = vec![disabled(10)];
        let rows = vec![Row {
            item: 0,
            y: 4,
            h: 10,
            separator: false,
            enabled: false,
        }];
        let layout = Layout::for_test(100, 18, rows, vec![]);
        let mut s = MenuState { active: -1 };
        let e = step(&mut s, &MenuEvent::Press { x: 50, y: 5 }, &layout, &items);
        assert_eq!(e, vec![]);
    }

    #[test]
    fn key_escape_and_tab_dismiss() {
        assert_eq!(
            run(2, MenuEvent::Key(XK_ESCAPE)).1,
            vec![MenuEffect::Close(-1)]
        );
        assert_eq!(
            run(2, MenuEvent::Key(XK_TAB)).1,
            vec![MenuEffect::Close(-1)]
        );
    }

    #[test]
    fn key_select_with_active() {
        for k in [XK_RETURN, XK_KP_ENTER, XK_SPACE] {
            assert_eq!(run(2, MenuEvent::Key(k)).1, vec![MenuEffect::Close(20)]);
        }
    }

    #[test]
    fn key_select_no_active_noop() {
        assert_eq!(run(-1, MenuEvent::Key(XK_RETURN)).1, vec![]);
    }

    #[test]
    fn key_down_from_none_first() {
        let (active, e) = run(-1, MenuEvent::Key(XK_DOWN));
        assert_eq!(active, 0);
        assert_eq!(e, vec![MenuEffect::Redraw]);
    }

    #[test]
    fn key_up_from_none_last() {
        let (active, e) = run(-1, MenuEvent::Key(XK_UP));
        assert_eq!(active, 2);
        assert_eq!(e, vec![MenuEffect::Redraw]);
    }

    #[test]
    fn key_down_wraps() {
        let (active, e) = run(2, MenuEvent::Key(XK_DOWN));
        assert_eq!(active, 0);
        assert_eq!(e, vec![MenuEffect::Redraw]);
    }

    #[test]
    fn key_down_single_item_noop() {
        let items = vec![item(10)];
        let rows = vec![Row {
            item: 0,
            y: 4,
            h: 10,
            separator: false,
            enabled: true,
        }];
        let layout = Layout::for_test(100, 18, rows, vec![0]);
        let mut s = MenuState { active: 0 };
        let e = step(&mut s, &MenuEvent::Key(XK_DOWN), &layout, &items);
        assert_eq!(s.active, 0);
        assert_eq!(e, vec![]);
    }

    #[test]
    fn key_unknown_noop() {
        assert_eq!(run(1, MenuEvent::Key(0xffff)).1, vec![]);
    }
}
