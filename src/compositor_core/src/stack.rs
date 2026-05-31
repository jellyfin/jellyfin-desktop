//! Per-surface registry: the bottom-to-top stacking order plus
//! main-surface tracking, shared by both compositors.
//!
//! Generic over the handle type `T` (the backend's opaque `*mut Surface`)
//! so the OS layer keeps its own pointer type and does its own
//! reparenting/visual work — this struct only does the bookkeeping. `T` is
//! `Copy + PartialEq` (raw pointers qualify); equality is by value/identity.
//!
//! The two backends model the registry slightly differently, and this type
//! is the faithful superset:
//! - **macOS** has no separate "live" list — the stack *is* the registry
//!   and the main surface is `stack.first()`. It uses [`replace_stack`],
//!   [`remove`], [`is_main`], [`stack`], [`take_stack`].
//! - **Windows** tracks `live` (all allocated), `stack` (currently
//!   parented), and an explicit `main` with a live fallback. It uses
//!   [`add_live`], [`remove`], [`clear_stack`] + [`push_stack`] +
//!   [`set_main_to_stack_first`] (its restack interleaves GPU calls),
//!   [`is_main`], [`take_live`].
//!
//! [`replace_stack`]: SurfaceStack::replace_stack
//! [`remove`]: SurfaceStack::remove
//! [`is_main`]: SurfaceStack::is_main
//! [`stack`]: SurfaceStack::stack
//! [`take_stack`]: SurfaceStack::take_stack
//! [`add_live`]: SurfaceStack::add_live
//! [`clear_stack`]: SurfaceStack::clear_stack
//! [`push_stack`]: SurfaceStack::push_stack
//! [`set_main_to_stack_first`]: SurfaceStack::set_main_to_stack_first
//! [`take_live`]: SurfaceStack::take_live

/// Bottom-to-top surface registry with main-surface tracking.
#[derive(Debug, Clone)]
pub struct SurfaceStack<T: Copy + PartialEq> {
    /// All allocated surfaces (Windows). macOS never populates this.
    live: Vec<T>,
    /// Current bottom-to-top stacking order.
    stack: Vec<T>,
    /// The "main" (bottom-most / mpv) surface that transition gating keys
    /// off. macOS keeps it equal to `stack.first()`; Windows stores it with
    /// a `live` fallback.
    main: Option<T>,
}

impl<T: Copy + PartialEq> SurfaceStack<T> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            live: Vec::new(),
            stack: Vec::new(),
            main: None,
        }
    }

    /// Windows alloc: register a newly allocated surface. The first
    /// allocated surface becomes main until a restack overrides it.
    pub fn add_live(&mut self, h: T) {
        self.live.push(h);
        if self.main.is_none() {
            self.main = Some(h);
        }
    }

    /// Remove a freed surface from both lists. If it was main, re-derive
    /// main as `stack.first()` then `live.first()` then `None` — matching
    /// Windows `win_free_surface`. (macOS keeps `live` empty, so this
    /// degrades to `stack.first()`, preserving `main == stack.first()`.)
    pub fn remove(&mut self, h: T) {
        self.live.retain(|&x| x != h);
        self.stack.retain(|&x| x != h);
        if self.main == Some(h) {
            self.main = self
                .stack
                .first()
                .copied()
                .or_else(|| self.live.first().copied());
        }
    }

    /// macOS restack: replace the entire stacking order and set main to the
    /// new bottom (`None` if empty).
    pub fn replace_stack(&mut self, ordered: &[T]) {
        self.stack.clear();
        self.stack.extend_from_slice(ordered);
        self.main = self.stack.first().copied();
    }

    /// Windows restack step: clear the stack before rebuilding it.
    pub fn clear_stack(&mut self) {
        self.stack.clear();
    }

    /// Windows restack step: append a surface that was successfully
    /// re-parented into the visual tree.
    pub fn push_stack(&mut self, h: T) {
        self.stack.push(h);
    }

    /// Windows restack step: set main to the bottom of the rebuilt stack,
    /// leaving it unchanged if the stack is empty (mirrors the original
    /// `if let Some(first) = stack.first()` guard).
    pub fn set_main_to_stack_first(&mut self) {
        if let Some(&first) = self.stack.first() {
            self.main = Some(first);
        }
    }

    #[must_use]
    pub fn is_main(&self, h: T) -> bool {
        self.main == Some(h)
    }

    #[must_use]
    pub fn main(&self) -> Option<T> {
        self.main
    }

    #[must_use]
    pub fn stack(&self) -> &[T] {
        &self.stack
    }

    #[must_use]
    pub fn live(&self) -> &[T] {
        &self.live
    }

    /// Windows cleanup: drain the live list (to free each surface), and
    /// reset the stack + main.
    pub fn take_live(&mut self) -> Vec<T> {
        self.stack.clear();
        self.main = None;
        std::mem::take(&mut self.live)
    }

    /// macOS cleanup: drain the stack (to detach each subview) and reset
    /// main.
    pub fn take_stack(&mut self) -> Vec<T> {
        self.main = None;
        std::mem::take(&mut self.stack)
    }
}

impl<T: Copy + PartialEq> Default for SurfaceStack<T> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Use distinct integers as stand-in surface handles.
    fn h(n: usize) -> usize {
        n
    }

    #[test]
    fn empty_has_no_main() {
        let s: SurfaceStack<usize> = SurfaceStack::new();
        assert_eq!(s.main(), None);
        assert!(!s.is_main(h(1)));
        assert!(s.stack().is_empty());
    }

    // ---- Windows model ----------------------------------------------

    #[test]
    fn windows_first_alloc_becomes_main() {
        let mut s = SurfaceStack::new();
        s.add_live(h(1));
        assert!(s.is_main(h(1)));
        s.add_live(h(2));
        // Second alloc does not steal main.
        assert!(s.is_main(h(1)));
        assert!(!s.is_main(h(2)));
    }

    #[test]
    fn windows_restack_sets_main_to_bottom() {
        let mut s = SurfaceStack::new();
        s.add_live(h(1));
        s.add_live(h(2));
        s.clear_stack();
        s.push_stack(h(2)); // bottom
        s.push_stack(h(1)); // top
        s.set_main_to_stack_first();
        assert!(s.is_main(h(2)));
        assert_eq!(s.stack(), &[h(2), h(1)]);
    }

    #[test]
    fn windows_restack_empty_leaves_main_unchanged() {
        let mut s = SurfaceStack::new();
        s.add_live(h(1));
        s.clear_stack();
        s.set_main_to_stack_first(); // no-op, stack empty
        assert!(s.is_main(h(1)));
    }

    #[test]
    fn windows_free_main_rederives_from_stack_then_live() {
        let mut s = SurfaceStack::new();
        s.add_live(h(1));
        s.add_live(h(2));
        s.clear_stack();
        s.push_stack(h(1));
        s.push_stack(h(2));
        s.set_main_to_stack_first(); // main = 1
        assert!(s.is_main(h(1)));

        // Free main → re-derive to next stack entry.
        s.remove(h(1));
        assert!(s.is_main(h(2)));

        // Free the last stacked surface → fall back to live.first().
        s.remove(h(2));
        // h(2) was also removed from live; only nothing remains.
        assert_eq!(s.main(), None);
    }

    #[test]
    fn windows_free_main_falls_back_to_live() {
        let mut s = SurfaceStack::new();
        s.add_live(h(1)); // main
        s.add_live(h(2)); // live only, never stacked
        // No restack: stack is empty, main = 1.
        s.remove(h(1));
        // stack.first() is None → live.first() == 2.
        assert!(s.is_main(h(2)));
    }

    #[test]
    fn windows_free_non_main_keeps_main() {
        let mut s = SurfaceStack::new();
        s.add_live(h(1));
        s.add_live(h(2));
        s.clear_stack();
        s.push_stack(h(1));
        s.push_stack(h(2));
        s.set_main_to_stack_first(); // main = 1
        s.remove(h(2));
        assert!(s.is_main(h(1)));
        assert_eq!(s.stack(), &[h(1)]);
    }

    #[test]
    fn windows_take_live_resets() {
        let mut s = SurfaceStack::new();
        s.add_live(h(1));
        s.add_live(h(2));
        s.clear_stack();
        s.push_stack(h(1));
        s.set_main_to_stack_first();
        let drained = s.take_live();
        assert_eq!(drained, vec![h(1), h(2)]);
        assert_eq!(s.main(), None);
        assert!(s.stack().is_empty());
    }

    // ---- macOS model ------------------------------------------------

    #[test]
    fn macos_replace_stack_tracks_first_as_main() {
        let mut s = SurfaceStack::new();
        s.replace_stack(&[h(10), h(11), h(12)]);
        assert!(s.is_main(h(10)));
        assert_eq!(s.stack(), &[h(10), h(11), h(12)]);

        // Restack to a new order updates main to the new bottom.
        s.replace_stack(&[h(11), h(12)]);
        assert!(s.is_main(h(11)));
    }

    #[test]
    fn macos_replace_stack_empty_clears_main() {
        let mut s = SurfaceStack::new();
        s.replace_stack(&[h(10)]);
        s.replace_stack(&[]);
        assert_eq!(s.main(), None);
    }

    #[test]
    fn macos_remove_keeps_main_equal_to_stack_first() {
        let mut s = SurfaceStack::new();
        s.replace_stack(&[h(10), h(11), h(12)]);
        // Remove a non-first surface: main (stack.first) unchanged.
        s.remove(h(12));
        assert!(s.is_main(h(10)));
        // Remove the main: re-derive to new first (live is empty on macOS).
        s.remove(h(10));
        assert!(s.is_main(h(11)));
        assert_eq!(s.stack(), &[h(11)]);
    }

    #[test]
    fn macos_take_stack_resets() {
        let mut s = SurfaceStack::new();
        s.replace_stack(&[h(10), h(11)]);
        let drained = s.take_stack();
        assert_eq!(drained, vec![h(10), h(11)]);
        assert_eq!(s.main(), None);
    }
}
