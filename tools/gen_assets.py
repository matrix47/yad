#!/usr/bin/env python3
"""Generate pure-C++ static asset tables (9-patches, icons, Terminus bitmap fonts) for libyad."""

import argparse
import glob
import io
import os
import subprocess
from PIL import Image, ImageDraw, ImageFont


ICON_SIZES = [12, 14, 16, 20, 24]
FONT_SIZES = [12, 14, 16, 18, 20, 24]

# Printable ASCII + symbols used by cards (°, ↓, ↑, …)
CODEPOINTS = list(range(0x20, 0x7F)) + [0x00B0, 0x2191, 0x2193, 0x2026]


def is_marker_pixel(px):
    r, g, b, a = px
    return a > 128 and r < 64 and g < 64 and b < 64


def build_spans(markers):
    if not markers:
        return []
    spans = []
    cur = markers[0]
    start = 0
    for i in range(1, len(markers)):
        if markers[i] != cur:
            spans.append((start, i - start, cur))
            cur = markers[i]
            start = i
    spans.append((start, len(markers) - start, cur))
    return spans


def extract_bounds(markers):
    first = -1
    last = -1
    for i, m in enumerate(markers):
        if m:
            if first == -1:
                first = i
            last = i
    if first == -1:
        return 0, 0
    return first, (len(markers) - 1) - last


def parse_ninepatch(path):
    im = Image.open(path).convert("RGBA")
    w_raw, h_raw = im.size
    w = w_raw - 2
    h = h_raw - 2
    px = im.load()

    x_stretch = [is_marker_pixel(px[x + 1, 0]) for x in range(w)]
    y_stretch = [is_marker_pixel(px[0, y + 1]) for y in range(h)]
    x_content = [is_marker_pixel(px[x + 1, h_raw - 1]) for x in range(w)]
    y_content = [is_marker_pixel(px[w_raw - 1, y + 1]) for y in range(h)]

    if not any(x_stretch):
        x_stretch = [True] * w
    if not any(y_stretch):
        y_stretch = [True] * h

    pad_left, pad_right = extract_bounds(x_content if any(x_content) else x_stretch)
    pad_top, pad_bottom = extract_bounds(y_content if any(y_content) else y_stretch)

    inner = im.crop((1, 1, w_raw - 1, h_raw - 1))
    return {
        "name": os.path.basename(path),
        "width": w,
        "height": h,
        "rgba": list(inner.tobytes()),
        "margins": (pad_left, pad_top, pad_right, pad_bottom),
        "x_spans": build_spans(x_stretch),
        "y_spans": build_spans(y_stretch),
    }


def render_svg_alpha(svg_path, size):
    png_bytes = subprocess.check_output(
        ["rsvg-convert", "-w", str(size), "-h", str(size), svg_path]
    )
    im = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    if im.size != (size, size):
        im = im.resize((size, size), Image.Resampling.BILINEAR)
    return list(im.getchannel("A").tobytes())


def find_terminus_font(size, bold):
    style = "b" if bold else "n"
    otb = f"/usr/share/fonts/misc/ter-u{size}{style}.otb"
    if os.path.exists(otb):
        return otb
    ttf = "/usr/share/fonts/TTF/TerminusTTF-Bold.ttf" if bold else "/usr/share/fonts/TTF/TerminusTTF.ttf"
    if os.path.exists(ttf):
        return ttf
    raise FileNotFoundError(f"No Terminus font found for size={size} bold={bold}")


def build_bitmap_font(size, bold):
    font_path = find_terminus_font(size, bold)
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()
    line_height = max(size, ascent + descent)

    glyphs = []
    alpha_data = bytearray()

    for cp in CODEPOINTS:
        ch = chr(cp)
        adv = max(1, int(round(font.getlength(ch))))
        bbox = font.getbbox(ch)
        if bbox is None:
            glyphs.append((cp, 0, 0, adv, 0, 0, len(alpha_data)))
            continue

        left, top, right, bottom = bbox
        gw = max(0, right - left)
        gh = max(0, bottom - top)
        if gw == 0 or gh == 0:
            glyphs.append((cp, 0, 0, adv, 0, 0, len(alpha_data)))
            continue

        canvas = Image.new("L", (gw + 4, gh + 4), 0)
        draw = ImageDraw.Draw(canvas)
        if size <= 20:
            draw.fontmode = "1"
        draw.text((2 - left, 2 - top), ch, fill=255, font=font)
        crop = canvas.crop((2, 2, 2 + gw, 2 + gh))
        data_offset = len(alpha_data)
        alpha_data.extend(crop.tobytes())
        glyphs.append((cp, gw, gh, adv, left, top, data_offset))

    return {
        "size": size,
        "bold": bold,
        "line_height": line_height,
        "ascent": ascent,
        "glyphs": glyphs,
        "alpha": bytes(alpha_data),
    }


def format_bytes(arr, cols=16):
    lines = []
    for i in range(0, len(arr), cols):
        chunk = arr[i:i + cols]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk))
    return ",\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets-dir", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    frames_dir = os.path.join(args.assets_dir, "frames")
    icons_dir = os.path.join(args.assets_dir, "icons")

    ninepatches = [parse_ninepatch(p) for p in sorted(glob.glob(os.path.join(frames_dir, "*.9.png")))]

    icons = []
    for svg in sorted(glob.glob(os.path.join(icons_dir, "*.svg"))):
        name = os.path.splitext(os.path.basename(svg))[0]
        for sz in ICON_SIZES:
            icons.append((name, sz, render_svg_alpha(svg, sz)))

    fonts = []
    for sz in FONT_SIZES:
        for bold in (False, True):
            fonts.append(build_bitmap_font(sz, bold))

    out = []
    out.append("// Auto-generated by tools/gen_assets.py - DO NOT EDIT")
    out.append('#include "yad/assets.hpp"')
    out.append("#include <algorithm>")
    out.append("#include <cmath>")
    out.append("")
    out.append("namespace yad {")
    out.append("namespace {")
    out.append("")

    for idx, np in enumerate(ninepatches):
        out.append(f"static const uint8_t kNpRgba_{idx}[] = {{")
        out.append(format_bytes(np["rgba"]))
        out.append("};")
        out.append(f"static const NinePatchSpan kNpXSpans_{idx}[] = {{")
        for s, l, st in np["x_spans"]:
            out.append(f"    {{{s}, {l}, {'true' if st else 'false'}}},")
        out.append("};")
        out.append(f"static const NinePatchSpan kNpYSpans_{idx}[] = {{")
        for s, l, st in np["y_spans"]:
            out.append(f"    {{{s}, {l}, {'true' if st else 'false'}}},")
        out.append("};")

    out.append("static const NinePatchAsset kNinePatches[] = {")
    for idx, np in enumerate(ninepatches):
        ml, mt, mr, mb = np["margins"]
        out.append(
            f'    {{"{np["name"]}", {np["width"]}, {np["height"]}, kNpRgba_{idx}, '
            f'{{{ml}, {mt}, {mr}, {mb}}}, kNpXSpans_{idx}, {len(np["x_spans"])}, '
            f'kNpYSpans_{idx}, {len(np["y_spans"])}}},'
        )
    out.append("};")
    out.append("")

    for idx, (name, sz, alpha) in enumerate(icons):
        out.append(f"static const uint8_t kIconAlpha_{idx}[] = {{")
        out.append(format_bytes(alpha))
        out.append("};")
    out.append("static const IconAsset kIcons[] = {")
    for idx, (name, sz, _) in enumerate(icons):
        out.append(f'    {{"{name}", {sz}, kIconAlpha_{idx}}},')
    out.append("};")
    out.append("")

    for idx, fnt in enumerate(fonts):
        out.append(f"static const uint8_t kFontAlpha_{idx}[] = {{")
        out.append(format_bytes(fnt["alpha"] if fnt["alpha"] else b"\x00"))
        out.append("};")
        out.append(f"static const FontGlyph kFontGlyphs_{idx}[] = {{")
        for cp, gw, gh, adv, xo, yo, off in fnt["glyphs"]:
            out.append(f"    {{{cp}u, {gw}, {gh}, {adv}, {xo}, {yo}, {off}u}},")
        out.append("};")

    out.append("static const BitmapFont kFonts[] = {")
    for idx, fnt in enumerate(fonts):
        out.append(
            f"    {{{fnt['size']}, {'true' if fnt['bold'] else 'false'}, "
            f"{fnt['line_height']}, {fnt['ascent']}, kFontGlyphs_{idx}, "
            f"{len(fnt['glyphs'])}, kFontAlpha_{idx}}},"
        )
    out.append("};")
    out.append("")
    out.append("}  // namespace")
    out.append("")

    out.append("const NinePatchAsset *findNinePatchAsset(const std::string &pathOrName) {")
    out.append("    if (pathOrName.empty()) return nullptr;")
    out.append("    std::string key = pathOrName;")
    out.append("    const size_t slash = key.find_last_of('/');")
    out.append("    if (slash != std::string::npos) key = key.substr(slash + 1);")
    out.append("    for (const auto &np : kNinePatches) {")
    out.append("        if (key == np.name) return &np;")
    out.append("    }")
    out.append("    return nullptr;")
    out.append("}")
    out.append("")
    out.append("const IconAsset *findIconAsset(const std::string &name, int desiredSize) {")
    out.append("    if (name.empty()) return nullptr;")
    out.append("    std::string key = name;")
    out.append("    const size_t slash = key.find_last_of('/');")
    out.append("    if (slash != std::string::npos) key = key.substr(slash + 1);")
    out.append('    if (key.size() > 4 && key.substr(key.size() - 4) == ".svg") {')
    out.append("        key.resize(key.size() - 4);")
    out.append("    }")
    out.append("    const IconAsset *best = nullptr;")
    out.append("    int bestDiff = 1000000;")
    out.append("    for (const auto &ic : kIcons) {")
    out.append("        if (key == ic.name) {")
    out.append("            const int diff = std::abs(ic.size - desiredSize);")
    out.append("            if (diff < bestDiff) { bestDiff = diff; best = &ic; }")
    out.append("        }")
    out.append("    }")
    out.append("    return best;")
    out.append("}")
    out.append("")
    out.append("const BitmapFont *findBitmapFont(int desiredSize, bool bold) {")
    out.append("    const BitmapFont *best = nullptr;")
    out.append("    int bestScore = 1000000;")
    out.append("    for (const auto &f : kFonts) {")
    out.append("        int score = std::abs(f.size - desiredSize) * 10 + (f.bold == bold ? 0 : 1);")
    out.append("        if (score < bestScore) { bestScore = score; best = &f; }")
    out.append("    }")
    out.append("    return best;")
    out.append("}")
    out.append("")
    out.append("}  // namespace yad")
    out.append("")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
