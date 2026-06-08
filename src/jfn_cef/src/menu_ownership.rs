//! Single-owner arbitration for the context menu: at most one session owns the
//! menu, and a superseded session's late result resolves to a no-op.

use crate::sink_routing::Handle;
use slotmap::SlotMap;

#[derive(Default)]
pub struct MenuOwnership {
    sessions: SlotMap<Handle, ()>,
    current: Option<Handle>,
}

impl MenuOwnership {
    pub fn open(&mut self) -> Option<Handle> {
        if self.current.is_some() {
            return None;
        }
        let h = self.sessions.insert(());
        self.current = Some(h);
        Some(h)
    }

    pub fn resolve(&mut self, h: Handle) -> bool {
        if self.current != Some(h) {
            return false;
        }
        self.current = None;
        self.sessions.remove(h);
        true
    }

    pub fn reset(&mut self) {
        self.current = None;
        self.sessions.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn open_from_idle_grants() {
        let mut m = MenuOwnership::default();
        assert!(m.open().is_some());
    }

    #[test]
    fn second_open_while_open_refused() {
        let mut m = MenuOwnership::default();
        let _ = m.open();
        assert!(m.open().is_none());
    }

    #[test]
    fn resolve_current_then_idle() {
        let mut m = MenuOwnership::default();
        let h = m.open().unwrap();
        assert!(m.resolve(h));
        assert!(m.open().is_some());
    }

    #[test]
    fn resolve_stale_is_noop_and_keeps_current() {
        let mut m = MenuOwnership::default();
        let h1 = m.open().unwrap();
        assert!(m.resolve(h1));
        let h2 = m.open().unwrap();
        assert_ne!(h1, h2);
        assert!(!m.resolve(h1));
        assert!(m.open().is_none());
        assert!(m.resolve(h2));
    }

    #[test]
    fn reset_clears() {
        let mut m = MenuOwnership::default();
        let _ = m.open();
        m.reset();
        assert!(m.open().is_some());
    }
}
