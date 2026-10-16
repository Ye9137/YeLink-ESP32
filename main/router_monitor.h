// Copyright (c) 2026 Ye；上游接口速率与有界网络事件日志。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

#define ROUTER_LOG_ROWS 4
#define ROUTER_LOG_COLS 20
typedef struct {
    uint32_t upload_bytes_per_sec, download_bytes_per_sec;
    uint32_t cpu_mhz, heap_free, heap_min;
    uint8_t cpu_percent[2], cpu_total_percent;
    bool cpu_sample_valid;
    unsigned log_count;
    char logs[ROUTER_LOG_ROWS][ROUTER_LOG_COLS+1];
} router_snapshot_t;

// 必须在 Wi-Fi 启动前调用；云端模式不调用，收发路径直接透传。
void router_monitor_init(esp_netif_t *upstream);
void router_monitor_log(const char *format, ...) __attribute__((format(printf,1,2)));
// 只由显示任务每秒调用一次，速率为上游网卡字节差值/实际采样间隔。
void router_monitor_snapshot(router_snapshot_t *snapshot);
