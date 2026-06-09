use cosmic_text::{Attrs, Buffer, Color, Family, FontSystem, Metrics, Shaping, SwashCache};
use tiny_skia::{Color as SkColor, FillRule, Paint, PathBuilder, Pixmap, Rect, Transform};

use crate::MenuItem;

// Logical (unscaled) metrics; multiplied by the display scale at layout time.
const FONT_PX: f32 = 13.0;
const ROW_H: f32 = 28.0;
const SEP_H: f32 = 9.0;
const PAD_X: f32 = 12.0;
const PAD_RIGHT: f32 = 24.0;
const PAD_Y: f32 = 4.0;
const MIN_W: f32 = 160.0;
const RADIUS: f32 = 4.0;

fn bg() -> SkColor {
    SkColor::from_rgba8(0x2b, 0x2b, 0x2b, 0xff)
}
fn border() -> SkColor {
    SkColor::from_rgba8(0x55, 0x55, 0x55, 0xff)
}
fn hover() -> SkColor {
    SkColor::from_rgba8(0x3d, 0x3d, 0x3d, 0xff)
}
fn sep() -> SkColor {
    SkColor::from_rgba8(0x44, 0x44, 0x44, 0xff)
}
const TEXT: Color = Color::rgb(0xe0, 0xe0, 0xe0);
const TEXT_DISABLED: Color = Color::rgb(0x66, 0x66, 0x66);

/// Geometry in physical pixels relative to the menu's top-left.
#[derive(Clone)]
pub struct Row {
    pub item: usize,
    pub y: i32,
    pub h: i32,
    pub separator: bool,
    pub enabled: bool,
}

#[derive(Clone)]
pub struct Layout {
    pub width: i32,
    pub height: i32,
    pub rows: Vec<Row>,
    pub selectable: Vec<usize>,
    scale: f32,
}

impl Layout {
    pub fn for_test(width: i32, height: i32, rows: Vec<Row>, selectable: Vec<usize>) -> Self {
        Self {
            width,
            height,
            rows,
            selectable,
            scale: 1.0,
        }
    }

    /// Backends divide physical dimensions by this to recover logical
    /// (compositor) sizes.
    pub fn scale(&self) -> f32 {
        self.scale
    }

    pub fn contains(&self, x: i32, y: i32) -> bool {
        x >= 0 && x < self.width && y >= 0 && y < self.height
    }

    pub fn row_at(&self, x: i32, y: i32) -> Option<usize> {
        if x < 0 || x >= self.width || y < 0 || y >= self.height {
            return None;
        }
        self.rows
            .iter()
            .find(|r| !r.separator && r.enabled && y >= r.y && y < r.y + r.h)
            .map(|r| r.item)
    }

    pub fn step(&self, active: i32, forward: bool) -> i32 {
        if self.selectable.is_empty() {
            return -1;
        }
        let pos = self.selectable.iter().position(|&i| i as i32 == active);
        let next = match pos {
            Some(p) if forward => (p + 1) % self.selectable.len(),
            Some(p) => (p + self.selectable.len() - 1) % self.selectable.len(),
            None if forward => 0,
            None => self.selectable.len() - 1,
        };
        self.selectable[next] as i32
    }
}

pub struct Fonts {
    system: FontSystem,
    cache: SwashCache,
}

impl Fonts {
    pub fn new() -> Self {
        Self {
            system: FontSystem::new(),
            cache: SwashCache::new(),
        }
    }

    fn shape(&mut self, text: &str, font_px: f32) -> Buffer {
        let mut buf = Buffer::new(&mut self.system, Metrics::new(font_px, font_px * 1.3));
        buf.set_size(&mut self.system, None, None);
        buf.set_text(
            &mut self.system,
            text,
            Attrs::new().family(Family::SansSerif),
            Shaping::Advanced,
        );
        buf.shape_until_scroll(&mut self.system, false);
        buf
    }

    fn text_width(&mut self, text: &str, font_px: f32) -> f32 {
        self.shape(text, font_px)
            .layout_runs()
            .map(|r| r.line_w)
            .fold(0.0_f32, f32::max)
    }
}

impl Default for Fonts {
    fn default() -> Self {
        Self::new()
    }
}

pub fn layout(fonts: &mut Fonts, items: &[MenuItem], scale: f32) -> Layout {
    let s = if scale > 0.0 { scale } else { 1.0 };
    let font_px = FONT_PX * s;
    let row_h = (ROW_H * s).round() as i32;
    let sep_h = (SEP_H * s).round() as i32;
    let pad_y = (PAD_Y * s).round() as i32;
    let text_w_budget = (PAD_X + PAD_RIGHT) * s;

    let mut max_text = MIN_W * s - text_w_budget;
    for it in items {
        if !it.separator {
            max_text = max_text.max(fonts.text_width(&it.label, font_px));
        }
    }
    let width = (max_text + text_w_budget).ceil() as i32;

    let mut rows = Vec::with_capacity(items.len());
    let mut selectable = Vec::new();
    let mut y = pad_y;
    for (i, it) in items.iter().enumerate() {
        let h = if it.separator { sep_h } else { row_h };
        rows.push(Row {
            item: i,
            y,
            h,
            separator: it.separator,
            enabled: it.enabled,
        });
        if !it.separator && it.enabled {
            selectable.push(i);
        }
        y += h;
    }
    let height = y + pad_y;

    Layout {
        width,
        height,
        rows,
        selectable,
        scale: s,
    }
}

pub fn paint(
    fonts: &mut Fonts,
    layout: &Layout,
    items: &[MenuItem],
    active: i32,
) -> Option<Pixmap> {
    let s = layout.scale;
    let mut pm = Pixmap::new(layout.width as u32, layout.height as u32)?;

    let w = layout.width as f32;
    let h = layout.height as f32;
    let radius = RADIUS * s;

    if let Some(path) = rounded_rect(0.5, 0.5, w - 1.0, h - 1.0, radius) {
        let mut bgp = Paint::default();
        bgp.set_color(bg());
        bgp.anti_alias = true;
        pm.fill_path(&path, &bgp, FillRule::Winding, Transform::identity(), None);

        let stroke = tiny_skia::Stroke {
            width: 1.0,
            ..Default::default()
        };
        let mut bp = Paint::default();
        bp.set_color(border());
        bp.anti_alias = true;
        pm.stroke_path(&path, &bp, &stroke, Transform::identity(), None);
    }

    let pad_x = PAD_X * s;
    let font_px = FONT_PX * s;
    for row in &layout.rows {
        let it = &items[row.item];
        if row.separator {
            if let Some(rect) = Rect::from_xywh(
                pad_x,
                row.y as f32 + row.h as f32 / 2.0,
                w - 2.0 * pad_x,
                (1.0 * s).max(1.0),
            ) {
                let mut p = Paint::default();
                p.set_color(sep());
                pm.fill_rect(rect, &p, Transform::identity(), None);
            }
            continue;
        }

        if row.item as i32 == active
            && let Some(rect) = Rect::from_xywh(1.0, row.y as f32, w - 2.0, row.h as f32)
        {
            let mut p = Paint::default();
            p.set_color(hover());
            pm.fill_rect(rect, &p, Transform::identity(), None);
        }

        let color = if it.enabled { TEXT } else { TEXT_DISABLED };
        let baseline_y = row.y as f32 + (row.h as f32 - font_px) / 2.0;
        draw_text(fonts, &mut pm, &it.label, font_px, pad_x, baseline_y, color);
    }

    Some(pm)
}

fn draw_text(
    fonts: &mut Fonts,
    pm: &mut Pixmap,
    text: &str,
    font_px: f32,
    ox: f32,
    oy: f32,
    color: Color,
) {
    let buf = fonts.shape(text, font_px);
    let pw = pm.width() as i32;
    let ph = pm.height() as i32;
    let pixels = pm.pixels_mut();
    let runs: Vec<_> = buf.layout_runs().collect();
    for run in runs {
        let glyphs: Vec<_> = run.glyphs.to_vec();
        for glyph in &glyphs {
            let phys = glyph.physical((ox, oy + run.line_y), 1.0);
            let Some(img) = fonts
                .cache
                .get_image(&mut fonts.system, phys.cache_key)
                .as_ref()
            else {
                continue;
            };
            if img.data.is_empty() {
                continue;
            }
            let gx = phys.x + img.placement.left;
            let gy = phys.y - img.placement.top;
            blend_coverage(
                pixels,
                pw,
                ph,
                &img.data,
                img.placement.width as i32,
                img.placement.height as i32,
                gx,
                gy,
                color,
            );
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn blend_coverage(
    pixels: &mut [tiny_skia::PremultipliedColorU8],
    pw: i32,
    ph: i32,
    coverage: &[u8],
    cw: i32,
    ch: i32,
    ox: i32,
    oy: i32,
    color: Color,
) {
    let (cr, cg, cb) = (color.r() as u32, color.g() as u32, color.b() as u32);
    for row in 0..ch {
        let py = oy + row;
        if py < 0 || py >= ph {
            continue;
        }
        for col in 0..cw {
            let px = ox + col;
            if px < 0 || px >= pw {
                continue;
            }
            let a = coverage[(row * cw + col) as usize] as u32;
            if a == 0 {
                continue;
            }
            let idx = (py * pw + px) as usize;
            let dst = pixels[idx];
            let inv = 255 - a;
            // Both src and dst are premultiplied; src alpha == glyph coverage.
            let nr = (cr * a + dst.red() as u32 * inv) / 255;
            let ng = (cg * a + dst.green() as u32 * inv) / 255;
            let nb = (cb * a + dst.blue() as u32 * inv) / 255;
            let na = a + dst.alpha() as u32 * inv / 255;
            if let Some(p) =
                tiny_skia::PremultipliedColorU8::from_rgba(nr as u8, ng as u8, nb as u8, na as u8)
            {
                pixels[idx] = p;
            }
        }
    }
}

fn rounded_rect(x: f32, y: f32, w: f32, h: f32, r: f32) -> Option<tiny_skia::Path> {
    let r = r.min(w / 2.0).min(h / 2.0).max(0.0);
    let mut pb = PathBuilder::new();
    pb.move_to(x + r, y);
    pb.line_to(x + w - r, y);
    pb.quad_to(x + w, y, x + w, y + r);
    pb.line_to(x + w, y + h - r);
    pb.quad_to(x + w, y + h, x + w - r, y + h);
    pb.line_to(x + r, y + h);
    pb.quad_to(x, y + h, x, y + h - r);
    pb.line_to(x, y + r);
    pb.quad_to(x, y, x + r, y);
    pb.close();
    pb.finish()
}
