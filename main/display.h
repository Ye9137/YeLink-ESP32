// Copyright (c) 2026 Ye；屏幕只由动画任务访问，避免 SPI 并发写入。
#pragma once
#include <stdint.h>
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 160
#define SCREEN_BYTES (SCREEN_WIDTH * SCREEN_HEIGHT * 2)
void display_init(void);
// pixels 必须指向完整的 40960 字节 DMA 内存，高字节在前的 RGB565 图像。
void display_frame(const uint8_t *pixels);
// 只提交指定的连续整行；返回前等待 DMA 完成，调用方随后可复用帧缓冲。
void display_rows(const uint8_t *pixels, int first_row, int row_count);
void display_enable(void);
