// Copyright (c) 2026 Ye；AP + STA 的 IPv4 网络共享与配网 DNS。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

// 在 Wi-Fi 启动前绑定网卡；DNS 由 DHCP 直接下发，不经过应用层代理。
void wifi_share_init(esp_netif_t *ap, esp_netif_t *sta);
// 由 Wi-Fi/IP 事件线程调用：上游取得地址时启用 NAPT，断线时关闭。
void wifi_share_update(bool connected);
bool wifi_share_ready(void);
bool wifi_share_conflict(void);
uint32_t wifi_share_dns(void);

typedef struct {
    uint32_t active_tcp, active_udp, active_icmp, forced_evictions;
    uint32_t ipv4_fragments, ipv4_reassembled, ipv4_fragment_input_drops;
    uint32_t tcp_mss_clamped;
} wifi_share_stats_t;
// 仅从 HTTP/应用任务调用；在 lwIP 线程内读取统计，不能从 lwIP 回调中调用。
bool wifi_share_get_stats(wifi_share_stats_t *stats);
// 路由维护任务低频调用；队列满时跳过，不阻塞上游重连。
void wifi_share_poll_stats(void);
