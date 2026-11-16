#include "yad/fb.hpp"

#include <algorithm>
#include <cmath>

namespace yad {

static inline uint16_t pack565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint32_t pack32(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

static inline uint16_t blend565(uint16_t dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return dst;
    if (a == 255) return pack565(r, g, b);

    const uint8_t dr = static_cast<uint8_t>(((dst >> 11) & 0x1F) * 255 / 31);
    const uint8_t dg = static_cast<uint8_t>(((dst >> 5) & 0x3F) * 255 / 63);
    const uint8_t db = static_cast<uint8_t>((dst & 0x1F) * 255 / 31);

    const uint16_t inv = 255u - a;
    const uint8_t or_ = static_cast<uint8_t>((r * a + dr * inv) / 255u);
    const uint8_t og_ = static_cast<uint8_t>((g * a + dg * inv) / 255u);
    const uint8_t ob_ = static_cast<uint8_t>((b * a + db * inv) / 255u);
    return pack565(or_, og_, ob_);
}

static inline uint32_t blend32(uint32_t dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return dst;
    if (a == 255) return pack32(r, g, b);

    const uint8_t dr = static_cast<uint8_t>((dst >> 16) & 0xFF);
    const uint8_t dg = static_cast<uint8_t>((dst >> 8) & 0xFF);
    const uint8_t db = static_cast<uint8_t>(dst & 0xFF);

    const uint16_t inv = 255u - a;
    const uint8_t or_ = static_cast<uint8_t>((r * a + dr * inv + 127u) / 255u);
    const uint8_t og_ = static_cast<uint8_t>((g * a + dg * inv + 127u) / 255u);
    const uint8_t ob_ = static_cast<uint8_t>((b * a + db * inv + 127u) / 255u);
    return pack32(or_, og_, ob_);
}

static uint32_t nextUtf8Codepoint(const std::string &s, size_t &idx) {
    const unsigned char c0 = static_cast<unsigned char>(s[idx]);
    if (c0 < 0x80) {
        ++idx;
        return c0;
    }
    if ((c0 & 0xE0) == 0xC0 && idx + 1 < s.size()) {
        const uint32_t cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(s[idx + 1]) & 0x3F);
        idx += 2;
        return cp;
    }
    if ((c0 & 0xF0) == 0xE0 && idx + 2 < s.size()) {
        const uint32_t cp = ((c0 & 0x0F) << 12) |
                            ((static_cast<unsigned char>(s[idx + 1]) & 0x3F) << 6) |
                            (static_cast<unsigned char>(s[idx + 2]) & 0x3F);
        idx += 3;
        return cp;
    }
    if ((c0 & 0xF8) == 0xF0 && idx + 3 < s.size()) {
        const uint32_t cp = ((c0 & 0x07) << 18) |
                            ((static_cast<unsigned char>(s[idx + 1]) & 0x3F) << 12) |
                            ((static_cast<unsigned char>(s[idx + 2]) & 0x3F) << 6) |
                            (static_cast<unsigned char>(s[idx + 3]) & 0x3F);
        idx += 4;
        return cp;
    }
    ++idx;
    return '?';
}

static const FontGlyph *findGlyph(const BitmapFont &font, uint32_t cp) {
    int lo = 0;
    int hi = font.glyphCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint32_t gcp = font.glyphs[mid].codepoint;
        if (gcp == cp) return &font.glyphs[mid];
        if (gcp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    for (int i = 0; i < font.glyphCount; ++i) {
        if (font.glyphs[i].codepoint == '?') return &font.glyphs[i];
    }
    return font.glyphCount > 0 ? &font.glyphs[0] : nullptr;
}

Framebuffer::Framebuffer(int width, int height, PixelFormat format) : format_(format) {
    resize(width, height);
}

void Framebuffer::setFormat(PixelFormat format) {
    if (format_ != format) {
        format_ = format;
        resize(width_, height_);
    }
}

void Framebuffer::resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    const size_t count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    if (format_ == PixelFormat::RGB32) {
        pixels565_.clear();
        pixels32_.assign(count, 0xFF000000u);
    } else {
        pixels32_.clear();
        pixels565_.assign(count, 0);
    }
    resetClip();
}

void Framebuffer::clear(Color c) {
    if (format_ == PixelFormat::RGB32) {
        std::fill(pixels32_.begin(), pixels32_.end(), pack32(c.r, c.g, c.b));
    } else {
        std::fill(pixels565_.begin(), pixels565_.end(), c.toRgb565());
    }
}

void Framebuffer::setClip(const Rect &r) {
    const int x0 = std::max(0, r.x);
    const int y0 = std::max(0, r.y);
    const int x1 = std::min(width_, r.x + r.w);
    const int y1 = std::min(height_, r.y + r.h);
    clip_ = {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

void Framebuffer::resetClip() {
    clip_ = {0, 0, width_, height_};
}

inline void Framebuffer::blendUnchecked(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const size_t idx = static_cast<size_t>(y) * width_ + x;
    if (format_ == PixelFormat::RGB32) {
        pixels32_[idx] = blend32(pixels32_[idx], r, g, b, a);
    } else {
        pixels565_[idx] = blend565(pixels565_[idx], r, g, b, a);
    }
}

void Framebuffer::blendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return;
    if (x < clip_.x || y < clip_.y || x >= clip_.x + clip_.w || y >= clip_.y + clip_.h) return;
    blendUnchecked(x, y, r, g, b, a);
}

void Framebuffer::fillRect(const Rect &r, Color c) {
    if (!c.valid || c.a == 0 || r.empty() || clip_.empty()) return;
    const int x0 = std::max(clip_.x, r.x);
    const int y0 = std::max(clip_.y, r.y);
    const int x1 = std::min(clip_.x + clip_.w, r.x + r.w);
    const int y1 = std::min(clip_.y + clip_.h, r.y + r.h);
    if (x0 >= x1 || y0 >= y1) return;

    if (format_ == PixelFormat::RGB32) {
        if (c.a == 255) {
            const uint32_t px = pack32(c.r, c.g, c.b);
            for (int y = y0; y < y1; ++y) {
                uint32_t *row = &pixels32_[static_cast<size_t>(y) * width_];
                std::fill(row + x0, row + x1, px);
            }
        } else {
            for (int y = y0; y < y1; ++y) {
                uint32_t *row = &pixels32_[static_cast<size_t>(y) * width_];
                for (int x = x0; x < x1; ++x) row[x] = blend32(row[x], c.r, c.g, c.b, c.a);
            }
        }
        return;
    }

    if (c.a == 255) {
        const uint16_t px = c.toRgb565();
        for (int y = y0; y < y1; ++y) {
            uint16_t *row = &pixels565_[static_cast<size_t>(y) * width_];
            std::fill(row + x0, row + x1, px);
        }
    } else {
        for (int y = y0; y < y1; ++y) {
            uint16_t *row = &pixels565_[static_cast<size_t>(y) * width_];
            for (int x = x0; x < x1; ++x) row[x] = blend565(row[x], c.r, c.g, c.b, c.a);
        }
    }
}

void Framebuffer::fillCircle(float cx, float cy, float radius, Color c) {
    if (!c.valid || c.a == 0 || radius <= 0.0f || clip_.empty()) return;
    const int x0 = std::max(clip_.x, static_cast<int>(std::floor(cx - radius - 1.0f)));
    const int y0 = std::max(clip_.y, static_cast<int>(std::floor(cy - radius - 1.0f)));
    const int x1 = std::min(clip_.x + clip_.w - 1, static_cast<int>(std::ceil(cx + radius + 1.0f)));
    const int y1 = std::min(clip_.y + clip_.h - 1, static_cast<int>(std::ceil(cy + radius + 1.0f)));

    for (int y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - cy;
        for (int x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) - cx;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist <= radius - 0.5f) {
                blendUnchecked(x, y, c.r, c.g, c.b, c.a);
            } else if (dist < radius + 0.5f) {
                const float cov = (radius + 0.5f) - dist;
                const uint8_t aa = static_cast<uint8_t>(std::clamp(cov, 0.0f, 1.0f) * c.a);
                blendUnchecked(x, y, c.r, c.g, c.b, aa);
            }
        }
    }
}

static std::vector<int> computeTargetSpanSizes(const NinePatchSpan *spans, int count, int targetTotal) {
    std::vector<int> sizes(count, 0);
    int fixedTotal = 0;
    int stretchSrcTotal = 0;
    for (int i = 0; i < count; ++i) {
        if (spans[i].stretch) stretchSrcTotal += spans[i].length;
        else {
            fixedTotal += spans[i].length;
            sizes[i] = spans[i].length;
        }
    }
    const int remaining = std::max(0, targetTotal - fixedTotal);
    if (stretchSrcTotal <= 0) return sizes;

    int assigned = 0;
    int lastStretch = -1;
    for (int i = 0; i < count; ++i) {
        if (spans[i].stretch) {
            lastStretch = i;
            const int s = (remaining * spans[i].length) / stretchSrcTotal;
            sizes[i] = s;
            assigned += s;
        }
    }
    if (lastStretch >= 0 && assigned < remaining) sizes[lastStretch] += (remaining - assigned);
    return sizes;
}

void Framebuffer::drawNinePatch(const Rect &r, const NinePatchAsset &np) {
    if (r.empty() || !np.rgba || np.xSpanCount <= 0 || np.ySpanCount <= 0) return;

    const auto xSizes = computeTargetSpanSizes(np.xSpans, np.xSpanCount, r.w);
    const auto ySizes = computeTargetSpanSizes(np.ySpans, np.ySpanCount, r.h);

    int dy = r.y;
    for (int yi = 0; yi < np.ySpanCount; ++yi) {
        const int dstH = ySizes[yi];
        const auto &ys = np.ySpans[yi];
        if (dstH > 0) {
            int dx = r.x;
            for (int xi = 0; xi < np.xSpanCount; ++xi) {
                const int dstW = xSizes[xi];
                const auto &xs = np.xSpans[xi];
                if (dstW > 0) {
                    const int cx0 = std::max(clip_.x, dx);
                    const int cy0 = std::max(clip_.y, dy);
                    const int cx1 = std::min(clip_.x + clip_.w, dx + dstW);
                    const int cy1 = std::min(clip_.y + clip_.h, dy + dstH);
                    for (int py = cy0; py < cy1; ++py) {
                        const int sy = ys.start + ((py - dy) * ys.length) / dstH;
                        for (int px = cx0; px < cx1; ++px) {
                            const int sx = xs.start + ((px - dx) * xs.length) / dstW;
                            const size_t off = (static_cast<size_t>(sy) * np.width + sx) * 4;
                            const uint8_t a = np.rgba[off + 3];
                            if (a > 0) {
                                blendUnchecked(px, py, np.rgba[off], np.rgba[off + 1], np.rgba[off + 2], a);
                            }
                        }
                    }
                }
                dx += dstW;
            }
        }
        dy += dstH;
    }
}

void Framebuffer::drawIcon(int x, int y, const IconAsset &icon, Color tint) {
    if (!icon.alpha || icon.size <= 0 || !tint.valid || tint.a == 0) return;
    for (int iy = 0; iy < icon.size; ++iy) {
        const int py = y + iy;
        if (py < clip_.y || py >= clip_.y + clip_.h) continue;
        for (int ix = 0; ix < icon.size; ++ix) {
            const int px = x + ix;
            if (px < clip_.x || px >= clip_.x + clip_.w) continue;
            const uint8_t mask = icon.alpha[iy * icon.size + ix];
            if (mask > 0) {
                const uint8_t a = static_cast<uint8_t>((mask * tint.a) / 255u);
                blendUnchecked(px, py, tint.r, tint.g, tint.b, a);
            }
        }
    }
}

int Framebuffer::measureText(const BitmapFont &font, const std::string &utf8) const {
    int w = 0;
    size_t idx = 0;
    while (idx < utf8.size()) {
        const uint32_t cp = nextUtf8Codepoint(utf8, idx);
        if (const FontGlyph *g = findGlyph(font, cp)) w += g->advance;
    }
    return w;
}

std::string Framebuffer::elideText(const BitmapFont &font, const std::string &utf8, int maxWidth) const {
    if (maxWidth <= 0) return {};
    if (measureText(font, utf8) <= maxWidth) return utf8;

    const std::string ellipsis = "...";
    const int ellW = measureText(font, ellipsis);
    if (ellW >= maxWidth) return {};

    std::string out;
    int curW = 0;
    size_t idx = 0;
    while (idx < utf8.size()) {
        const size_t prevIdx = idx;
        const uint32_t cp = nextUtf8Codepoint(utf8, idx);
        const FontGlyph *g = findGlyph(font, cp);
        const int adv = g ? g->advance : 0;
        if (curW + adv + ellW > maxWidth) break;
        out.append(utf8, prevIdx, idx - prevIdx);
        curW += adv;
    }
    return out + ellipsis;
}

void Framebuffer::drawText(const Rect &r,
                           const BitmapFont &font,
                           const std::string &utf8,
                           Color color,
                           uint8_t align,
                           int xOffset) {
    if (utf8.empty() || !color.valid || color.a == 0 || r.empty()) return;

    const int textW = measureText(font, utf8);
    int penX = r.x;
    if (align & AlignRight) penX = r.x + r.w - textW;
    else if (align & AlignHCenter) penX = r.x + (r.w - textW) / 2;
    penX += xOffset;

    int lineTop = r.y;
    if (align & AlignBottom) lineTop = r.y + r.h - font.lineHeight;
    else if (align & AlignVCenter) lineTop = r.y + (r.h - font.lineHeight) / 2;

    size_t idx = 0;
    while (idx < utf8.size()) {
        const uint32_t cp = nextUtf8Codepoint(utf8, idx);
        const FontGlyph *g = findGlyph(font, cp);
        if (!g) continue;

        const uint8_t *src = font.alphaData + g->dataOffset;
        const int gx0 = penX + g->xOffset;
        const int gy0 = lineTop + g->yOffset;
        for (int row = 0; row < g->height; ++row) {
            const int py = gy0 + row;
            if (py < clip_.y || py >= clip_.y + clip_.h) continue;
            for (int col = 0; col < g->width; ++col) {
                const int px = gx0 + col;
                if (px < clip_.x || px >= clip_.x + clip_.w) continue;
                const uint8_t alpha = src[row * g->width + col];
                if (alpha > 0) {
                    const uint8_t a = static_cast<uint8_t>((alpha * color.a) / 255u);
                    blendUnchecked(px, py, color.r, color.g, color.b, a);
                }
            }
        }
        penX += g->advance;
    }
}

}  // namespace yad
