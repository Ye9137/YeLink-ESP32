// Copyright (c) 2026 Ye；仅播放器代码，角色素材的权利归原作者。
#include "moon_cat_gif.h"
#include "moon_cat_gif_data.h"
#include "display.h"
#include "gif_scale.h"
#include <stddef.h>

extern const uint8_t cat_data[] asm("_binary_moon_cat_gif_bin_start");
extern const uint8_t cat_end[] asm("_binary_moon_cat_gif_bin_end");

_Static_assert(CAT_GIF_WIDTH <= SCREEN_WIDTH && CAT_GIF_HEIGHT <= 104,
               "GIF must fit inside the partial refresh region");

int moon_cat_gif_frame(int64_t elapsed_us)
{
    uint32_t time_ms = (uint32_t)((elapsed_us < 0 ? 0 : elapsed_us / 1000) % CAT_GIF_PERIOD_MS);
    for (int i = 0; i < CAT_GIF_FRAMES; i++) {
        if (time_ms < cat_gif_durations[i]) return i;
        time_ms -= cat_gif_durations[i];
    }
    return 0;
}

bool moon_cat_gif_render(uint8_t *pixels, int index)
{
    if (!pixels || index < 0 || index >= CAT_GIF_FRAMES) return false;
    size_t begin = cat_gif_offsets[index], end = cat_gif_offsets[index + 1];
    if (begin > end || end > (size_t)(cat_end - cat_data) || (end - begin) % 4) return false;
    // 先检查像素总数与游程边界；资源有误时不留下半张损坏画面。
    size_t count = 0;
    for (size_t pos = begin; pos < end; pos += 4) {
        unsigned run = ((unsigned)cat_data[pos] << 8) | cat_data[pos + 1];
        if (!run || count + run > CAT_GIF_WIDTH * CAT_GIF_HEIGHT) return false;
        count += run;
    }
    if (count != CAT_GIF_WIDTH * CAT_GIF_HEIGHT) return false;
    // 原版与上传 GIF 同样逐行放大铺满，不增加整帧内存占用。
    uint16_t row[SCREEN_WIDTH];
    int x = 0, y = 0;
    for (size_t pos = begin; pos < end; pos += 4) {
        unsigned run = ((unsigned)cat_data[pos] << 8) | cat_data[pos + 1];
        while (run--) {
            row[x]=((uint16_t)cat_data[pos+2]<<8)|cat_data[pos+3];
            if (++x == CAT_GIF_WIDTH) { gif_scale_row(pixels,row,CAT_GIF_WIDTH,CAT_GIF_HEIGHT,y++); x=0; }
        }
    }
    return true;
}
