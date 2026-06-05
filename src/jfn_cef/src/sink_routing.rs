//! Exclusive sink routing: many producers feed one shared output, exactly one
//! producer reaches it at a time.
//!
//! Selection is bound at switch-time, not re-decided per event. A producer never
//! asks whether it is selected — it [`emit`](Router::emit)s unconditionally and
//! the [`Router`] forwards to the shared output only for the currently selected
//! producer; everyone else is silently dropped.
//!
//! Two producer flavors mirror the two kinds of stream:
//!
//! - **Stream** ([`Router::add_stream`]) — a stateless transient (keystrokes,
//!   pointer events). Drops while unselected, forwards while selected, retains
//!   nothing.
//! - **Level** ([`Router::add_level`]) — always has a current value (e.g. cursor
//!   shape). Records its latest value even while parked, and replays it once on
//!   selection so the output shows the right baseline immediately at the switch.
//!   State is *established on activate*; nothing relies on the outgoing producer
//!   to tidy up.
//!
//! Producer identities are generational [`Handle`]s (via `slotmap`): a handle to
//! a removed producer never aliases a later one, so a late `emit` from a
//! deselected or removed producer is a structural no-op.
//!
//! **Precondition:** all calls run on one serialized context. The API takes
//! `&mut self`, so the borrow checker already forbids concurrent mutation; a
//! multi-threaded integration wraps the whole `Router` behind a single lock
//! rather than synchronizing per producer.
//!
//! This is the general engine only — it is intentionally not yet wired to any
//! concrete consumer.

use slotmap::{SlotMap, new_key_type};

/// The shared output. The selected producer's values are forwarded here.
pub trait Sink<T> {
    fn emit(&mut self, value: T);
}

new_key_type! {
    /// Opaque, `Copy` producer identity. Generational: a handle to a removed
    /// producer never aliases a later one, so use-after-remove is a no-op.
    pub struct Handle;
}

enum Kind<T> {
    /// Stateless transient: forwards while selected, drops otherwise.
    Stream,
    /// Level: holds its latest value (`None` until first emit) and replays it on
    /// selection.
    Level(Option<T>),
}

/// Routes many producers to a single shared output `S`, with exactly one
/// producer selected at a time.
pub struct Router<T, S> {
    out: S,
    current: Option<Handle>,
    producers: SlotMap<Handle, Kind<T>>,
}

impl<T, S: Sink<T>> Router<T, S> {
    pub fn new(out: S) -> Self {
        Self {
            out,
            current: None,
            producers: SlotMap::with_key(),
        }
    }

    /// Register a stateless producer.
    pub fn add_stream(&mut self) -> Handle {
        self.producers.insert(Kind::Stream)
    }

    /// Register a level producer (records its latest value, replays on select).
    pub fn add_level(&mut self) -> Handle {
        self.producers.insert(Kind::Level(None))
    }

    /// Which producer currently reaches the output, if any.
    pub fn current(&self) -> Option<Handle> {
        self.current
    }

    pub fn out(&self) -> &S {
        &self.out
    }

    pub fn out_mut(&mut self) -> &mut S {
        &mut self.out
    }

    /// Make `next` the selected producer. A level replays its latest value once,
    /// establishing the output's baseline at the switch. A stale handle is
    /// ignored.
    pub fn select(&mut self, next: Handle)
    where
        T: Clone,
    {
        if !self.producers.contains_key(next) {
            return;
        }
        self.current = Some(next);
        if let Some(Kind::Level(Some(value))) = self.producers.get(next) {
            let value = value.clone();
            self.out.emit(value);
        }
    }

    /// Remove a producer. If it was selected, drop the selection and promote
    /// `fallback` (replaying its level baseline). Removing an unselected producer
    /// leaves the current selection untouched.
    pub fn remove(&mut self, gone: Handle, fallback: Option<Handle>)
    where
        T: Clone,
    {
        let was_current = self.current == Some(gone);
        self.producers.remove(gone);
        if was_current {
            self.current = None;
            if let Some(fb) = fallback {
                self.select(fb);
            }
        }
    }

    /// Emit a value from `who`. A level records it as its latest value (even while
    /// parked); the value reaches the output only if `who` is currently selected.
    /// A stale handle drops the value.
    pub fn emit(&mut self, who: Handle, value: T)
    where
        T: Clone,
    {
        let is_current = self.current == Some(who);
        if let Some(Kind::Level(last)) = self.producers.get_mut(who) {
            *last = Some(value.clone());
        }
        if is_current {
            self.out.emit(value);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Recorder(Vec<u32>);

    impl Sink<u32> for Recorder {
        fn emit(&mut self, value: u32) {
            self.0.push(value);
        }
    }

    fn router() -> Router<u32, Recorder> {
        Router::new(Recorder::default())
    }

    #[test]
    fn parked_stream_drops() {
        let mut r = router();
        let a = r.add_stream();
        for v in 0..5 {
            r.emit(a, v);
        }
        assert!(r.out().0.is_empty());
    }

    #[test]
    fn selected_stream_forwards() {
        let mut r = router();
        let a = r.add_stream();
        r.select(a);
        r.emit(a, 1);
        r.emit(a, 2);
        assert_eq!(r.out().0, vec![1, 2]);
    }

    #[test]
    fn deselected_stream_emit_drops() {
        let mut r = router();
        let a = r.add_stream();
        let b = r.add_stream();
        r.select(a);
        r.emit(a, 1);
        r.select(b);
        r.emit(a, 99);
        assert_eq!(r.out().0, vec![1]);
    }

    #[test]
    fn level_records_while_parked_and_replays_on_activate() {
        let mut r = router();
        let lvl = r.add_level();
        r.emit(lvl, 7);
        assert!(r.out().0.is_empty());
        r.select(lvl);
        assert_eq!(r.out().0, vec![7]);
    }

    #[test]
    fn selecting_stream_replays_nothing() {
        let mut r = router();
        let s = r.add_stream();
        r.emit(s, 1);
        r.select(s);
        assert!(r.out().0.is_empty());
    }

    #[test]
    fn selecting_valueless_level_replays_nothing() {
        let mut r = router();
        let lvl = r.add_level();
        r.select(lvl);
        assert!(r.out().0.is_empty());
        r.emit(lvl, 3);
        assert_eq!(r.out().0, vec![3]);
    }

    #[test]
    fn remove_current_promotes_fallback_and_replays_level() {
        let mut r = router();
        let a = r.add_stream();
        let b = r.add_level();
        r.emit(b, 42);
        r.select(a);
        r.emit(a, 1);
        r.remove(a, Some(b));
        assert_eq!(r.current(), Some(b));
        assert_eq!(r.out().0, vec![1, 42]);
    }

    #[test]
    fn remove_inactive_is_silent() {
        let mut r = router();
        let a = r.add_stream();
        let b = r.add_stream();
        r.select(a);
        r.emit(a, 1);
        r.remove(b, None);
        assert_eq!(r.current(), Some(a));
        assert_eq!(r.out().0, vec![1]);
    }

    #[test]
    fn use_after_remove_is_noop() {
        let mut r = router();
        let a = r.add_stream();
        r.select(a);
        r.remove(a, None);
        assert_eq!(r.current(), None);
        r.emit(a, 1);
        r.select(a);
        assert_eq!(r.current(), None);
        assert!(r.out().0.is_empty());
    }

    #[test]
    fn only_current_reaches_output() {
        let mut r = router();
        let a = r.add_stream();
        let b = r.add_stream();
        let c = r.add_level();
        r.select(a);
        r.emit(a, 1);
        r.emit(b, 100);
        r.emit(c, 200);
        r.select(b);
        r.emit(b, 2);
        r.emit(a, 100);
        r.select(c);
        r.emit(c, 3);
        assert_eq!(r.out().0, vec![1, 2, 200, 3]);
    }
}
