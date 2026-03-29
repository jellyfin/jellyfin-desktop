#define STB_TRUETYPE_IMPLEMENTATION
#include "ui/stb_truetype.h"
#include "ui/menu_overlay.h"
#include <fstream>
#include <algorithm>
#include <cstring>
#include "logging.h"

// Font search paths
static const char* FONT_PATHS[] = {
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/Hack-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/Roboto-Regular.ttf",
    nullptr
};

MenuOverlay::MenuOverlay() = default;

MenuOverlay::~MenuOverlay() {
    delete static_cast<stbtt_fontinfo*>(font_info_);
}

bool MenuOverlay::init() {
    // Find and load a font
    for (int i = 0; FONT_PATHS[i]; i++) {
        std::ifstream file(FONT_PATHS[i], std::ios::binary | std::ios::ate);
        if (file) {
            size_t size = file.tellg();
            file.seekg(0);
            font_data_.resize(size);
            file.read(reinterpret_cast<char*>(font_data_.data()), size);

            auto* info = new stbtt_fontinfo;
            if (stbtt_InitFont(info, font_data_.data(), 0)) {
                font_info_ = info;
                font_scale_ = stbtt_ScaleForPixelHeight(info, FONT_SIZE);
                stbtt_GetFontVMetrics(info, &font_ascent_, &font_descent_, nullptr);
                font_ascent_ = static_cast<int>(font_ascent_ * font_scale_);
                font_descent_ = static_cast<int>(font_descent_ * font_scale_);
                font_line_height_ = font_ascent_ - font_descent_;
                return true;
            }
            delete info;
        }
    }
    return false;
}

void MenuOverlay::open(int x, int y, const std::vector<MenuItem>& items,
                       CefRefPtr<CefRunContextMenuCallback> callback) {
    LOG_DEBUG(LOG_MENU, "open() called at %d,%d with %zu items", x, y, items.size());
    items_ = items;
    callback_ = callback;
    // Offset so cursor is inside menu, not at the corner
    menu_x_ = x - PADDING_X;
    menu_y_ = y - PADDING_Y;
    hover_index_ = -1;
    is_open_ = true;
    ignore_next_up_ = true;  // Ignore the button-up from the right-click that opened us
    if (on_open_) on_open_();
    render();
    LOG_DEBUG(LOG_MENU, "rendered, tex=%dx%d pixels=%zu", tex_width_, tex_height_, pixels_.size());
}

void MenuOverlay::close() {
    if (!is_open_) return;
    if (callback_) {
        callback_->Cancel();
    }
    is_open_ = false;
    needs_redraw_ = true;  // Force compositor to redraw without menu
    callback_ = nullptr;
    items_.clear();
    pixels_.clear();
    if (on_close_) on_close_();
}

void MenuOverlay::select(int index) {
    if (!is_open_ || !callback_) return;
    if (index >= 0 && index < static_cast<int>(items_.size()) && items_[index].enabled) {
        callback_->Continue(items_[index].command_id, EVENTFLAG_NONE);
        is_open_ = false;
        needs_redraw_ = true;
        callback_ = nullptr;
        items_.clear();
        pixels_.clear();
        if (on_close_) on_close_();
    }
}

bool MenuOverlay::handleMouseMove(int x, int y) {
    if (!is_open_) return false;
    int new_hover = itemAtPoint(x, y);
    if (new_hover != hover_index_) {
        hover_index_ = new_hover;
        render();
    }
    return true;
}

bool MenuOverlay::handleMouseClick(int x, int y, bool down) {
    LOG_DEBUG(LOG_MENU, "handleMouseClick %s at %d,%d is_open=%d ignore_next_up=%d",
              down ? "DOWN" : "UP", x, y, is_open_, ignore_next_up_);
    if (!is_open_) return false;
    if (down) {
        // Close on click-down outside menu (more responsive)
        int idx = itemAtPoint(x, y);
        if (idx < 0) {
            LOG_DEBUG(LOG_MENU, "DOWN outside menu, closing");
            close();
            return false;  // Let event pass through to CEF
        }
    } else {  // On release
        if (ignore_next_up_) {
            ignore_next_up_ = false;
            LOG_DEBUG(LOG_MENU, "ignoring initial UP");
            return true;
        }
        int idx = itemAtPoint(x, y);
        LOG_DEBUG(LOG_MENU, "itemAtPoint=%d", idx);
        if (idx >= 0) {
            select(idx);
        }
        // Don't close on UP outside - we already closed on DOWN
    }
    return true;
}

bool MenuOverlay::handleKeyDown(int key) {
    if (!is_open_) return false;
    if (key == 0x1B) {  // ESC
        close();
        return true;
    }
    return false;
}

int MenuOverlay::itemAtPoint(int x, int y) const {
    if (x < menu_x_ || x >= menu_x_ + tex_width_) return -1;
    if (y < menu_y_ || y >= menu_y_ + tex_height_) return -1;
    int rel_y = y - menu_y_;
    int idx = rel_y / ITEM_HEIGHT;
    if (idx >= 0 && idx < static_cast<int>(items_.size())) {
        return idx;
    }
    return -1;
}

void MenuOverlay::render() {
    if (!font_info_ || items_.empty()) return;

    auto* info = static_cast<stbtt_fontinfo*>(font_info_);

    // Calculate dimensions
    int max_text_width = 0;
    for (const auto& item : items_) {
        int w = 0;
        for (char c : item.label) {
            int advance, lsb;
            stbtt_GetCodepointHMetrics(info, c, &advance, &lsb);
            w += static_cast<int>(advance * font_scale_);
        }
        max_text_width = (std::max)(max_text_width, w);
    }

    tex_width_ = (std::max)(MIN_WIDTH, max_text_width + PADDING_X * 2);
    tex_height_ = static_cast<int>(items_.size()) * ITEM_HEIGHT;

    pixels_.resize(static_cast<size_t>(tex_width_) * tex_height_ * 4);

    // Background color (dark gray)
    uint8_t bg_r = 45, bg_g = 45, bg_b = 48, bg_a = 240;
    // Hover color (lighter)
    uint8_t hv_r = 65, hv_g = 65, hv_b = 70, hv_a = 255;
    // Text color
    uint8_t tx_r = 230, tx_g = 230, tx_b = 230;
    // Disabled text
    uint8_t ds_r = 120, ds_g = 120, ds_b = 120;

    // Fill background
    for (int y = 0; y < tex_height_; y++) {
        int item_idx = y / ITEM_HEIGHT;
        bool hover = (item_idx == hover_index_ && items_[item_idx].enabled);
        for (int x = 0; x < tex_width_; x++) {
            int i = (y * tex_width_ + x) * 4;
            if (hover) {
                pixels_[i + 0] = hv_r;
                pixels_[i + 1] = hv_g;
                pixels_[i + 2] = hv_b;
                pixels_[i + 3] = hv_a;
            } else {
                pixels_[i + 0] = bg_r;
                pixels_[i + 1] = bg_g;
                pixels_[i + 2] = bg_b;
                pixels_[i + 3] = bg_a;
            }
        }
    }

    // Render text for each item
    for (size_t idx = 0; idx < items_.size(); idx++) {
        const auto& item = items_[idx];
        int text_y = static_cast<int>(idx) * ITEM_HEIGHT + (ITEM_HEIGHT + font_ascent_) / 2;
        int text_x = PADDING_X;

        uint8_t r = item.enabled ? tx_r : ds_r;
        uint8_t g = item.enabled ? tx_g : ds_g;
        uint8_t b = item.enabled ? tx_b : ds_b;

        for (char c : item.label) {
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(info, c, font_scale_, font_scale_, &x0, &y0, &x1, &y1);

            int glyph_w = x1 - x0;
            int glyph_h = y1 - y0;
            if (glyph_w > 0 && glyph_h > 0) {
                std::vector<uint8_t> glyph(static_cast<size_t>(glyph_w) * glyph_h);
                stbtt_MakeCodepointBitmap(info, glyph.data(), glyph_w, glyph_h, glyph_w,
                                          font_scale_, font_scale_, c);

                // Blit glyph to texture
                for (int gy = 0; gy < glyph_h; gy++) {
                    int dst_y = text_y + y0 + gy;
                    if (dst_y < 0 || dst_y >= tex_height_) continue;
                    for (int gx = 0; gx < glyph_w; gx++) {
                        int dst_x = text_x + x0 + gx;
                        if (dst_x < 0 || dst_x >= tex_width_) continue;
                        uint8_t alpha = glyph[gy * glyph_w + gx];
                        if (alpha > 0) {
                            int i = (dst_y * tex_width_ + dst_x) * 4;
                            // Alpha blend
                            uint8_t inv = 255 - alpha;
                            pixels_[i + 0] = (r * alpha + pixels_[i + 0] * inv) / 255;
                            pixels_[i + 1] = (g * alpha + pixels_[i + 1] * inv) / 255;
                            pixels_[i + 2] = (b * alpha + pixels_[i + 2] * inv) / 255;
                            pixels_[i + 3] = (std::max)(pixels_[i + 3], alpha);
                        }
                    }
                }
            }

            int advance, lsb;
            stbtt_GetCodepointHMetrics(info, c, &advance, &lsb);
            text_x += static_cast<int>(advance * font_scale_);
        }
    }

}

void MenuOverlay::blendOnto(uint8_t* frame, int frame_width, int frame_height) {
    if (!is_open_ || pixels_.empty()) return;

    for (int y = 0; y < tex_height_; y++) {
        int dst_y = menu_y_ + y;
        if (dst_y < 0 || dst_y >= frame_height) continue;

        for (int x = 0; x < tex_width_; x++) {
            int dst_x = menu_x_ + x;
            if (dst_x < 0 || dst_x >= frame_width) continue;

            int src_i = (y * tex_width_ + x) * 4;
            int dst_i = (dst_y * frame_width + dst_x) * 4;

            uint8_t src_r = pixels_[src_i + 0];
            uint8_t src_g = pixels_[src_i + 1];
            uint8_t src_b = pixels_[src_i + 2];
            uint8_t src_a = pixels_[src_i + 3];

            if (src_a == 255) {
                // Opaque - direct copy (RGBA -> BGRA)
                frame[dst_i + 0] = src_b;
                frame[dst_i + 1] = src_g;
                frame[dst_i + 2] = src_r;
                frame[dst_i + 3] = 255;
            } else if (src_a > 0) {
                // Alpha blend (RGBA -> BGRA)
                uint8_t dst_b = frame[dst_i + 0];
                uint8_t dst_g = frame[dst_i + 1];
                uint8_t dst_r = frame[dst_i + 2];
                uint8_t inv = 255 - src_a;
                frame[dst_i + 0] = (src_b * src_a + dst_b * inv) / 255;
                frame[dst_i + 1] = (src_g * src_a + dst_g * inv) / 255;
                frame[dst_i + 2] = (src_r * src_a + dst_r * inv) / 255;
                frame[dst_i + 3] = 255;
            }
        }
    }
}
