// Copyright (c) 2026 Ye；完整校验游程长度、调色板索引与资源边界。
#include "custom_animation.h"
#include "display.h"
#include "gif_scale.h"
#include <string.h>
static unsigned u16(const uint8_t *p) { return ((unsigned)p[0]<<8)|p[1]; }
static uint32_t u32(const uint8_t *p) { return ((uint32_t)u16(p)<<16)|u16(p+2); }

bool custom_animation_load(custom_animation_t *a, uint8_t *data, size_t length)
{
    if (!a || !data || length<16 || length>CUSTOM_ANIMATION_MAX || memcmp(data,"YEA1",4)) return false;
    memset(a,0,sizeof(*a));
    a->width=u16(data+4); a->height=u16(data+6); a->count=u16(data+8);
    a->background=u16(data+10); a->period=u32(data+12);
    if (!a->width || a->width>128 || !a->height || a->height>104 || !a->count || a->count>120) return false;
    size_t pos=16; uint32_t duration=0;
    for (int i=0;i<a->count;i++) {
        if (length-pos<38) return false;
        a->offsets[i]=pos; a->durations[i]=u16(data+pos);
        if (a->durations[i]<20 || a->durations[i]>10000) return false;
        duration+=a->durations[i];
        uint32_t bytes=u32(data+pos+2); pos+=38;
        if (bytes>length-pos) return false;
        size_t end=pos+bytes, pixels=0;
        while (pos<end) {
            unsigned token=data[pos++], run=(token&127)+1;
            unsigned needed=token&128?1:run;
            if (needed>end-pos || pixels+run>a->width*a->height) return false;
            for (unsigned j=0;j<needed;j++) if (data[pos+j]>=16) return false;
            pos+=needed; pixels+=run;
        }
        if (pixels!=a->width*a->height) return false;
    }
    if (pos!=length || duration!=a->period) return false;
    a->data=data; a->length=length;
    return true;
}

int custom_animation_frame(const custom_animation_t *a, int64_t elapsed_us)
{
    uint32_t ms=(elapsed_us<0?0:elapsed_us/1000)%a->period;
    for (int i=0;i<a->count;i++) { if (ms<a->durations[i]) return i; ms-=a->durations[i]; }
    return 0;
}

void custom_animation_draw(const custom_animation_t *a, uint8_t *pixels, int index)
{
    if (index<0 || index>=a->count) return;
    size_t pos=a->offsets[index];
    const uint8_t *palette=a->data+pos+6;
    size_t end=pos+38+u32(a->data+pos+2); pos+=38;
    int x=0,y=0;
    uint16_t row[SCREEN_WIDTH];
    // 仅缓存一行；压缩资源保持小尺寸，显示时放大铺满，减少网络传输量。
    while (pos<end) {
        unsigned token=a->data[pos++], run=(token&127)+1;
        for (unsigned j=0;j<run;j++) {
            unsigned color=a->data[pos+(token&128?0:j)]*2;
            row[x]=u16(palette+color);
            if (++x==a->width) { gif_scale_row(pixels,row,a->width,a->height,y++); x=0; }
        }
        pos+=token&128?1:run;
    }
}
