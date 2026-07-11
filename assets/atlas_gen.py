#!/usr/bin/env python3
"""assets/atlas_gen.py — generates assets/atlas.png + src/atlas.h (UV table) +
src/atlas_data.h (raw RGBA, compiled into the binary), per PRD §7.5.

Grows in place per milestone rather than being rewritten:
  M1: white 1x1 swatch (solid quads) + the font8x8 strip, parsed directly
      from tools/font8x8.h (single source of truth, no duplication).
  M2: player ship (16x16) + player shot (8x16) — the only two sprites M2's
      world batch needs.
  M4: triangle + circle enemies (16x16 each).
  M5: octagon (16x16), enemy bullet (8x8), spiker boss star (32x32).

Usage: python3 assets/atlas_gen.py
"""
import math
import re
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
FONT_HEADER = ROOT / "tools" / "font8x8.h"
ATLAS_PNG_PATH = ROOT / "assets" / "atlas.png"
ATLAS_H_PATH = ROOT / "src" / "atlas.h"
ATLAS_DATA_H_PATH = ROOT / "src" / "atlas_data.h"

ATLAS_SIZE = 256

FONT_COLS = 16
FONT_ROWS = 8  # 16*8 = 128 glyphs
GLYPH_SIZE = 8
FONT_ORIGIN_X = 0
FONT_ORIGIN_Y = 8  # row 0 is reserved for the white swatch

WHITE_X, WHITE_Y = 0, 0

# shapes region: below the font strip (font occupies y:[8,72))
SHAPES_ORIGIN_Y = 80
PLAYER_RECT   = (0, SHAPES_ORIGIN_Y, 16, 16)   # x, y, w, h
PSHOT_RECT    = (16, SHAPES_ORIGIN_Y, 8, 16)
TRIANGLE_RECT = (24, SHAPES_ORIGIN_Y, 16, 16)
CIRCLE_RECT   = (40, SHAPES_ORIGIN_Y, 16, 16)
OCTAGON_RECT  = (56, SHAPES_ORIGIN_Y, 16, 16)
BULLET_RECT   = (72, SHAPES_ORIGIN_Y, 8, 8)
SPIKER_RECT   = (0, SHAPES_ORIGIN_Y + 16, 32, 32)  # boss, own row below the 16px strip


def parse_font8x8(path):
    text = path.read_text()
    # strip // comments first: several glyph comments contain a literal '}'
    # (e.g. "U+007D (})"), which would otherwise break brace matching below.
    text = re.sub(r"//[^\n]*", "", text)
    m = re.search(r"font8x8_basic\[128\]\[8\]\s*=\s*\{(.*)\};", text, re.S)
    if not m:
        raise SystemExit(f"could not find font8x8_basic table in {path}")
    rows = re.findall(r"\{([^}]*)\}", m.group(1))
    if len(rows) != 128:
        raise SystemExit(f"expected 128 glyph rows, found {len(rows)}")
    glyphs = []
    for row in rows:
        byte_vals = [int(v.strip(), 16) for v in row.split(",") if v.strip()]
        if len(byte_vals) != 8:
            raise SystemExit(f"expected 8 bytes per glyph, found {len(byte_vals)}: {row}")
        glyphs.append(byte_vals)
    return glyphs


def build_atlas(glyphs):
    img = Image.new("RGBA", (ATLAS_SIZE, ATLAS_SIZE), (0, 0, 0, 0))
    px = img.load()

    px[WHITE_X, WHITE_Y] = (255, 255, 255, 255)

    uv_rects = {}
    for idx, glyph in enumerate(glyphs):
        col = idx % FONT_COLS
        row = idx // FONT_COLS
        ox = FONT_ORIGIN_X + col * GLYPH_SIZE
        oy = FONT_ORIGIN_Y + row * GLYPH_SIZE
        for gy in range(GLYPH_SIZE):
            bits = glyph[gy]
            for gx in range(GLYPH_SIZE):
                if (bits >> gx) & 1:
                    px[ox + gx, oy + gy] = (255, 255, 255, 255)
        uv_rects[idx] = (ox, oy, GLYPH_SIZE, GLYPH_SIZE)

    return img, uv_rects


def draw_shapes(img):
    draw = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)

    # player ship: small dart/chevron pointing up (PRD §0.1: "small white ship")
    px, py, pw, ph = PLAYER_RECT
    draw.polygon(
        [(px + 8, py + 1), (px + 13, py + 14), (px + 8, py + 11), (px + 3, py + 14)],
        fill=white,
    )

    # player shot: thin rounded-capsule bolt
    sx, sy, sw, sh = PSHOT_RECT
    draw.rounded_rectangle([sx + 1, sy + 1, sx + sw - 2, sy + sh - 2], radius=3, fill=white)

    # triangle enemy: equilateral triangle, tinted yellow at runtime
    tx, ty, tw, th = TRIANGLE_RECT
    draw.polygon(
        [(tx + tw / 2, ty + 1), (tx + tw - 2, ty + th - 2), (tx + 2, ty + th - 2)],
        fill=white,
    )

    # circle enemy: filled disc, tinted light-blue at runtime
    cx, cy, cw, ch = CIRCLE_RECT
    draw.ellipse([cx + 2, cy + 2, cx + cw - 2, cy + ch - 2], fill=white)

    # octagon enemy: regular 8-gon, tinted pink at runtime
    ox, oy, ow, oh = OCTAGON_RECT
    r = ow / 2.0 - 1.0
    ccx, ccy = ox + ow / 2.0, oy + oh / 2.0
    # rotate by half a step so flats sit top/bottom/left/right, like a real stop sign
    pts = [
        (ccx + r * math.cos(math.pi / 8 + i * math.pi / 4),
         ccy + r * math.sin(math.pi / 8 + i * math.pi / 4))
        for i in range(8)
    ]
    draw.polygon(pts, fill=white)

    # enemy bullet: fat filled disc. Bullet visibility is the whole ballgame in
    # danmaku (PRD §0.1), so it fills its 8x8 cell edge-to-edge.
    bx, by, bw, bh = BULLET_RECT
    draw.ellipse([bx, by, bx + bw - 1, by + bh - 1], fill=white)

    # spiker boss: 8-pointed star (alternating outer/inner radii)
    kx, ky, kw, kh = SPIKER_RECT
    kcx, kcy = kx + kw / 2.0, ky + kh / 2.0
    r_out, r_in = kw / 2.0 - 1.0, kw / 2.0 * 0.42
    star = []
    for i in range(16):
        r = r_out if i % 2 == 0 else r_in
        a = -math.pi / 2 + i * math.pi / 8
        star.append((kcx + r * math.cos(a), kcy + r * math.sin(a)))
    draw.polygon(star, fill=white)

    return {
        "player": PLAYER_RECT,
        "pshot": PSHOT_RECT,
        "triangle": TRIANGLE_RECT,
        "circle": CIRCLE_RECT,
        "octagon": OCTAGON_RECT,
        "bullet": BULLET_RECT,
        "spiker": SPIKER_RECT,
    }


def emit_atlas_h(uv_rects, shape_rects, path):
    white_u = (WHITE_X + 0.5) / ATLAS_SIZE
    white_v = (WHITE_Y + 0.5) / ATLAS_SIZE

    lines = [
        "// atlas.h — generated by assets/atlas_gen.py. Do not hand-edit.",
        "#ifndef ATLAS_H",
        "#define ATLAS_H",
        "",
        f"#define ATLAS_SIZE {ATLAS_SIZE}",
        "",
        "typedef struct { float u0, v0, u1, v1; } AtlasUV;",
        "",
        "// single texel, sampled at its center so a solid-color quad never bleeds",
        f"static const AtlasUV ATLAS_WHITE = {{{white_u:.6f}f, {white_v:.6f}f, {white_u:.6f}f, {white_v:.6f}f}};",
        "",
        "// font8x8 glyph UVs, indexed by ASCII code (0-127)",
        "static const AtlasUV ATLAS_FONT[128] = {",
    ]
    for idx in range(128):
        ox, oy, w, h = uv_rects[idx]
        u0, v0 = ox / ATLAS_SIZE, oy / ATLAS_SIZE
        u1, v1 = (ox + w) / ATLAS_SIZE, (oy + h) / ATLAS_SIZE
        lines.append(f"    {{{u0:.6f}f, {v0:.6f}f, {u1:.6f}f, {v1:.6f}f}}, // {idx}")
    lines += ["};", ""]

    lines.append("// sprites (PRD §7.5)")
    for name, (ox, oy, w, h) in shape_rects.items():
        u0, v0 = ox / ATLAS_SIZE, oy / ATLAS_SIZE
        u1, v1 = (ox + w) / ATLAS_SIZE, (oy + h) / ATLAS_SIZE
        const_name = f"ATLAS_{name.upper()}"
        lines.append(f"static const AtlasUV {const_name} = {{{u0:.6f}f, {v0:.6f}f, {u1:.6f}f, {v1:.6f}f}};")
    lines += ["", "#endif // ATLAS_H"]
    path.write_text("\n".join(lines) + "\n")


def emit_atlas_data_h(img, path):
    rgba = img.tobytes()
    lines = [
        "// atlas_data.h — generated by assets/atlas_gen.py. Do not hand-edit.",
        "// Raw RGBA8 pixel data, row-major, ATLAS_SIZE x ATLAS_SIZE.",
        "#ifndef ATLAS_DATA_H",
        "#define ATLAS_DATA_H",
        "",
        "#include <stdint.h>",
        "",
        f"static const uint8_t ATLAS_DATA[{len(rgba)}] = {{",
    ]
    items_per_line = 20
    for i in range(0, len(rgba), items_per_line):
        chunk = rgba[i:i + items_per_line]
        lines.append("    " + ", ".join(str(b) for b in chunk) + ",")
    lines += ["};", "", "#endif // ATLAS_DATA_H"]
    path.write_text("\n".join(lines) + "\n")


def main():
    glyphs = parse_font8x8(FONT_HEADER)
    img, uv_rects = build_atlas(glyphs)
    shape_rects = draw_shapes(img)
    ATLAS_PNG_PATH.parent.mkdir(parents=True, exist_ok=True)
    img.save(ATLAS_PNG_PATH)
    emit_atlas_h(uv_rects, shape_rects, ATLAS_H_PATH)
    emit_atlas_data_h(img, ATLAS_DATA_H_PATH)
    print(f"wrote {ATLAS_PNG_PATH}, {ATLAS_H_PATH}, {ATLAS_DATA_H_PATH}")


if __name__ == "__main__":
    main()
