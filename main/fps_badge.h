// Copyright (c) 2026 Ye；左上角圆润像素字体的 FPS 徽标。
#pragma once
#include <stdint.h>

// 在完整 RGB565 帧缓冲的前 16 行绘制；负数表示正在采样。
void fps_badge_draw(uint8_t *pixels, int fps);
