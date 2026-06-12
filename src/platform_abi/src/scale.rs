//! Single source of truth for the display scale.
//!
//! The platform backends are the exclusive producers: each pushes its boot
//! probe via [`scale_push_boot`] and runtime observations via [`scale_push`].

use std::sync::OnceLock;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::geometry::validate_scale_factor;

struct Store {
    runtime: AtomicU64,
    boot: AtomicU64,
}

impl Store {
    const fn new() -> Self {
        Self {
            runtime: AtomicU64::new(0),
            boot: AtomicU64::new(0),
        }
    }

    fn load(slot: &AtomicU64) -> Option<f64> {
        let bits = slot.load(Ordering::Acquire);
        (bits != 0).then(|| f64::from_bits(bits))
    }

    fn push(&self, s: f64) -> bool {
        if !validate_scale_factor(s) {
            return false;
        }
        let prev = self.get();
        self.runtime.store(s.to_bits(), Ordering::Release);
        prev != Some(s)
    }

    fn push_boot(&self, s: f64) {
        if !validate_scale_factor(s) {
            return;
        }
        self.boot.store(s.to_bits(), Ordering::Release);
    }

    fn get(&self) -> Option<f64> {
        Self::load(&self.runtime).or_else(|| Self::load(&self.boot))
    }

    fn runtime_known(&self) -> bool {
        Self::load(&self.runtime).is_some()
    }
}

static STORE: Store = Store::new();

type ChangedHandler = Box<dyn Fn(f64) + Send + Sync + 'static>;

fn changed_slot() -> &'static parking_lot::Mutex<Option<ChangedHandler>> {
    static SLOT: OnceLock<parking_lot::Mutex<Option<ChangedHandler>>> = OnceLock::new();
    SLOT.get_or_init(|| parking_lot::Mutex::new(None))
}

/// Invalid values are ignored; the changed handler fires only when the
/// effective scale changes.
pub fn scale_push(s: f64) {
    if STORE.push(s)
        && let Some(cb) = changed_slot().lock().as_ref()
    {
        cb(s);
    }
}

pub fn scale_push_boot(s: f64) {
    STORE.push_boot(s);
}

/// Runtime value, else boot value, else `None`.
pub fn scale_get() -> Option<f64> {
    STORE.get()
}

pub fn scale_get_or_one() -> f64 {
    STORE.get().unwrap_or(1.0)
}

pub fn scale_runtime_known() -> bool {
    STORE.runtime_known()
}

pub fn jfn_scale_set_changed_handler<F: Fn(f64) + Send + Sync + 'static>(cb: F) {
    *changed_slot().lock() = Some(Box::new(cb));
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::{LogicalSize, PhysicalSize};

    #[test]
    fn unknown_until_pushed() {
        let s = Store::new();
        assert_eq!(s.get(), None);
        assert!(!s.runtime_known());
        assert_eq!(s.get().unwrap_or(1.0), 1.0);
    }

    #[test]
    fn invalid_pushes_ignored() {
        let s = Store::new();
        assert!(!s.push(0.0));
        assert!(!s.push(-1.5));
        assert!(!s.push(f64::NAN));
        s.push_boot(0.0);
        assert_eq!(s.get(), None);
    }

    #[test]
    fn runtime_wins_over_boot() {
        let s = Store::new();
        s.push_boot(1.25);
        assert_eq!(s.get(), Some(1.25));
        assert!(!s.runtime_known());
        assert!(s.push(1.5));
        assert_eq!(s.get(), Some(1.5));
        assert!(s.runtime_known());
    }

    #[test]
    fn push_reports_effective_change_only() {
        let s = Store::new();
        s.push_boot(1.5);
        assert!(!s.push(1.5));
        assert!(s.push(2.0));
        assert!(!s.push(2.0));
    }

    #[test]
    fn round_trips_through_dpi_conversions() {
        for (scale, logical, physical) in [
            (
                1.25,
                LogicalSize::new(1280, 720),
                PhysicalSize::new(1600, 900),
            ),
            (
                1.5,
                LogicalSize::new(1600, 900),
                PhysicalSize::new(2400, 1350),
            ),
            (
                2.0,
                LogicalSize::new(1280, 720),
                PhysicalSize::new(2560, 1440),
            ),
        ] {
            let s = Store::new();
            s.push(scale);
            let got = s.get().unwrap_or(1.0);
            assert_eq!(logical.to_physical::<i32>(got), physical);
            assert_eq!(physical.to_logical::<i32>(got), logical);
        }
    }
}
