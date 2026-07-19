//! Level-triggered CEF sizing: layer size is a function of the current
//! window snapshot, pulled on each platform-abi window wakeup.

use jfn_platform_abi::{LogicalSize, PhysicalSize, WindowSnapshot};
use parking_lot::Mutex;

/// The size handed to CEF, in both coordinate spaces.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct CefViewSize {
    pub logical: LogicalSize,
    pub physical: PhysicalSize,
}

/// What CEF should be sized to for a given snapshot; `None` when the
/// extent is unknown or degenerate.
pub(crate) fn cef_size_from_snapshot(snap: &WindowSnapshot) -> Option<CefViewSize> {
    let extent = snap.extent?;
    let logical = extent.logical();
    let physical = extent.physical();
    if logical.w <= 0 || logical.h <= 0 || physical.w <= 0 || physical.h <= 0 {
        return None;
    }
    Some(CefViewSize { logical, physical })
}

/// Guards snapshot→apply: taking the snapshot outside this lock would let
/// an older snapshot apply after a newer one.
static LAST_APPLIED: Mutex<Option<CefViewSize>> = Mutex::new(None);

/// Pull the current window snapshot and size the CEF layers from it.
/// Callable from any thread, any number of times.
pub(crate) fn sync_from_window() {
    let mut last = LAST_APPLIED.lock();
    let snap = jfn_platform_abi::get().window_source().snapshot();

    if let Some(size) = cef_size_from_snapshot(&snap)
        && *last != Some(size)
    {
        jfn_logging::log(
            jfn_logging::CATEGORY_CEF,
            jfn_logging::LEVEL_DEBUG,
            &format!(
                "window sync: logical={}x{} physical={}x{}",
                size.logical.w, size.logical.h, size.physical.w, size.physical.h
            ),
        );
        crate::browsers::jfn_browsers_set_size(
            size.logical.w,
            size.logical.h,
            size.physical.w,
            size.physical.h,
        );
        *last = Some(size);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use jfn_platform_abi::{Scale, WindowExtent, WindowSnapshot};

    fn snap(extent: Option<WindowExtent>) -> WindowSnapshot {
        WindowSnapshot {
            extent,
            position: None,
            maximized: false,
            fullscreen: false,
        }
    }

    #[test]
    fn exact_logical_wins_over_division() {
        // 1497 / 2.5 rounds to 599 — the compositor's exact 598 must win
        // over re-derivation.
        let extent = WindowExtent::with_logical(
            PhysicalSize { w: 1497, h: 843 },
            Scale(2.5),
            LogicalSize { w: 598, h: 337 },
        );
        let size = cef_size_from_snapshot(&snap(Some(extent)));
        let Some(size) = size else {
            panic!("expected size");
        };
        assert_eq!(size.logical, LogicalSize { w: 598, h: 337 });
        assert_eq!(size.physical, PhysicalSize { w: 1497, h: 843 });
    }

    #[test]
    fn derived_logical_divides_by_extent_scale() {
        let extent = WindowExtent::new(PhysicalSize { w: 1196, h: 636 }, Scale(2.0));
        let Some(size) = cef_size_from_snapshot(&snap(Some(extent))) else {
            panic!("expected size");
        };
        assert_eq!(size.logical, LogicalSize { w: 598, h: 318 });
    }

    #[test]
    fn missing_or_degenerate_extent_is_none() {
        assert!(cef_size_from_snapshot(&snap(None)).is_none());
        let zero = WindowExtent::new(PhysicalSize { w: 0, h: 720 }, Scale(1.0));
        assert!(cef_size_from_snapshot(&snap(Some(zero))).is_none());
    }

    /// No matter which wakeups are dropped, the final applied size equals
    /// the size derived from the current snapshot.
    #[test]
    fn applied_size_matches_source_regardless_of_dropped_wakeups() {
        let extents = [
            WindowExtent::new(PhysicalSize { w: 1280, h: 720 }, Scale(2.0)),
            WindowExtent::new(PhysicalSize { w: 1196, h: 636 }, Scale(2.0)),
            WindowExtent::new(PhysicalSize { w: 1196, h: 636 }, Scale(1.5)),
            WindowExtent::new(PhysicalSize { w: 2400, h: 1350 }, Scale(1.5)),
        ];
        let n = extents.len() as u32;
        // Each bit decides whether the wakeup after mutation i is dropped.
        for drop_mask in 0..(1u32 << n) {
            let mut last_applied: Option<CefViewSize> = None;
            let mut applied_log = Vec::new();
            let mut current = None;
            for (i, extent) in extents.iter().enumerate() {
                current = Some(*extent);
                let dropped = drop_mask & (1 << i) != 0;
                if !dropped {
                    // A wakeup pulls the current snapshot, not the
                    // mutation that triggered it.
                    if let Some(size) = cef_size_from_snapshot(&snap(current))
                        && last_applied != Some(size)
                    {
                        applied_log.push(size);
                        last_applied = Some(size);
                    }
                }
            }
            // The attach-time reconcile repairs whatever the dropped
            // wakeups missed.
            if let Some(size) = cef_size_from_snapshot(&snap(current))
                && last_applied != Some(size)
            {
                applied_log.push(size);
                last_applied = Some(size);
            }
            assert_eq!(last_applied, cef_size_from_snapshot(&snap(current)));
            for pair in applied_log.windows(2) {
                assert_ne!(pair[0], pair[1]);
            }
        }
    }
}
