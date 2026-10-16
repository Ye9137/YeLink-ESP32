// Copyright (c) 2026 Ye；逐行等比例铺满，使用一行缓存而非第二张帧缓冲。
#pragma once
#include "display.h"

// 把解码后的一行映射到 128×160；居中裁剪超出屏幕的边缘。
static inline void gif_scale_row(uint8_t *pixels, const uint16_t *row,
                                 int width, int height, int source_y)
{
    int scaled_w=SCREEN_WIDTH, scaled_h=(height*SCREEN_WIDTH+width-1)/width;
    if (scaled_h<SCREEN_HEIGHT) {
        scaled_h=SCREEN_HEIGHT; scaled_w=(width*SCREEN_HEIGHT+height-1)/height;
    }
    int crop_x=(scaled_w-SCREEN_WIDTH)/2, crop_y=(scaled_h-SCREEN_HEIGHT)/2;
    int first=(source_y*scaled_h+height-1)/height-crop_y;
    int end=((source_y+1)*scaled_h+height-1)/height-crop_y;
    if (first<0) first=0;
    if (end>SCREEN_HEIGHT) end=SCREEN_HEIGHT;
    for (int y=first;y<end;y++) {
        uint8_t *dest=pixels+y*SCREEN_WIDTH*2;
        for (int x=0;x<SCREEN_WIDTH;x++) {
            uint16_t color=row[(x+crop_x)*width/scaled_w];
            dest[x*2]=color>>8; dest[x*2+1]=color&255;
        }
    }
}
