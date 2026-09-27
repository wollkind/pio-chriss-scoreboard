#!/usr/bin/env python3
"""Render every panel animation to docs/gifs/*.gif, from the firmware's own drawing code.

    pip install pillow
    pio run -e esp32s3            # once, so the Adafruit GFX and ArduinoJson libraries are downloaded
    python tools/sim/make_gifs.py

Compiles tools/sim/sim.cpp (which includes src/main.cpp) with the host C++ compiler and the
stand-in headers in tools/sim/include, runs it, and turns each scene's raw RGB565 frames into an
animated GIF that looks like the LED panel: one round dot per pixel, 50 ms per frame.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIBS = os.path.join(ROOT, ".pio", "libdeps", "esp32s3")
GFX = os.path.join(LIBS, "Adafruit GFX Library")
OUT = os.path.join(ROOT, "docs", "gifs")
W = H = 64
SCALE = 4          # 256 x 256 GIFs
FRAME_MS = 50


def build(exe):
    cmd = [
        os.environ.get("CXX", "g++"), "-std=gnu++17", "-O1", "-w",
        "-DARDUINO=10800", "-DARDUINOJSON_ENABLE_PROGMEM=0",
        "-I", os.path.join(ROOT, "tools", "sim", "include"),
        "-I", os.path.join(ROOT, "src"),
        "-I", GFX,
        "-I", os.path.join(LIBS, "ArduinoJson", "src"),
        os.path.join(ROOT, "tools", "sim", "sim.cpp"),
        os.path.join(ROOT, "src", "startup.cpp"),
        os.path.join(ROOT, "src", "celebrate.cpp"),
        os.path.join(GFX, "Adafruit_GFX.cpp"),
        "-o", exe,
    ]
    subprocess.run(cmd, check=True)


def frames(path):
    data = open(path, "rb").read()
    size = W * H * 2
    for i in range(0, len(data) - size + 1, size):
        yield data[i:i + size]


def to_image(raw, dot):
    img = Image.new("RGB", (W * SCALE, H * SCALE), (12, 12, 12))
    d = ImageDraw.Draw(img)
    for y in range(H):
        for x in range(W):
            v = raw[(y * W + x) * 2] | (raw[(y * W + x) * 2 + 1] << 8)
            if not v:
                continue
            r = (v >> 8) & 0xF8
            g = (v >> 3) & 0xFC
            b = (v << 3) & 0xF8
            d.rectangle(dot(x, y), fill=(r | r >> 5, g | g >> 6, b | b >> 5))
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "sim")
        build(exe)
        names = subprocess.run([exe, tmp], check=True, capture_output=True, text=True).stdout.split()

        def dot(x, y):
            return [x * SCALE, y * SCALE, x * SCALE + SCALE - 2, y * SCALE + SCALE - 2]

        for name in names:
            imgs = [to_image(raw, dot) for raw in frames(os.path.join(tmp, name + ".rgb565"))]
            # Merge runs of identical frames into one longer frame (smaller files, same timing).
            out, durations = [], []
            for img in imgs:
                if out and img.tobytes() == out[-1].tobytes():
                    durations[-1] += FRAME_MS
                else:
                    out.append(img)
                    durations.append(FRAME_MS)
            path = os.path.join(OUT, name + ".gif")
            out[0].save(path, save_all=True, append_images=out[1:], duration=durations, loop=0, optimize=True)
            print("%-14s %4d frames  %6.1f s  %4d KB" % (name, len(imgs), len(imgs) * FRAME_MS / 1000,
                                                       os.path.getsize(path) // 1024))


if __name__ == "__main__":
    sys.exit(main())
