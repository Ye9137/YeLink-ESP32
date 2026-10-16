// Copyright (c) 2026 Ye；IPv4 分片在 NAPT 解析 TCP/UDP 头之前完成重组。
#include "router_ip4.h"
#include "router_tcp_mss.h"
#include "sdkconfig.h"
#include "ye_lwip_hooks.h"
#include "wifi_share.h"
#include "lwip/ip.h"
#include "lwip/ip4.h"
#include "lwip/ip4_frag.h"
#include "lwip/inet_chksum.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/ip.h"

static router_ip4_stats_t counters;

void router_ip4_get_stats(router_ip4_stats_t *stats)
{
    LWIP_ASSERT_CORE_LOCKED();
    *stats=counters;
}

static int drop_fragment(struct pbuf *packet)
{
    counters.input_drops++;
    pbuf_free(packet);
    return 1;
}

int ye_router_ip4_input(struct pbuf *packet, struct netif *input)
{
    LWIP_ASSERT_CORE_LOCKED();
    if (packet->len<IP_HLEN) return wifi_share_ready()?drop_fragment(packet):0;
    const struct ip_hdr *header=packet->payload;
    uint16_t offset=lwip_ntohs(IPH_OFFSET(header));
    if (!(offset&(IP_OFFMASK|IP_MF))) {
        // 只在 TCP 握手时查询共享状态；普通数据仍走原路径。
        unsigned ihl=IPH_HL_BYTES(header);
        if (IPH_PROTO(header)==IP_PROTO_TCP && ihl>=IP_HLEN && packet->len>=ihl+20 &&
            (((const uint8_t *)packet->payload)[ihl+13]&2) && wifi_share_ready() &&
            router_tcp_clamp_mss(packet->payload,packet->len,packet->tot_len,CONFIG_YE_ROUTER_TCP_MSS))
            counters.tcp_mss_clamped++;
        return 0;
    }
    if (!wifi_share_ready()) return 0;
    counters.fragments++;
#if IP_REASSEMBLY
    uint16_t length=lwip_ntohs(IPH_LEN(header));
    // hook 位于 lwIP 长度/校验和检查之前，必须先独立校验分片边界。
    if (IPH_HL_BYTES(header)!=IP_HLEN || length<=IP_HLEN || length>packet->tot_len ||
        (offset&IP_DF) || ((offset&IP_MF) && ((length-IP_HLEN)&7)) ||
        (uint32_t)(offset&IP_OFFMASK)*8+length>65535 ||
        inet_chksum(header,IP_HLEN)!=0)
        return drop_fragment(packet);
    pbuf_realloc(packet,length);
    // 只复制分片，及时归还 Wi-Fi RX 缓冲；重组缓存由 MAX_PBUFS 和超时约束。
    struct pbuf *owned=pbuf_clone(PBUF_RAW,PBUF_RAM,packet);
    pbuf_free(packet);
    if (!owned) { counters.input_drops++; return 1; }
    // ip4_reass 在按网卡生成校验和时需要输入网卡上下文。
    struct netif *saved_input=ip_data.current_input_netif;
    ip_data.current_input_netif=input;
    struct pbuf *whole=ip4_reass(owned); // 无论是否完成，owned 的所有权均交给 lwIP。
    ip_data.current_input_netif=saved_input;
    if (!whole) return 1;
    // 连续化完整报文，避免极小首片导致 NAPT 越过首个 pbuf 读取传输层头。
    whole=pbuf_coalesce(whole,PBUF_RAW);
    if (whole->next || whole->len!=whole->tot_len) return drop_fragment(whole);
    const struct ip_hdr *complete=whole->payload;
    unsigned minimum=IP_HLEN;
    if (IPH_PROTO(complete)==IP_PROTO_TCP) minimum+=20;
    else if (IPH_PROTO(complete)==IP_PROTO_UDP || IPH_PROTO(complete)==IP_PROTO_ICMP) minimum+=8;
    if (whole->tot_len<minimum) return drop_fragment(whole);
    counters.reassembled++;
    // 完整包已清除 MF/offset；再次进入 hook 会直接返回 0，不会重复重组。
    (void)ip4_input(whole,input);
    return 1;
#else
    return drop_fragment(packet);
#endif
}
