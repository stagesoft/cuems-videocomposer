#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>

"""Generate splash_png.h from resources/splash.png (same format as xxd -i)."""
import sys
import os

def main():
    if len(sys.argv) != 3:
        sys.stderr.write("Usage: embed_splash.py <input.png> <output.h>\n")
        sys.exit(1)
    png_path = sys.argv[1]
    out_path = sys.argv[2]
    with open(png_path, "rb") as f:
        data = f.read()
    n = len(data)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as out:
        out.write("/* Generated from %s - do not edit */\n" % os.path.basename(png_path))
        out.write("unsigned char splash_png[] = {\n")
        for i in range(0, n, 12):
            chunk = data[i:i+12]
            out.write("  " + ", ".join("0x%02x" % b for b in chunk) + ",\n")
        out.write("};\n")
        out.write("unsigned int splash_png_len = %u;\n" % n)
    return 0

if __name__ == "__main__":
    sys.exit(main())
