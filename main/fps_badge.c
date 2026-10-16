// Copyright (c) 2026 Ye；手工设计的圆角数字、小写 fps 和奶油粉徽标。
#include "fps_badge.h"
#include "display.h"
#include <stdio.h>

// 每行低五位为像素，圆角轮廓避免默认字形的生硬直角。
static const uint8_t digits[10][7] = {
    {14,27,27,27,27,27,14}, {6,14,6,6,6,6,15},
    {14,27,3,6,12,24,31}, {14,27,3,14,3,27,14},
    {6,14,22,22,31,6,6}, {31,24,24,30,3,27,14},
    {14,24,24,30,27,27,14}, {31,3,6,6,12,12,12},
    {14,27,27,14,27,27,14}, {14,27,27,15,3,3,14}
};
static const uint8_t lower_f[7] = {6,12,12,30,12,12,12};
static const uint8_t lower_p[7] = {0,0,30,27,27,30,24};
static const uint8_t lower_s[7] = {0,0,15,24,14,3,30};
static const uint8_t dash[7] = {0,0,0,14,0,0,0};

// 字形和背景范围固定，输出与屏幕一致的高字节在前 RGB565。
static void put(uint8_t *pixels, int x, int y, uint16_t color)
{
    size_t pos = (y * SCREEN_WIDTH + x) * 2;
    pixels[pos] = color >> 8;
    pixels[pos + 1] = color & 255;
}

void fps_badge_draw(uint8_t *pixels, int fps)
{
    // 56×14 胶囊形底色，左上高光和底部浅粉阴影；不越过动画起始行。
    for (int y = 1; y <= 14; y++) {
        int inset = (y == 1 || y == 14) ? 4 : (y == 2 || y == 13) ? 2 : 0;
        for (int x = 2 + inset; x < 58 - inset; x++)
            put(pixels,x,y,y == 14 ? 0xe5f8 : y == 1 ? 0xffbf : 0xff1c);
    }
    char label[12];
    if (fps < 0) snprintf(label,sizeof(label),"-- fps");
    else snprintf(label,sizeof(label),"%d fps",fps > 999 ? 999 : fps);
    for (int i = 0; label[i]; i++) {
        const uint8_t *glyph = label[i] >= '0' && label[i] <= '9' ? digits[label[i]-'0'] :
            label[i] == 'f' ? lower_f : label[i] == 'p' ? lower_p :
            label[i] == 's' ? lower_s : label[i] == '-' ? dash : NULL;
        if (!glyph) continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (glyph[row] & (1 << (4-col))) put(pixels,7+i*6+col,4+row,0x8a49);
    }
}
