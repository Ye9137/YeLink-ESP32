// Copyright (c) 2026 Ye
#pragma once
#include <stdint.h>
typedef struct {
    uint32_t fragments, reassembled, input_drops;
    uint32_t tcp_mss_clamped;
} router_ip4_stats_t;
// 只由 lwIP 线程中的统计回调调用。
void router_ip4_get_stats(router_ip4_stats_t *stats);
