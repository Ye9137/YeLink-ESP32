"""将一张普通照片转换为 ST7735 屏幕可直接使用的 RGB565 像素文件。

用法：
    python3 scripts/prepare_photo.py /path/to/photo.jpg

依赖：
    只需在执行转换的电脑上安装 Pillow：python3 -m pip install Pillow

输出格式：
    - 分辨率：128×160
    - 每个像素占 2 字节
    - 色彩格式：RGB565
    - 字节顺序：高字节在前，低字节在后
    - 输出位置：工程的 main/photo.rgb565
"""

# Path 用来安全地处理输入、输出文件路径。
from pathlib import Path
# sys 用来读取命令行中传入的照片路径。
import sys

# Image 负责打开、缩放和保存图片数据；ImageOps 用来处理 EXIF 方向。
from PIL import Image, ImageOps


# 目标屏幕的物理分辨率。这两个值必须与 main.c 保持一致。
WIDTH = 128
HEIGHT = 160

# __file__ 是本脚本的位置。
# parents[1] 返回工程根目录，然后拼出输出文件 main/photo.rgb565。
OUTPUT = Path(__file__).resolve().parents[1] / "main" / "photo.rgb565"


def convert(source: Path) -> None:
    """读取 source 照片，将其转换后写入 OUTPUT 指定的 RGB565 文件。"""

    # 使用 with 打开照片，离开代码块后文件会自动关闭。
    with Image.open(source) as opened:
        # 手机照片可能通过 EXIF 标记方向；先按该标记旋转，再统一转为 RGB。
        photo = ImageOps.exif_transpose(opened).convert("RGB")

    # 在不裁剪原图的前提下等比例缩小，使其完整放入 128×160 区域。
    # LANCZOS 在缩小图片时能获得较平滑的结果。
    photo = ImageOps.contain(photo, (WIDTH, HEIGHT), Image.Resampling.LANCZOS)

    # 创建一张与屏幕相同大小的黑色背景。
    # 原图宽高比与屏幕不同时，空余区域会显示为黑边。
    canvas = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))

    # 计算左上角坐标，把缩放后的照片水平、垂直居中放到黑色背景上。
    canvas.paste(photo, ((WIDTH - photo.width) // 2, (HEIGHT - photo.height) // 2))

    # RGB565 每个像素占 16 位，也就是 2 字节，因此总大小为宽×高×2。
    pixels = bytearray(WIDTH * HEIGHT * 2)

    # 转为连续的 RGB888 字节：每个像素依次包含红、绿、蓝三个字节。
    rgb = canvas.tobytes()

    # 逐像素将 24 位 RGB888 压缩为 16 位 RGB565。
    for i in range(WIDTH * HEIGHT):
        # 每个 RGB888 像素占 3 字节，按 R、G、B 顺序读取。
        red, green, blue = rgb[i * 3 : i * 3 + 3]

        # RGB565 的位分配为：红色 5 位、绿色 6 位、蓝色 5 位。
        # 0xF8 保留红色高 5 位，0xFC 保留绿色高 6 位，蓝色右移保留高 5 位。
        rgb565 = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)

        # ST7735 通过 SPI 接收 16 位颜色时先接收高字节，再接收低字节。
        pixels[i * 2] = rgb565 >> 8
        pixels[i * 2 + 1] = rgb565 & 0xFF

    # 一次性把转换好的裸像素数据写入文件。
    OUTPUT.write_bytes(pixels)

    # 输出转换结果，便于核对缩放尺寸、目标路径和最终字节数。
    print(f"{source} -> {OUTPUT} ({photo.width}x{photo.height} within {WIDTH}x{HEIGHT}, {len(pixels)} bytes)")


# 只有直接运行本文件时才执行下面的命令行入口；被其他脚本导入时不会执行。
if __name__ == "__main__":
    # sys.argv[0] 是脚本自身，sys.argv[1] 应当是唯一的照片路径参数。
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python3 scripts/prepare_photo.py /path/to/photo.jpg")

    # 将命令行字符串转换为 Path 对象并开始转换。
    convert(Path(sys.argv[1]))
