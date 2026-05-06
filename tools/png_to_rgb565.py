#!/usr/bin/env python3
"""
Convert a PNG/JPG image into a 16-bit RGB565 PROGMEM array suitable for
bit-banging onto an ST7735 LCD. Used at build time to derive a binary blob
from an existing image asset already on disk; does not embed any image data
into source code.

Usage:
  python tools/png_to_rgb565.py <input.png> <output.h> <symbol> <w> <h> [fit|cover]

Mode "fit" (default): preserve aspect, letterbox with black to fill target.
Mode "cover":         preserve aspect, crop centred to fill target exactly.
"""
import sys
from PIL import Image

def main():
    if len(sys.argv) not in (6, 7):
        print("usage: png_to_rgb565.py <in.png> <out.h> <symbol> <w> <h> [fit|cover]")
        sys.exit(2)
    in_path, out_path, sym = sys.argv[1], sys.argv[2], sys.argv[3]
    max_w, max_h = int(sys.argv[4]), int(sys.argv[5])
    mode = sys.argv[6] if len(sys.argv) == 7 else "fit"

    img = Image.open(in_path).convert("RGB")
    iw, ih = img.size

    if mode == "cover":
        # Scale by the larger axis ratio so the image fully covers the target,
        # then center-crop the overflow.
        scale = max(max_w / iw, max_h / ih)
        new_w = max(1, int(round(iw * scale)))
        new_h = max(1, int(round(ih * scale)))
        img = img.resize((new_w, new_h), Image.LANCZOS)
        x0 = (new_w - max_w) // 2
        y0 = (new_h - max_h) // 2
        canvas = img.crop((x0, y0, x0 + max_w, y0 + max_h))
    else:
        # Fit: aspect-preserve, pad with black.
        scale = min(max_w / iw, max_h / ih)
        new_w = max(1, int(round(iw * scale)))
        new_h = max(1, int(round(ih * scale)))
        img = img.resize((new_w, new_h), Image.LANCZOS)
        canvas = Image.new("RGB", (max_w, max_h), (0, 0, 0))
        canvas.paste(img, ((max_w - new_w) // 2, (max_h - new_h) // 2))

    px = canvas.load()
    words = []
    for y in range(max_h):
        for x in range(max_w):
            r, g, b = px[x, y]
            r5 = (r >> 3) & 0x1F
            g6 = (g >> 2) & 0x3F
            b5 = (b >> 3) & 0x1F
            words.append((r5 << 11) | (g6 << 5) | b5)

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("// Auto-generated from {} via tools/png_to_rgb565.py\n".format(in_path.replace("\\", "/")))
        f.write("// DO NOT EDIT BY HAND. Re-run the script after replacing the source PNG.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("static const uint16_t {}_W = {};\n".format(sym, max_w))
        f.write("static const uint16_t {}_H = {};\n".format(sym, max_h))
        f.write("static const uint16_t {}[{} * {}] = {{\n".format(sym, sym + "_W", sym + "_H"))
        for i in range(0, len(words), 16):
            row = ", ".join("0x{:04X}".format(w) for w in words[i:i+16])
            f.write("    " + row + ",\n")
        f.write("};\n")

    print("wrote {} ({}x{} pixels, {} bytes)".format(out_path, max_w, max_h, len(words) * 2))

if __name__ == "__main__":
    main()
