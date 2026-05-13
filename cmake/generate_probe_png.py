#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
#
# Generates resources/probe-1080p.png — the cold-boot mode-verification probe.
#
# Why a custom generator: ImageMagick / Pillow are not assumed to be installed
# on every dev machine. This uses only Python stdlib (zlib + struct) so the
# image is reproducible from a clean checkout, and the script doubles as a
# spec for what the probe should look like.
#
# Usage: python3 generate_probe_png.py <output_path>
#
# Probe content (1920x1080 PNG):
#   - 100 px solid red border on all four sides
#   - 200x200 colored corner squares: cyan TL, magenta TR, yellow BL, green BR
#   - Centered black "1920 x 1080" text rendered in a 7-segment-style font
#   - White background between markers
#
# Operator interpretation:
#   PASS — red border at panel edges, all four colored corners at panel corners,
#          "1920 x 1080" centered on the panel.
#   FAIL — red border at the 1/4 mark, all four corners clustered in the
#          top-left quadrant, the rest of the panel showing fbcon black/garbage.

import struct
import sys
import zlib

W, H = 1920, 1080

WHITE   = (255, 255, 255)
BLACK   = (  0,   0,   0)
RED     = (220,  30,  30)
CYAN    = (  0, 200, 220)
MAGENTA = (220,   0, 200)
YELLOW  = (240, 220,   0)
GREEN   = ( 30, 200,  30)


def make_buf():
    return bytearray(WHITE * (W * H))


def fill_rect(buf, x0, y0, x1, y1, color):
    x0 = max(0, x0); y0 = max(0, y0)
    x1 = min(W, x1); y1 = min(H, y1)
    row = bytes(color) * (x1 - x0)
    for y in range(y0, y1):
        i = (y * W + x0) * 3
        buf[i:i + len(row)] = row


# 7-segment-style digits + 'x'. Each glyph is a list of normalized
# (x0,y0,x1,y1) rectangles within a 0..1 x 0..1 cell.
SEG = {
    'a': (0.10, 0.00, 0.90, 0.12),  # top horizontal
    'b': (0.88, 0.05, 1.00, 0.50),  # top right vertical
    'c': (0.88, 0.50, 1.00, 0.95),  # bottom right vertical
    'd': (0.10, 0.88, 0.90, 1.00),  # bottom horizontal
    'e': (0.00, 0.50, 0.12, 0.95),  # bottom left vertical
    'f': (0.00, 0.05, 0.12, 0.50),  # top left vertical
    'g': (0.10, 0.44, 0.90, 0.56),  # middle horizontal
}
DIGIT_SEGS = {
    '0': 'abcdef',
    '1': 'bc',
    '2': 'abged',
    '3': 'abgcd',
    '4': 'fbgc',
    '5': 'afgcd',
    '6': 'afgcde',
    '7': 'abc',
    '8': 'abcdefg',
    '9': 'abcdfg',
}


def draw_thick_line(buf, x0, y0, x1, y1, thickness, color):
    """Bresenham line, painting a thickness x thickness square at each step."""
    dx = abs(x1 - x0); sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0); sy = 1 if y0 < y1 else -1
    err = dx + dy
    half = thickness // 2
    x, y = x0, y0
    while True:
        fill_rect(buf, x - half, y - half, x + thickness - half, y + thickness - half, color)
        if x == x1 and y == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy; x += sx
        if e2 <= dx:
            err += dx; y += sy


def draw_glyph(buf, ch, x, y, w, h, color):
    if ch == 'x':
        # Render 'x' as two diagonal strokes — a real multiplication sign.
        thick = max(10, h // 18)
        inset_x = w // 8
        inset_y = h // 8
        draw_thick_line(buf, x + inset_x, y + inset_y,
                             x + w - inset_x, y + h - inset_y, thick, color)
        draw_thick_line(buf, x + w - inset_x, y + inset_y,
                             x + inset_x, y + h - inset_y, thick, color)
        return
    segs = DIGIT_SEGS.get(ch, '')
    for s in segs:
        nx0, ny0, nx1, ny1 = SEG[s]
        fill_rect(buf,
                  x + int(nx0 * w), y + int(ny0 * h),
                  x + int(nx1 * w), y + int(ny1 * h),
                  color)


def draw_text(buf, text, cx, cy, char_h, color):
    # Each digit cell is char_h tall and ~0.55 * char_h wide; spacing is
    # 0.20 * char_h between cells. Text is horizontally centered around cx.
    char_w = int(char_h * 0.55)
    space = int(char_h * 0.20)
    total_w = len(text) * char_w + (len(text) - 1) * space
    x = cx - total_w // 2
    y = cy - char_h // 2
    for ch in text:
        draw_glyph(buf, ch, x, y, char_w, char_h, color)
        x += char_w + space


def encode_png(buf, w, h):
    def chunk(tag, data):
        out = struct.pack('>I', len(data)) + tag + data
        crc = zlib.crc32(tag + data) & 0xffffffff
        return out + struct.pack('>I', crc)

    # IHDR: 8-bit RGB, no interlace
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    # Build raw scanlines with leading filter byte (0 = None) per row
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        i = y * stride
        raw += buf[i:i + stride]
    idat = zlib.compress(bytes(raw), 9)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')


def main(out_path):
    buf = make_buf()
    # 1. Red border, 100 px on each side
    fill_rect(buf,    0,    0,    W,  100, RED)  # top
    fill_rect(buf,    0, H - 100, W,    H, RED)  # bottom
    fill_rect(buf,    0,    0,  100,    H, RED)  # left
    fill_rect(buf, W - 100, 0,    W,    H, RED)  # right
    # 2. Four 200x200 colored corner squares (inside the red border)
    fill_rect(buf, 100,        100,        300,   300,        CYAN)
    fill_rect(buf, W - 300,    100,        W-100, 300,        MAGENTA)
    fill_rect(buf, 100,        H - 300,    300,   H - 100,    YELLOW)
    fill_rect(buf, W - 300,    H - 300,    W-100, H - 100,    GREEN)
    # 3. Centered "1920 x 1080" text. At char_h=270 with the default ratios
    #    (width 0.55, spacing 0.20) the 9-char string is 1620 px wide,
    #    comfortably inside the 1720 px usable area between the red borders.
    draw_text(buf, '1920x1080', cx=W // 2, cy=H // 2, char_h=270, color=BLACK)
    with open(out_path, 'wb') as f:
        f.write(encode_png(buf, W, H))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.stderr.write("Usage: generate_probe_png.py <output.png>\n")
        sys.exit(1)
    main(sys.argv[1])
