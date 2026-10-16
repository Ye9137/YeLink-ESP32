// Copyright (c) 2026 Ye；用户提供的原 GIF 动画播放器。
#pragma once
#include <stdbool.h>
#include <stdint.h>

// 根据真实经过时间选择源动画帧，保持原始节奏，慢帧不积压。
int moon_cat_gif_frame(int64_t elapsed_us);
// 写入铺满屏幕的 RGB565 画布，居中裁剪，失败时由调用方回退。
bool moon_cat_gif_render(uint8_t *pixels, int index);
