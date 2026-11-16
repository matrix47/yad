#pragma once

#include "yad/assets.hpp"
#include "yad/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace yad {

enum class PixelFormat {
    RGB565,
    RGB32  // 0xFFRRGGBB in native uint32_t (matches QImage::Format_RGB32 / SDL_PIXELFORMAT_ARGB8888)
};

class Framebuffer {
public:
    Framebuffer(int width = 320, int height = 240, PixelFormat format = PixelFormat::RGB32);

    void resize(int width, int height);
    void setFormat(PixelFormat format);
    PixelFormat format() const { return format_; }

    int width() const { return width_; }
    int height() const { return height_; }

    const uint16_t *data565() const { return pixels565_.data(); }
    uint16_t *data565() { return pixels565_.data(); }
    const uint32_t *data32() const { return pixels32_.data(); }
    uint32_t *data32() { return pixels32_.data(); }
    const void *rawData() const {
        return (format_ == PixelFormat::RGB32) ? static_cast<const void *>(pixels32_.data())
                                               : static_cast<const void *>(pixels565_.data());
    }
    int strideBytes() const {
        return width_ * static_cast<int>(format_ == PixelFormat::RGB32 ? sizeof(uint32_t) : sizeof(uint16_t));
    }

    void clear(Color c);
    void setClip(const Rect &r);
    void resetClip();
    Rect clip() const { return clip_; }

    void blendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void fillRect(const Rect &r, Color c);
    void fillCircle(float cx, float cy, float radius, Color c);
    void drawNinePatch(const Rect &r, const NinePatchAsset &np);
    void drawIcon(int x, int y, const IconAsset &icon, Color tint);

    int measureText(const BitmapFont &font, const std::string &utf8) const;
    void drawText(const Rect &r,
                  const BitmapFont &font,
                  const std::string &utf8,
                  Color color,
                  uint8_t align,
                  int xOffset = 0);
    std::string elideText(const BitmapFont &font, const std::string &utf8, int maxWidth) const;

private:
    void blendUnchecked(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    int width_ = 0;
    int height_ = 0;
    PixelFormat format_ = PixelFormat::RGB32;
    Rect clip_;
    std::vector<uint16_t> pixels565_;
    std::vector<uint32_t> pixels32_;
};

}  // namespace yad
