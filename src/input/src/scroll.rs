//! Coalescing scroll accumulator.
//!
//! macOS delivers high-frequency wheel/trackpad deltas; firing a CEF scroll
//! event per `scrollWheel:` floods the renderer. This batches deltas and
//! drains an integer chunk per runloop flush, carrying the fractional
//! remainder. It's a pure state machine — the platform keeps the `Mutex`
//! and the main-queue scheduling, and just feeds events in and pushes the
//! drained chunk out. The fixed-point drain math is subtle, so it lives
//! here where it can be unit-tested on any host.

/// Cocoa non-precise `scrollWheel:` reports line deltas; Chromium maps one
/// scroll line to 40 CSS pixels.
const PIXELS_PER_TICK: f32 = 40.0;

/// Fraction of the pending non-precise delta drained per flush — smooths a
/// burst of line scrolls into several frames.
const DRAIN: f32 = 0.45;

/// An integer scroll chunk to forward to CEF. Mirrors the arguments of
/// `jfn_input_dispatch_scroll_precise`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ScrollFlush {
    pub x: i32,
    pub y: i32,
    pub dx: i32,
    pub dy: i32,
    pub mods: u32,
    pub precise: bool,
}

/// Accumulated scroll state. Construct with [`ScrollAccum::new`], feed
/// [`accumulate`], drain with [`flush`].
///
/// [`accumulate`]: ScrollAccum::accumulate
/// [`flush`]: ScrollAccum::flush
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct ScrollAccum {
    ax: f32,
    ay: f32,
    x: i32,
    y: i32,
    mods: u32,
    precise: bool,
    pending: bool,
    flush_scheduled: bool,
}

impl ScrollAccum {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            ax: 0.0,
            ay: 0.0,
            x: 0,
            y: 0,
            mods: 0,
            precise: false,
            pending: false,
            flush_scheduled: false,
        }
    }

    /// Accumulate one wheel/trackpad event. `dx`/`dy` are the raw deltas
    /// (`scrollingDeltaX/Y` when `precise`, the `deltaX/Y` line counts
    /// otherwise — line counts are scaled to pixels here). Returns `true`
    /// if a flush needs scheduling (i.e. one wasn't already pending), so
    /// the caller schedules exactly one main-queue drain per burst.
    pub fn accumulate(
        &mut self,
        x: i32,
        y: i32,
        mods: u32,
        precise: bool,
        dx: f32,
        dy: f32,
    ) -> bool {
        self.x = x;
        self.y = y;
        self.mods = mods;
        self.precise = precise;
        if precise {
            self.ax += dx;
            self.ay += dy;
        } else {
            self.ax += dx * PIXELS_PER_TICK;
            self.ay += dy * PIXELS_PER_TICK;
        }
        self.pending = true;
        if self.flush_scheduled {
            false
        } else {
            self.flush_scheduled = true;
            true
        }
    }

    /// Drain one flush worth of integer deltas, carrying the remainder.
    /// Returns `None` when there's nothing pending or the drained chunk
    /// rounds to zero this cycle. Clears the "flush scheduled" latch, so a
    /// stuck sub-integer remainder waits for the next [`accumulate`] to
    /// reschedule.
    ///
    /// [`accumulate`]: ScrollAccum::accumulate
    pub fn flush(&mut self) -> Option<ScrollFlush> {
        self.flush_scheduled = false;
        if !self.pending {
            return None;
        }
        let mut dx: i32;
        let mut dy: i32;
        if self.precise {
            dx = self.ax.round() as i32;
            dy = self.ay.round() as i32;
            self.ax -= dx as f32;
            self.ay -= dy as f32;
        } else {
            dx = (self.ax * DRAIN).round() as i32;
            dy = (self.ay * DRAIN).round() as i32;
            // Drain at least one unit once a full line has accumulated, so a
            // slow scroll still moves instead of stalling under the round.
            if dx == 0 && self.ax.abs() >= 1.0 {
                dx = if self.ax > 0.0 { 1 } else { -1 };
            }
            if dy == 0 && self.ay.abs() >= 1.0 {
                dy = if self.ay > 0.0 { 1 } else { -1 };
            }
            self.ax -= dx as f32;
            self.ay -= dy as f32;
            // Snap a tiny residual to zero so it doesn't linger as pending.
            if self.ax.abs() < 0.5 {
                self.ax = 0.0;
            }
            if self.ay.abs() < 0.5 {
                self.ay = 0.0;
            }
        }
        self.pending = self.ax != 0.0 || self.ay != 0.0;
        if dx == 0 && dy == 0 {
            return None;
        }
        Some(ScrollFlush {
            x: self.x,
            y: self.y,
            dx,
            dy,
            mods: self.mods,
            precise: self.precise,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn precise_integer_deltas_pass_through() {
        let mut s = ScrollAccum::new();
        s.accumulate(10, 20, 0, true, 3.0, -5.0);
        let f = s.flush().unwrap();
        assert_eq!((f.dx, f.dy), (3, -5));
        assert_eq!((f.x, f.y), (10, 20));
        assert!(f.precise);
        // Nothing left pending.
        assert_eq!(s.flush(), None);
    }

    #[test]
    fn precise_carries_fractional_remainder() {
        let mut s = ScrollAccum::new();
        // 0.6 rounds to 1, leaving -0.4; 0.4 rounds to 0, leaving 0.4.
        s.accumulate(0, 0, 0, true, 0.4, 0.6);
        let f = s.flush().unwrap();
        assert_eq!((f.dx, f.dy), (0, 1));
        // Remainder still pending (ax = 0.4).
        let again = s.flush();
        // 0.4 alone still rounds to 0 → nothing emitted, but flush was
        // descheduled.
        assert_eq!(again, None);
    }

    #[test]
    fn nonprecise_scales_and_drains() {
        let mut s = ScrollAccum::new();
        // One line down → ay = 40. First drain: round(40 * 0.45) = 18.
        s.accumulate(5, 6, 0, false, 0.0, 1.0);
        let f = s.flush().unwrap();
        assert_eq!(f.dy, 18);
        assert_eq!(f.dx, 0);
        // Remainder (22) is still pending for the next scheduled flush.
        let f2 = s.flush().unwrap();
        assert_eq!(f2.dy, (22.0_f32 * DRAIN).round() as i32);
    }

    #[test]
    fn nonprecise_nudges_at_least_one_unit() {
        let mut s = ScrollAccum::new();
        // Tiny line delta: ay ends just above 1.0 so round(ay*0.45)=0 but the
        // nudge forces a single unit.
        s.accumulate(0, 0, 0, false, 0.0, 0.026); // ay ≈ 1.04
        let f = s.flush().unwrap();
        assert_eq!(f.dy, 1);
        // Residual snapped to zero → no longer pending.
        assert_eq!(s.flush(), None);
    }

    #[test]
    fn schedule_signal_fires_once_per_burst() {
        let mut s = ScrollAccum::new();
        assert!(s.accumulate(0, 0, 0, true, 1.0, 1.0)); // first → schedule
        assert!(!s.accumulate(0, 0, 0, true, 1.0, 1.0)); // already scheduled
        let _ = s.flush(); // clears the latch
        assert!(s.accumulate(0, 0, 0, true, 1.0, 1.0)); // schedules again
    }

    #[test]
    fn flush_with_nothing_pending_is_none() {
        let mut s = ScrollAccum::new();
        assert_eq!(s.flush(), None);
    }
}
