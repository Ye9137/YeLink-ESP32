#!/usr/bin/env python3
"""Copyright (c) 2026 Ye；把原 GIF 转成保留帧时长的 RGB565 游程动画。

用法：python3 scripts/prepare_cat_gif.py assets/moon-cat-original.gif
仅电脑端需要 Pillow；设备无需 GIF 解码器或额外帧缓冲。
"""
from pathlib import Path
import struct
import sys

from PIL import Image, ImageChops, ImageOps

ROOT = Path(__file__).resolve().parents[1]


def rgb565(rgb):
    """使用屏幕当前的 RGB 顺序，高字节在前。"""
    r, g, b = rgb
    return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3)


def convert(source):
    """合成 GIF 的增量帧，再统一裁掉静止背景，避免角色逐帧跳位。"""
    frames, durations, boxes = [], [], []
    with Image.open(source) as gif:
        background = gif.convert("RGB").getpixel((0, 0))
        for index in range(gif.n_frames):
            gif.seek(index)
            # Pillow 的 seek 会按 GIF disposal 规则合成完整帧。
            rgba = gif.convert("RGBA")
            canvas = Image.new("RGBA", gif.size, (*background, 255))
            frame = Image.alpha_composite(canvas, rgba).convert("RGB")
            frames.append(frame)
            durations.append(max(10, int(gif.info.get("duration", 100))))
            box = ImageChops.difference(frame, Image.new("RGB", frame.size, background)).getbbox()
            if box:
                boxes.append(box)
    if not boxes:
        raise ValueError("GIF 没有可见角色")
    # 所有帧共同的包围盒外留两像素；不裁掉任何帧中的猫或尾巴。
    crop = (max(0, min(b[0] for b in boxes) - 2), max(0, min(b[1] for b in boxes) - 2),
            min(frames[0].width, max(b[2] for b in boxes) + 2),
            min(frames[0].height, max(b[3] for b in boxes) + 2))
    blob, offsets, previews = bytearray(), [], []
    for frame in frames:
        fitted = ImageOps.contain(frame.crop(crop), (128, 104), Image.Resampling.LANCZOS)
        rgb_bytes = fitted.tobytes()
        values = [rgb565(rgb_bytes[i:i + 3]) for i in range(0, len(rgb_bytes), 3)]
        offsets.append(len(blob))
        # 每个游程为大端 uint16 像素数和大端 RGB565，颜色量化后无损压缩。
        start = 0
        while start < len(values):
            end = start + 1
            while end < len(values) and values[end] == values[start] and end - start < 65535:
                end += 1
            blob.extend(struct.pack(">HH", end - start, values[start]))
            start = end
        # 独立展开压缩结果，避免生成损坏的固件资源。
        decoded = []
        for pos in range(offsets[-1], len(blob), 4):
            count, color = struct.unpack_from(">HH", blob, pos)
            decoded.extend([color] * count)
        assert decoded == values
        # 预览同样使用 RGB565 量化后的颜色，而非原始高色深图片。
        rgb = [((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31) for v in values]
        small = Image.new("RGB", fitted.size)
        small.putdata(rgb)
        bg = rgb565(background)
        preview = Image.new("RGB", (128, 160), ((bg >> 11) * 255 // 31, ((bg >> 5) & 63) * 255 // 63, (bg & 31) * 255 // 31))
        preview.paste(small, ((128 - small.width) // 2, 16 + (104 - small.height) // 2))
        previews.append(preview.resize((384, 480), Image.Resampling.NEAREST))
    offsets.append(len(blob))
    width, height = fitted.size
    # 元数据随资源一起生成，源文件与二进制文件均纳入工程。
    metadata = ["// Copyright (c) 2026 Ye；由 prepare_cat_gif.py 生成，勿手动修改。",
                "#pragma once", "#include <stdint.h>",
                f"#define CAT_GIF_WIDTH {width}", f"#define CAT_GIF_HEIGHT {height}",
                f"#define CAT_GIF_FRAMES {len(frames)}", f"#define CAT_GIF_PERIOD_MS {sum(durations)}",
                f"#define CAT_GIF_BACKGROUND 0x{rgb565(background):04x}",
                "static const uint32_t cat_gif_offsets[] = {" + ",".join(map(str, offsets)) + "};",
                "static const uint32_t cat_gif_durations[] = {" + ",".join(map(str, durations)) + "};", ""]
    (ROOT / "main/moon_cat_gif_data.h").write_text("\n".join(metadata))
    (ROOT / "main/moon_cat_gif.bin").write_bytes(blob)
    output = ROOT / "build/moon-cat-original-preview"
    output.mkdir(parents=True, exist_ok=True)
    previews[0].save(output / "screen.gif", save_all=True, append_images=previews[1:], duration=durations, loop=0)
    previews[0].save(output / "first-frame.png")
    print(f"{len(frames)} 帧，周期 {sum(durations)}ms，角色区域 {width}×{height}，资源 {len(blob)} 字节；全部帧压缩往返一致。")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python3 scripts/prepare_cat_gif.py /path/to/animation.gif")
    convert(Path(sys.argv[1]))
