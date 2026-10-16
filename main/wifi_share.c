// Copyright (c) 2026 Ye；使用 ESP-IDF 自带 NAPT 和 DHCP DNS，不做代理转发。
#include "wifi_share.h"
#include <string.h>
#include "sdkconfig.h"
#include "router_monitor.h"
#include "router_ip4.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/stats.h"
#include "lwip/lwip_napt.h"
#include "lwip/tcpip.h"

#define SHARE_READY BIT0
#define SHARE_CONFLICT BIT1
#define DHCPS_OFFER_DNS 0x02
static esp_netif_t *ap_netif, *sta_netif;
static EventGroupHandle_t state;
static uint32_t current_dns;
static const char *TAG="wifi_share";

void wifi_share_init(esp_netif_t *ap, esp_netif_t *sta)
{
    ap_netif=ap; sta_netif=sta; state=xEventGroupCreate();
    configASSERT(ap && sta && state);
}
bool wifi_share_ready(void) { return state && (xEventGroupGetBits(state)&SHARE_READY); }
bool wifi_share_conflict(void) { return state && (xEventGroupGetBits(state)&SHARE_CONFLICT); }
uint32_t wifi_share_dns(void) { return current_dns; }

#if LWIP_STATS && IP_NAPT_STATS
static struct stats_ip_napt napt_baseline;
static void record_napt_baseline(void *context)
{
    (void)context;
    // 本机 SDK 释放整张表时不会扣减活跃统计；以空表时的累计值作基线。
    ip_napt_get_stats(&napt_baseline);
}

static void collect_napt_stats(void *context)
{
    wifi_share_stats_t *out=context;
    struct stats_ip_napt stats;
    static STAT_COUNTER previous_evictions;
    ip_napt_get_stats(&stats);
    out->active_tcp=(STAT_COUNTER)(stats.nr_active_tcp-napt_baseline.nr_active_tcp);
    out->active_udp=(STAT_COUNTER)(stats.nr_active_udp-napt_baseline.nr_active_udp);
    out->active_icmp=(STAT_COUNTER)(stats.nr_active_icmp-napt_baseline.nr_active_icmp);
    out->forced_evictions=stats.nr_forced_evictions;
    router_ip4_stats_t fragments;
    router_ip4_get_stats(&fragments);
    out->ipv4_fragments=fragments.fragments;
    out->ipv4_reassembled=fragments.reassembled;
    out->ipv4_fragment_input_drops=fragments.input_drops;
    out->tcp_mss_clamped=fragments.tcp_mss_clamped;
    if (stats.nr_forced_evictions!=previous_evictions) {
        unsigned delta=(STAT_COUNTER)(stats.nr_forced_evictions-previous_evictions);
        previous_evictions=stats.nr_forced_evictions;
        router_monitor_log("NAT FULL %u",delta);
        ESP_LOGW(TAG,"NAPT forced evictions +%u; active %lu/%d",delta,
            (unsigned long)(out->active_tcp+out->active_udp+out->active_icmp),CONFIG_YE_NAPT_CAPACITY);
    }
}
#endif

bool wifi_share_get_stats(wifi_share_stats_t *stats)
{
    if (!stats) return false;
    memset(stats,0,sizeof(*stats));
#if LWIP_STATS && IP_NAPT_STATS
    // 同步等待回调完成，栈上上下文不会在函数返回后被网络线程访问。
    return tcpip_callback_wait(collect_napt_stats,stats)==ERR_OK;
#else
    return false;
#endif
}

void wifi_share_poll_stats(void)
{
#if LWIP_STATS && IP_NAPT_STATS
    // 上下文常驻，排队回调不会引用已退出函数的栈内存。
    static wifi_share_stats_t stats;
    (void)tcpip_try_callback(collect_napt_stats,&stats);
#endif
}

static esp_err_t set_client_dns(void)
{
    esp_netif_dns_info_t dns={0};
    if (esp_netif_get_dns_info(sta_netif,ESP_NETIF_DNS_MAIN,&dns)!=ESP_OK ||
        dns.ip.type!=ESP_IPADDR_TYPE_V4 || !dns.ip.u_addr.ip4.addr) {
        dns.ip.type=ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr=ESP_IP4TOADDR(223,5,5,5);
    }
    current_dns=dns.ip.u_addr.ip4.addr;
    // ESP-IDF 要求 DHCP server 停止后修改下发给客户端的 DNS。
    esp_err_t result=esp_netif_dhcps_stop(ap_netif);
    if (result!=ESP_OK && result!=ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return result;
    uint8_t offer=DHCPS_OFFER_DNS;
    result=esp_netif_dhcps_option(ap_netif,ESP_NETIF_OP_SET,ESP_NETIF_DOMAIN_NAME_SERVER,&offer,sizeof(offer));
    if (result==ESP_OK) result=esp_netif_set_dns_info(ap_netif,ESP_NETIF_DNS_MAIN,&dns);
    esp_err_t start=esp_netif_dhcps_start(ap_netif);
    if (result==ESP_OK && start!=ESP_OK && start!=ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) result=start;
    return result;
}

void wifi_share_update(bool connected)
{
    xEventGroupClearBits(state,SHARE_READY|SHARE_CONFLICT);
    esp_err_t disabled=esp_netif_napt_disable(ap_netif);
#if LWIP_STATS && IP_NAPT_STATS
    if (disabled==ESP_OK && tcpip_callback_wait(record_napt_baseline,NULL)!=ERR_OK)
        ESP_LOGW(TAG,"Cannot refresh NAPT statistics baseline");
#else
    (void)disabled;
#endif
    current_dns=0;
    if (!connected) return;
    esp_netif_ip_info_t ap,sta;
    if (esp_netif_get_ip_info(ap_netif,&ap)!=ESP_OK || esp_netif_get_ip_info(sta_netif,&sta)!=ESP_OK) return;
    uint32_t common=ap.netmask.addr & sta.netmask.addr;
    if ((ap.ip.addr&common)==(sta.ip.addr&common)) {
        xEventGroupSetBits(state,SHARE_CONFLICT);
        ESP_LOGE(TAG,"Upstream subnet overlaps 192.168.4.0/24; change upstream subnet");
        return;
    }
    esp_err_t result=esp_netif_set_default_netif(sta_netif);
    if (result==ESP_OK) result=set_client_dns();
    if (result==ESP_OK) result=esp_netif_napt_enable(ap_netif);
    if (result==ESP_OK) {
        xEventGroupSetBits(state,SHARE_READY);
        // 客户端必须重新获取租约，才能丢弃旧的 192.168.4.1 DNS 配置。
        esp_wifi_deauth_sta(0);
        ESP_LOGI(TAG,"IPv4 sharing enabled; clients will reconnect with upstream DNS");
    } else ESP_LOGE(TAG,"Cannot enable sharing: %s",esp_err_to_name(result));
}
