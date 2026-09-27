#!/usr/bin/env python3
"""Render docs/panel-preview.png (scoreboard) and docs/update-preview.png (update screen) offline.

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


def main():
    f46 = load_font("src/fonts/Font4x6.h")
    f57 = load_font("src/fonts/Font5x7.h")
    tiny = load_font(TOMTHUMB)
    pos = {"QB": (255, 70, 70), "RB": (60, 220, 90), "WR": (70, 150, 255), "TE": (255, 160, 40),
           "K": (200, 110, 255), "DEF": (160, 160, 160)}
    rows = [  # position, label, points, game live, has the ball, red zone
        ("QB", "Shough", "19.8", True, True, True),
        ("RB", "Cook", "18.3", False, False, False),
        ("RB", "Gibbs", "25.1", True, False, False),
        ("WR", "Smith-Njigba", "22.8", True, False, False),
        ("WR", "Boston", "11.3", False, False, False),
        ("WR", "Washington", "16.3", True, True, False),
        ("TE", "Schultz", "12.1", True, False, False),
        ("TE", "Hockenson", "8.7", True, False, False),
        ("DEF", "Titans", "6.8", True, False, False),
    ]
    p = Panel()
    for i, (position, label, pts, live, ball, red) in enumerate(rows):
        y = i * 6
        for yy in range(y, y + 5):
            p.put(0, yy, pos[position])
            p.put(1, yy, pos[position])
        pts_x = W - ink(f46, pts)
        p.text(f46, pts_x, y + 5, pts, (255, 215, 140))
        ball_x = pts_x - 2 - 5
        if ball:
            c = (255, 40, 40) if red else (200, 105, 35)
            for dx in range(1, 4):
                p.put(ball_x + dx, y + 1, c)
                p.put(ball_x + dx, y + 3, c)
            for dx in range(5):
                p.put(ball_x + dx, y + 2, (255, 255, 255) if dx == 2 else c)
        right = ball_x if live else pts_x
        p.text(f46, 3, y + 5, abbreviate(label, right - 2 - 3, lambda s: ink(f46, s)), (235, 235, 235))
    for x in range(0, W, 2):
        p.put(x + 1, 55, (90, 70, 130))
    total = "%.1f" % sum(float(r[2]) for r in rows)
    tx = W - ink(f46, total)
    p.text(f46, tx, 62, total, (255, 190, 60))
    score = "KC 17 MIA 10"
    font = f46 if ink(f46, score) <= tx - 3 else tiny
    p.text(font, 0, 62, score, (235, 235, 235))
    p.save("docs/panel-preview.png")

    # Update screen after a celebration.
    u = Panel()
    green = (60, 220, 90)
    for x in range(W):
        u.put(x, 0, green); u.put(x, H - 1, green)
    for y in range(H):
        u.put(0, y, green); u.put(W - 1, y, green)

    def centered(font, base, s, c, scale=1):
        w = ink(font, s) * scale
        x0 = (W - w) // 2
        if scale == 1:
            u.text(font, x0, base, s, c)
            return
        tmp = Panel()
        tmp.text(font, 0, 10, s, c)
        for yy in range(H):
            for xx in range(W):
                if tmp.px[yy][xx]:
                    for a in range(scale):
                        for b in range(scale):
                            u.put(x0 + xx * scale + a, base - (10 - yy) * scale + b, tmp.px[yy][xx])

    centered(f57, 4 + 12, "GIBBS", (235, 235, 235), 2)
    # Too wide for one line: split at the space nearest the middle, as drawUpdate() does.
    centered(f57, 22 + 6, "RUN FOR", green)
    centered(f57, 30 + 6, "30 YDS", green)
    centered(f57, 40 + 12, "+3.0", (80, 255, 120), 2)
    centered(f46, 56 + 5, "NOW 25.1", (255, 215, 140))
    u.save("docs/update-preview.png")


if __name__ == "__main__":
    main()
