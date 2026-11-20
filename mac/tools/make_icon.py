#!/usr/bin/env python3
"""Generate the Flipper TNC macOS app icon (Assets.xcassets/AppIcon.appiconset).

Original artwork in the Flipper Zero's visual style: an orange macOS rounded square,
a white device body with an orange-backlit monochrome LCD and a round d-pad. The LCD
shows the same 10x10 pixel antenna glyph as the Flipper app (fap/icons/kiss_10px.png)
plus "KISS TNC" in a 4x5 pixel font. 16/32 px sizes use a simplified glyph-only design.

Usage: python3 mac/tools/make_icon.py   (needs Pillow)
"""

import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "FlipperTNC" / "Assets.xcassets" / "AppIcon.appiconset"

ORANGE = (255, 130, 0)
ORANGE_DEEP = (232, 104, 0)
LCD = (255, 152, 41)
LCD_PIXEL = (20, 12, 4)
BODY = (247, 246, 243)
BODY_EDGE = (214, 211, 205)
BEZEL = (36, 36, 38)

# Same glyph as fap/icons/kiss_10px.png ('#' = lit pixel).
ANTENNA = [
    ".#......#.",
    "#..#..#..#",
    "#.#....#.#",
    "#.#.##.#.#",
    "#..#..#..#",
    ".#..##..#.",
    "....##....",
    "....##....",
    "...####...",
    "..######..",
]

FONT_4X5 = {
    "K": ["#..#", "#.#.", "##..", "#.#.", "#..#"],
    "I": ["###", ".#.", ".#.", ".#.", "###"],
    "S": [".###", "#...", ".##.", "...#", "###."],
    "T": ["####", ".##.", ".##.", ".##.", ".##."],
    "N": ["#..#", "##.#", "#.##", "#..#", "#..#"],
    "C": [".###", "#...", "#...", "#...", ".###"],
}


def squircle(draw, box, radius, fill):
    draw.rounded_rectangle(box, radius=radius, fill=fill)


def gradient_square(size, top, bottom):
    img = Image.new("RGB", (size, size), top)
    px = img.load()
    for y in range(size):
        t = y / (size - 1)
        c = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(size):
            px[x, y] = c
    return img


def draw_pixels(draw, rows, x0, y0, cell, color):
    for r, row in enumerate(rows):
        for c, ch in enumerate(row):
            if ch == "#":
                draw.rectangle(
                    [x0 + c * cell, y0 + r * cell, x0 + (c + 1) * cell - 1, y0 + (r + 1) * cell - 1],
                    fill=color,
                )


def macos_base(S):
    """Transparent canvas with the orange rounded square + soft shadow (macOS icon grid)."""
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    inset = round(S * 100 / 1024)  # 824/1024 body per Apple's template
    radius = round(S * 185 / 1024)
    box = [inset, inset, S - inset, S - inset]

    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        [box[0], box[1] + S * 0.012, box[2], box[3] + S * 0.012], radius=radius, fill=(0, 0, 0, 90)
    )
    canvas = Image.alpha_composite(canvas, shadow.filter(ImageFilter.GaussianBlur(S * 0.012)))

    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    grad = gradient_square(S, ORANGE, ORANGE_DEEP).convert("RGBA")
    canvas.paste(grad, (0, 0), mask)
    return canvas, box


def full_icon(S):
    canvas, (l, t, r, b) = macos_base(S)
    d = ImageDraw.Draw(canvas)
    w = r - l

    # Device body: a white rounded slab across the middle (~1.9:1, like the Flipper).
    bx0, bx1 = l + w * 0.07, r - w * 0.07
    by0, by1 = t + w * 0.27, b - w * 0.27
    bh = by1 - by0
    br = bh * 0.24
    d.rounded_rectangle([bx0, by0 + w * 0.014, bx1, by1 + w * 0.014], radius=br, fill=BODY_EDGE)
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=br, fill=BODY)

    # LCD: bezel + orange backlit screen, 2:1 like the Flipper's 128x64 display.
    lcd_h = bh * 0.60
    lcd_w = lcd_h * 2.0
    lx0 = bx0 + bh * 0.16
    ly0 = by0 + (bh - lcd_h) / 2
    pad = lcd_h * 0.07
    d.rounded_rectangle(
        [lx0 - pad, ly0 - pad, lx0 + lcd_w + pad, ly0 + lcd_h + pad], radius=lcd_h * 0.10, fill=BEZEL
    )
    d.rectangle([lx0, ly0, lx0 + lcd_w, ly0 + lcd_h], fill=LCD)

    # LCD content on a 34x17 pixel grid.
    cell = lcd_w / 34
    def grid(x, y):
        return round(lx0 + x * cell), round(ly0 + y * cell)
    c = max(1, round(cell))
    draw_pixels(d, ANTENNA, *grid(2, 3.5), c, LCD_PIXEL)
    for word, row in (("KISS", 3), ("TNC", 9)):
        x = 14
        for ch in word:
            draw_pixels(d, FONT_4X5[ch], *grid(x, row), c, LCD_PIXEL)
            x += len(FONT_4X5[ch][0]) + 1

    # D-pad: dark ring with an orange center button, right of the screen.
    right = lx0 + lcd_w + pad
    cx = (right + bx1) / 2
    cy = (by0 + by1) / 2
    rr = min(bx1 - right, bh) * 0.36
    d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=BEZEL)
    ri = rr * 0.40
    d.ellipse([cx - ri, cy - ri, cx + ri, cy + ri], fill=ORANGE)
    return canvas


def small_icon(S):
    """16/32 px: just the antenna glyph on the orange square, pixel-aligned."""
    canvas, (l, t, r, b) = macos_base(S)
    d = ImageDraw.Draw(canvas)
    cell = max(1, (r - l) * 0.78 // 10)
    x0 = round((S - cell * 10) / 2)
    y0 = round((S - cell * 10) / 2)
    draw_pixels(d, ANTENNA, x0, y0, int(cell), LCD_PIXEL)
    return canvas


def render(px):
    if px <= 32:
        return small_icon(px * 8).resize((px, px), Image.NEAREST if px >= 32 else Image.LANCZOS)
    big = full_icon(2048)  # supersample, then downscale for anti-aliasing
    return big.resize((px, px), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    images = []
    for pt in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = pt * scale
            name = f"icon_{pt}x{pt}{'@2x' if scale == 2 else ''}.png"
            render(px).save(OUT / name)
            images.append({"idiom": "mac", "size": f"{pt}x{pt}", "scale": f"{scale}x", "filename": name})
    (OUT / "Contents.json").write_text(
        json.dumps({"images": images, "info": {"author": "xcode", "version": 1}}, indent=2) + "\n"
    )
    cat = OUT.parent
    (cat / "Contents.json").write_text(json.dumps({"info": {"author": "xcode", "version": 1}}, indent=2) + "\n")
    accent = cat / "AccentColor.colorset"
    accent.mkdir(exist_ok=True)
    (accent / "Contents.json").write_text(
        json.dumps(
            {
                "colors": [
                    {
                        "idiom": "universal",
                        "color": {
                            "color-space": "srgb",
                            "components": {"red": "1.000", "green": "0.510", "blue": "0.000", "alpha": "1.000"},
                        },
                    }
                ],
                "info": {"author": "xcode", "version": 1},
            },
            indent=2,
        )
        + "\n"
    )
    full_icon(2048).resize((1024, 1024), Image.LANCZOS).save(ROOT / "tools" / "icon_preview.png")
    print(f"wrote {len(images)} icons to {OUT}")


if __name__ == "__main__":
    main()
