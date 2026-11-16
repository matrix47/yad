#pragma once

#include "yad/types.hpp"
#include <cstdint>
#include <string>

namespace yad {

struct NinePatchSpan {
    uint16_t start = 0;
    uint16_t length = 0;
    bool stretch = false;
};

struct NinePatchAsset {
    const char *name = nullptr;
    int width = 0;
    int height = 0;
    const uint8_t *rgba = nullptr;
    Margins contentMargins;
    const NinePatchSpan *xSpans = nullptr;
    int xSpanCount = 0;
    const NinePatchSpan *ySpans = nullptr;
    int ySpanCount = 0;
};

struct IconAsset {
    const char *name = nullptr;
    int size = 0;
    const uint8_t *alpha = nullptr;
};

struct FontGlyph {
    uint32_t codepoint = 0;
    uint8_t width = 0;
    uint8_t height = 0;
    uint8_t advance = 0;
    int8_t xOffset = 0;
    int8_t yOffset = 0;
    uint32_t dataOffset = 0;
};

struct BitmapFont {
    int size = 12;
    bool bold = false;
    int lineHeight = 12;
    int ascent = 10;
    const FontGlyph *glyphs = nullptr;
    int glyphCount = 0;
    const uint8_t *alphaData = nullptr;
};

const NinePatchAsset *findNinePatchAsset(const std::string &pathOrName);
const IconAsset *findIconAsset(const std::string &name, int desiredSize);
const BitmapFont *findBitmapFont(int desiredSize, bool bold);

}  // namespace yad
