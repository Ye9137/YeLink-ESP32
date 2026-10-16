// Copyright (c) 2026 Ye；服务器转换的有界 16 色 GIF 播放器。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CUSTOM_ANIMATION_MAX (48 * 1024)
typedef struct {
    uint8_t *data;
    size_t length;
    uint16_t width, height, count, background;
    uint32_t period, offsets[120];
    uint16_t durations[120];
} custom_animation_t;
// 校验全部帧后才允许发布给显示任务；data 的释放由调用者负责。
bool custom_animation_load(custom_animation_t *anim, uint8_t *data, size_t length);
int custom_animation_frame(const custom_animation_t *anim, int64_t elapsed_us);
void custom_animation_draw(const custom_animation_t *anim, uint8_t *pixels, int index);
