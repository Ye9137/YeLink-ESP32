// Copyright (c) 2026 Ye；参考用户提供的月薪猫截图重绘的小屏动画。
#pragma once
#include <stdbool.h>
#include <stdint.h>

// 输出完整 128×160、大端 RGB565 画面；活动范围固定为第 16～119 行。
// expression: 日常/开心/困困/惊讶；look/theme 沿用后台枚举。
void moon_cat_render(uint8_t *pixels, int64_t now_us, int expression, int look,
                     int theme, bool blink_requested);
// 互动表情参数：眼睛开合百分比、身体摆幅、抬爪偏移、RGB565 眼睛颜色。
void moon_cat_set_style(int eye_open, int sway, int paw, uint16_t eye_color);
