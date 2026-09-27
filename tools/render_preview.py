#!/usr/bin/env python3
"""Render docs/panel-preview.png: an offline picture of one page of the panel (Medium name size).

    pip install pillow
    pio run -e esp32s3          # once, so the Adafruit GFX library (TomThumb) is downloaded
    python tools/render_preview.py

The fonts are read from the same headers the firmware uses, and labels are shortened with a port
of src/abbrev.h. The players, points and game lines are made-up sample data.
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


def main():
    f57 = load_font("src/fonts/Font5x7.h")
    tiny = load_font(TOMTHUMB)
    pos = {"QB": (255, 70, 70), "RB": (60, 220, 90), "WR": (70, 150, 255), "TE": (255, 160, 40),
           "K": (200, 110, 255), "DEF": (160, 160, 160)}
    lead, trail, tied, info = (90, 230, 110), (255, 90, 80), (220, 220, 220), (110, 130, 160)
    rows = [  # position, label, points, score line segments, has the ball
        ("RB", "Gibbs", "25.1", [("21-14", lead), ("vWAS", info)], True),
        ("WR", "Smith-Njigba", "22.8", [("24-27", trail), ("Q4 2:03", info)], False),
        ("QB", "Shough", "19.8", [("17-17", tied), ("HALF", info)], False),
        ("RB", "Cook", "18.3", [("31-10", lead), ("vLAC F", info)], False),
        ("WR", "Washington", "16.3", [("@SF 4:05P", info)], False),
    ]
    p = Panel()
    block = 7 + 5   # Medium: 7 px name row + 5 px score line
    for i, (position, label, pts, segments, ball) in enumerate(rows):
        y = i * block
        for yy in range(y, y + block - 1):
            p.put(0, yy, pos[position])
            p.put(1, yy, pos[position])
        pts_x = W - ink(f57, pts)
        p.text(f57, pts_x, y + 6, pts, (255, 215, 140))
        p.text(f57, 3, y + 6, abbreviate(label, pts_x - 2 - 3, lambda s: ink(f57, s)), (235, 235, 235))
        top = y + 7
        if ball:
            for dx in (1, 2):
                p.put(3 + dx, top + 1, (255, 200, 0))
                p.put(3 + dx, top + 3, (255, 200, 0))
            for dx in range(4):
                p.put(3 + dx, top + 2, (255, 200, 0))
        x = 8
        for k, (text, color) in enumerate(segments):
            x = p.text(tiny, x, top + 5, text + (" " if k < len(segments) - 1 else ""), color)
    for k in range(2):   # page dots: page 1 of 2
        for dx in range(2):
            p.put((W - 5) // 2 + k * 3 + dx, H - 1, (235, 235, 235) if k == 0 else (80, 80, 80))
    p.save("docs/panel-preview.png")


if __name__ == "__main__":
    main()
