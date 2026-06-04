//! X11 display scale probe used before mpv creates its window.
//!
//! mpv's `--geometry` is specified in physical X11 pixels, while Jellyfin
//! stores/restores window size in logical pixels. At startup this backend must
//! therefore predict the same X11 HiDPI scale mpv will later expose through
//! `display-hidpi-scale`, so this mirrors mpv's logic in
//! `third_party/mpv/video/out/x11_common.c` — including the half-step
//! quantization the tests pin.

use x11rb::connection::Connection;
use x11rb::resource_manager::new_from_resource_manager;
use x11rb::rust_connection::RustConnection;

const BASE_DPI: f64 = 96.0;

pub(crate) fn query_display_scale() -> Option<f32> {
    let (conn, screen_num) = RustConnection::connect(None).ok()?;
    if let Some(scale) = query_xft_dpi_scale(&conn) {
        tracing::debug!(target: "x11::scale", "Using Xft.dpi scale: {scale}");
        return Some(scale);
    }
    if let Some(scale) = query_screen_dpi_scale(&conn, screen_num) {
        tracing::debug!(target: "x11::scale", "Using X11 screen DPI scale: {scale}");
        return Some(scale);
    }
    None
}

fn query_xft_dpi_scale(conn: &impl Connection) -> Option<f32> {
    let db = new_from_resource_manager(conn).ok().flatten()?;
    let value: i64 = db.get_value("Xft.dpi", "").ok().flatten()?;
    quantize_dpi(value as f64)
}

fn query_screen_dpi_scale(conn: &impl Connection, screen_num: usize) -> Option<f32> {
    let screen = conn.setup().roots.get(screen_num)?;
    let w_mm = screen.width_in_millimeters;
    let h_mm = screen.height_in_millimeters;
    if w_mm == 0 || h_mm == 0 {
        return None;
    }

    let dpi_x = screen.width_in_pixels as f64 * 25.4 / f64::from(w_mm);
    let dpi_y = screen.height_in_pixels as f64 * 25.4 / f64::from(h_mm);
    if !dpi_x.is_finite() || !dpi_y.is_finite() {
        return None;
    }

    let sx = quantize_dpi_steps(dpi_x)?;
    let sy = quantize_dpi_steps(dpi_y)?;
    if sx == sy {
        Some(sx as f32 / 2.0)
    } else {
        None
    }
}

fn quantize_dpi(dpi: f64) -> Option<f32> {
    let s = quantize_dpi_steps(dpi)?;
    Some(s as f32 / 2.0)
}

fn quantize_dpi_steps(dpi: f64) -> Option<i32> {
    if !dpi.is_finite() {
        return None;
    }
    let s = (2.0 * dpi / BASE_DPI).clamp(0.0, 20.0).round_ties_even() as i32;
    if s > 2 && s < 20 { Some(s) } else { None }
}

#[cfg(test)]
mod tests {
    use super::{quantize_dpi, quantize_dpi_steps};

    #[test]
    fn xft_dpi_uses_mpv_half_step_quantization() {
        assert_eq!(quantize_dpi(144.0), Some(1.5));
        assert_eq!(quantize_dpi(168.0), Some(2.0));
        assert_eq!(quantize_dpi(192.0), Some(2.0));
        assert_eq!(quantize_dpi(288.0), Some(3.0));
    }

    #[test]
    fn unscaled_or_invalid_dpi_is_ignored() {
        assert_eq!(quantize_dpi(96.0), None);
        assert_eq!(quantize_dpi(120.0), None);
        assert_eq!(quantize_dpi(0.0), None);
        assert_eq!(quantize_dpi(f64::NAN), None);
    }

    #[test]
    fn screen_dpi_fallback_requires_matching_axes() {
        assert_eq!(quantize_dpi_steps(144.0), Some(3));
        assert_ne!(quantize_dpi_steps(144.0), quantize_dpi_steps(192.0));
    }
}
