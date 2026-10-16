#!/usr/bin/env python3
"""Generate an original geometric demo using Python's standard library.

Copyright (c) 2026 Ye. MIT license.
Refuses to overwrite local animation files; use your own licensed GIF with
prepare_cat_gif.py if you want to replace the animation intentionally.
"""
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def main():
    binary = ROOT / "main/moon_cat_gif.bin"
    header = ROOT / "main/moon_cat_gif_data.h"
    if binary.exists() or header.exists():
        raise SystemExit("Animation already exists; preserve or move BOTH files before generating a demo.")
    width, height, frames, duration = 64, 64, 16, 80
    background, foreground = 0x0863, 0x07FF
    blob, offsets = bytearray(), []
    for frame in range(frames):
        # Four animated signal bars, designed directly from simple rectangles.
        values = []
        for y in range(height):
            for x in range(width):
                bar = (x - 8) // 12
                lit = 0 <= bar < 4 and (x - 8) % 12 < 8 and 50 - (bar + 1) * 9 <= y < 50
                values.append(foreground if lit and bar <= frame // 4 else background)
        offsets.append(len(blob))
        start = 0
        while start < len(values):
            end = start + 1
            while end < len(values) and values[end] == values[start]:
                end += 1
            blob.extend(struct.pack(">HH", end - start, values[start]))
            start = end
        decoded = []
        for pos in range(offsets[-1], len(blob), 4):
            count, color = struct.unpack_from(">HH", blob, pos)
            decoded.extend([color] * count)
        assert decoded == values
    offsets.append(len(blob))
    metadata = [
        "// Copyright (c) 2026 Ye. MIT. Generated geometric signal animation.",
        "#pragma once", "#include <stdint.h>",
        f"#define CAT_GIF_WIDTH {width}", f"#define CAT_GIF_HEIGHT {height}",
        f"#define CAT_GIF_FRAMES {frames}", f"#define CAT_GIF_PERIOD_MS {frames * duration}",
        f"#define CAT_GIF_BACKGROUND 0x{background:04x}",
        "static const uint32_t cat_gif_offsets[] = {" + ",".join(map(str, offsets)) + "};",
        "static const uint32_t cat_gif_durations[] = {" + ",".join([str(duration)] * frames) + "};",
        "",
    ]
    binary.write_bytes(blob)
    header.write_text("\n".join(metadata))
    print(f"Demo ready: {frames} frames, {len(blob)} bytes; RLE round-trip verified.")


if __name__ == "__main__":
    main()
