#!/usr/bin/env python3
"""Copyright (c) 2026 Ye；在电脑上运行实际 C 绘图代码，导出预览并检查局部刷新边界。"""
import ctypes
from pathlib import Path
import subprocess
import sys

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "moon-cat-preview"
OUT.mkdir(parents=True, exist_ok=True)

# 与固件共用 moon_cat.c，而非在 Python 中另画一个不同的角色。
library = OUT / ("renderer.dylib" if sys.platform == "darwin" else "renderer.so")
subprocess.run(["cc", "-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC", "-O2",
                "-I", str(ROOT / "main"), str(ROOT / "main/moon_cat.c"), "-lm", "-o", str(library)], check=True)
renderer = ctypes.CDLL(str(library)).moon_cat_render
renderer.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int64, ctypes.c_int,
                     ctypes.c_int, ctypes.c_int, ctypes.c_bool]
renderer.restype = None
buffer = (ctypes.c_uint8 * (128 * 160 * 2))()


def render(now_us, expression=0, look=0, theme=0, blink=False):
    """把真实的 RGB565 帧展开为预览 RGB，保持固件中的色彩量化。"""
    renderer(buffer, now_us, expression, look, theme, blink)
    raw = bytes(buffer)
    rgb = bytearray()
    for index in range(0, len(raw), 2):
        value = (raw[index] << 8) | raw[index + 1]
        r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
        rgb.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return Image.frombytes("RGB", (128, 160), bytes(rgb)), raw


# 预览采样为 16FPS，固件仍以配置的 60FPS 目标运行。
frames = [render(1_000_000 + i * 62_500)[0].resize((384, 480), Image.Resampling.NEAREST) for i in range(64)]
frames[0].save(OUT / "moon-cat.gif", save_all=True, append_images=frames[1:], duration=63, loop=0)
sheet = Image.new("RGB", (384 * 4, 510), "#fffdf9")
labels = ["NORMAL", "HAPPY", "SLEEPY", "SURPRISED"]
for state in range(4):
    picture, _ = render(6_000_000 + state * 1_000_000, state, 1)
    sheet.paste(picture.resize((384, 480), Image.Resampling.NEAREST), (state * 384, 30))
    ImageDraw.Draw(sheet).text((state * 384 + 140, 12), labels[state], fill="#6a4334")
sheet.save(OUT / "expressions.png")

# 检查所有表情、视线、颜色和手动眨眼下，变化都不会越出局部刷新区域。
_, reference = render(11_000_000)
outside = reference[:16 * 128 * 2] + reference[120 * 128 * 2:]
clock = 12_000_000
for expression in range(4):
    for look in range(6):
        for theme in range(5):
            for step in range(5):
                _, raw = render(clock, expression, look, theme, step == 0)
                assert raw[:16 * 128 * 2] + raw[120 * 128 * 2:] == outside, "动画超出局部刷新区域"
                clock += 62_500
print("600 帧边界检查通过；预览：", OUT)
