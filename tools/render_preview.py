#!/usr/bin/env python3
"""Render docs/panel-preview*.png: offline pictures of the panel with 9 players, one per name size.

    pip install pillow
    pio run -e esp32s3          # once, so the Adafruit GFX library (TomThumb) is downloaded
    python tools/render_preview.py

The fonts are read from the same headers the firmware uses, and labels are shortened with a port
of src/abbrev.h. The points and possession states are made-up sample data.
"""
import re
from PIL import Image, ImageDraw

W = H = 64
TOMTHUMB = ".pio/libdeps/esp32s3/Adafruit GFX Library/Fonts/TomThumb.h"


def load_font(path):
    text = open(path).read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    bitmaps_part, glyphs_part = text.split("Glyphs[]", 1)
    bitmaps_part = bitmaps_part.split("Bitmaps[]", 1)[1]
    bitmaps = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})\b", bitmaps_part)]
    glyphs = [tuple(map(int, m)) for m in re.findall(
        r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", glyphs_part)]
    return bitmaps, glyphs


class Panel:
    def __init__(self):
        self.px = [[None] * W for _ in range(H)]

    def put(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[y][x] = c

    def text(self, font, x, base, s, c):
        bitmaps, glyphs = font
        for ch in s:
            off, w, h, adv, xo, yo = glyphs[ord(ch) - 32]
            bits = []
            for b in bitmaps[off:off + (w * h + 7) // 8]:
                bits += [(b >> (7 - k)) & 1 for k in range(8)]
            for r in range(h):
                for q in range(w):
                    if bits[r * w + q]:
                        self.put(x + xo + q, base + yo + r, c)
            x += adv
        return x

    def save(self, path, scale=8):
        img = Image.new("RGB", (W * scale, H * scale), (10, 10, 10))
        d = ImageDraw.Draw(img)
        for y in range(H):
            for x in range(W):
                if self.px[y][x]:
                    d.ellipse([x * scale + 1, y * scale + 1, x * scale + scale - 2, y * scale + scale - 2],
                              fill=self.px[y][x])
        img.save(path)


def ink(font, s):
    x = right = 0
    for ch in s:
        _, w, _, adv, xo, _ = font[1][ord(ch) - 32]
        if w:
            right = max(right, x + xo + w)
        x += adv
    return right


def abbreviate(s, max_w, width):
    """Port of src/abbrev.h."""
    s = list(s)
    vowels = "aeiou"

    def word_start(i):
        return i == 0 or not s[i - 1].isalpha()

    rules = [
        lambda i: s[i] in ".'-",
        lambda i: s[i].islower() and s[i - 1].lower() == s[i] and s[i] not in vowels,
        lambda i: s[i] in vowels and not word_start(i),
        lambda i: s[i] == " ",
        lambda i: i >= 2 and s[i].islower() and s[i] not in vowels and not word_start(i),
    ]
    for rule in rules:
        while width("".join(s)) > max_w:
            for i in range(len(s) - 2, 0, -1):
                if rule(i):
                    del s[i]
                    break
            else:
                break
    while s and width("".join(s)) > max_w:
        s.pop()
    return "".join(s)


def glcd_font():
    """Adafruit GFX's built-in 6x8 font (glcdfont.c) as a GFXfont-like table."""
    src = open(".pio/libdeps/esp32s3/Adafruit GFX Library/glcdfont.c").read()
    body = src[src.index("{") + 1:src.rindex("}")]
    body = re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", body, flags=re.S))
    cols = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", body)]
    bitmaps, glyphs = [], []
    for code in range(32, 127):
        off = len(bitmaps)
        bits = [(cols[code * 5 + c] >> r) & 1 for r in range(8) for c in range(5)]
        bits += [0] * (-len(bits) % 8)
        bitmaps += [int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
        glyphs.append((off, 5, 8, 6, 0, 0))
    return bitmaps, glyphs


def main():
    f57 = load_font("src/fonts/Font5x7.h")
    sq7 = load_font("src/fonts/FontSqueezed7.h")
    glcd = glcd_font()
    sizes = {  # name: (label font, points font, baseline, font height)
        "large": (glcd, glcd, 0, 8),
        "medium": (f57, f57, 6, 7),
        "narrow": (sq7, f57, 7, 8),
    }
    pos = {"QB": (255, 70, 70), "RB": (60, 220, 90), "WR": (70, 150, 255), "TE": (255, 160, 40),
           "K": (200, 110, 255), "DEF": (160, 160, 160)}
    rows = [  # position, label, points, game live, has the ball, red zone
        ("RB", "Gibbs", "25.1", True, True, False),
        ("WR", "Smith-Njigba", "22.8", True, False, False),
        ("QB", "Shough", "19.8", True, True, True),
        ("RB", "Cook", "18.3", False, False, False),
        ("WR", "Washington", "16.3", True, False, False),
        ("TE", "Schultz", "12.1", True, False, False),
        ("WR", "Boston", "11.3", False, False, False),
        ("TE", "Hockenson", "8.7", True, True, False),
        ("DEF", "Titans", "6.8", True, False, False),
    ]
    for name, (label_font, points_font, baseline, height) in sizes.items():
        p = Panel()
        row_h = min(height + 1, max(7, H // len(rows)))
        for i, (position, label, pts, live, ball, red) in enumerate(rows):
            y = i * row_h
            for yy in range(y, y + row_h - 1):
                p.put(0, yy, pos[position])
                p.put(1, yy, pos[position])
            glcd_pts = points_font is glcd
            pts_x = W - (6 * len(pts) - 1 if glcd_pts else ink(points_font, pts))
            p.text(points_font, pts_x, y + baseline, pts, (255, 215, 140))
            ball_x = pts_x - 2 - 5
            if ball:
                c = (255, 40, 40) if red else (200, 105, 35)
                for dx in range(1, 4):
                    p.put(ball_x + dx, y + 2, c)
                    p.put(ball_x + dx, y + 4, c)
                for dx in range(5):
                    p.put(ball_x + dx, y + 3, (255, 255, 255) if dx == 2 else c)
            right = ball_x if live else pts_x
            if label_font is glcd:
                width = lambda s: 6 * len(s) - 1
            else:
                width = lambda s, f=label_font: ink(f, s)
            p.text(label_font, 3, y + baseline, abbreviate(label, right - 2 - 3, width), (235, 235, 235))
            if row_h < height:   # clear descenders reaching into the next row
                for yy in range(y + row_h, y + height):
                    for x in range(W):
                        if 0 <= yy < H:
                            p.px[yy][x] = None
        p.save("docs/panel-preview.png" if name == "medium" else f"docs/panel-preview-{name}.png")


if __name__ == "__main__":
    main()
