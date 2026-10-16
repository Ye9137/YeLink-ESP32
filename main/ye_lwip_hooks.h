// Copyright (c) 2026 Ye；在 ESP-IDF 默认 hooks 之外接入 IPv4 转发兼容处理。
#pragma once
struct pbuf;
struct netif;
#ifdef __cplusplus
extern "C" {
#endif
// 返回 1 表示缓冲区已被消费；返回 0 时仍由原 ip4_input 处理。
int ye_router_ip4_input(struct pbuf *packet, struct netif *input);
#ifdef __cplusplus
}
#endif
#define LWIP_HOOK_IP4_INPUT(packet,input) ye_router_ip4_input((packet),(input))
