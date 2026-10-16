#!/usr/bin/env python3
"""Host checks for the actual IPv4 hook's validation and buffer ownership.

Uses mock lwIP allocation/reassembly boundaries. Does not build or flash firmware
and does not validate the real lwIP reassembler, NAPT, radio, or VPN endpoint.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

BASE = Path(__file__).resolve().parents[1]
MOCK = r'''
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <arpa/inet.h>
#include <assert.h>
#define LWIP_ASSERT_CORE_LOCKED() ((void)0)
#define IP_HLEN 20
#define IP_OFFMASK 0x1fff
#define IP_MF 0x2000
#define IP_DF 0x4000
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17
#define PBUF_RAW 0
#define PBUF_RAM 0
#define lwip_ntohs ntohs
struct ip_hdr { uint8_t vhl,tos; uint16_t len,id,offset; uint8_t ttl,proto; uint16_t checksum; uint32_t src,dst; };
_Static_assert(sizeof(struct ip_hdr)==20,"IPv4 header size");
#define IPH_OFFSET(h) ((h)->offset)
#define IPH_LEN(h) ((h)->len)
#define IPH_HL_BYTES(h) (((h)->vhl&15)*4)
#define IPH_PROTO(h) ((h)->proto)
struct pbuf { void *payload; uint16_t len,tot_len; struct pbuf *next; };
struct netif { int id; };
struct ip_globals { struct netif *current_input_netif; };
extern struct ip_globals ip_data;
bool wifi_share_ready(void);
uint16_t inet_chksum(const void *,uint16_t);
void pbuf_free(struct pbuf *);
void pbuf_realloc(struct pbuf *,uint16_t);
struct pbuf *pbuf_clone(int,int,const struct pbuf *);
struct pbuf *pbuf_coalesce(struct pbuf *,int);
struct pbuf *ip4_reass(struct pbuf *);
int ip4_input(struct pbuf *,struct netif *);
'''

TEST = r'''
#include "mock.h"
#include "router_ip4.h"
#include "ye_lwip_hooks.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct ip_globals ip_data;
static struct netif input={1},previous={2};
static bool ready=true,clone_fails,coalesce_fails,complete;
static unsigned allocated,freed,clones,forwarded,reass_calls;
static struct pbuf *pending;
uint16_t inet_chksum(const void *data,uint16_t n) {
 const uint8_t *b=data; uint32_t sum=0;
 for(unsigned i=0;i<n;i+=2)sum+=((uint16_t)b[i]<<8)|(i+1<n?b[i+1]:0);
 while(sum>>16)sum=(sum&65535)+(sum>>16);
 return htons((uint16_t)~sum);
}
bool wifi_share_ready(void){return ready;}
static struct pbuf *alloc_packet(uint16_t n){
 struct pbuf *p=calloc(1,sizeof(*p));assert(p);p->payload=calloc(1,n);assert(p->payload);
 p->len=p->tot_len=n;allocated++;return p;
}
void pbuf_free(struct pbuf *p){while(p){struct pbuf *next=p->next;free(p->payload);free(p);freed++;p=next;}}
void pbuf_realloc(struct pbuf *p,uint16_t n){assert(n<=p->tot_len);p->len=p->tot_len=n;}
struct pbuf *pbuf_clone(int l,int t,const struct pbuf *p){
 (void)l;(void)t;clones++;if(clone_fails)return NULL;
 struct pbuf *q=alloc_packet(p->tot_len);memcpy(q->payload,p->payload,p->len);return q;
}
struct pbuf *pbuf_coalesce(struct pbuf *p,int l){(void)l;assert(!p->next||coalesce_fails);return p;}
struct pbuf *ip4_reass(struct pbuf *p){
 assert(ip_data.current_input_netif==&input);reass_calls++;
 if(!complete){assert(!pending);pending=p;return NULL;}
 struct ip_hdr *h=p->payload;h->offset=0;h->checksum=0;h->checksum=inet_chksum(h,20);
 if(coalesce_fails){p->next=alloc_packet(8);p->tot_len+=8;}
 return p;
}
int ip4_input(struct pbuf *p,struct netif *n){
 assert(n==&input);assert(!p->next);assert(ip_data.current_input_netif==&previous);
 assert(ye_router_ip4_input(p,n)==0);forwarded++;pbuf_free(p);return 0;
}
static struct pbuf *packet(uint16_t flags,uint16_t payload){
 struct pbuf *p=alloc_packet(20+payload);struct ip_hdr *h=p->payload;
 h->vhl=0x45;h->len=htons(p->len);h->offset=htons(flags);h->ttl=64;h->proto=17;
 h->checksum=inet_chksum(h,20);return p;
}
static void reset(void){
 if(pending){pbuf_free(pending);pending=NULL;}assert(allocated==freed);
 ready=true;clone_fails=coalesce_fails=complete=false;
 ip_data.current_input_netif=&previous;
}
int main(void){
 struct pbuf *p;unsigned before;
 reset();p=packet(0,8);before=clones;
 assert(ye_router_ip4_input(p,&input)==0&&clones==before);pbuf_free(p);
 reset();ready=false;p=packet(IP_MF,8);assert(ye_router_ip4_input(p,&input)==0);pbuf_free(p);
 reset();p=packet(IP_MF,8);assert(ye_router_ip4_input(p,&input)==1&&pending);
 assert(ip_data.current_input_netif==&previous);
 reset();complete=true;p=packet(IP_MF,8);before=forwarded;
 assert(ye_router_ip4_input(p,&input)==1&&forwarded==before+1);
 reset();clone_fails=true;p=packet(IP_MF,8);before=reass_calls;
 assert(ye_router_ip4_input(p,&input)==1&&reass_calls==before);
 reset();complete=coalesce_fails=true;p=packet(IP_MF,8);before=forwarded;
 assert(ye_router_ip4_input(p,&input)==1&&forwarded==before);
 reset();p=packet(IP_MF,8);((struct ip_hdr *)p->payload)->checksum^=1;
 before=reass_calls;assert(ye_router_ip4_input(p,&input)==1&&reass_calls==before);
 reset();p=packet(IP_MF,7);assert(ye_router_ip4_input(p,&input)==1);
 reset();p=packet(IP_DF|IP_MF,8);assert(ye_router_ip4_input(p,&input)==1);
 reset();p=packet(IP_OFFMASK,8);assert(ye_router_ip4_input(p,&input)==1);
 reset();p=packet(IP_MF,8);((struct ip_hdr *)p->payload)->len=htons(80);
 assert(ye_router_ip4_input(p,&input)==1);
 reset();p=packet(IP_MF,8);((struct ip_hdr *)p->payload)->vhl=0x46;
 assert(ye_router_ip4_input(p,&input)==1);
 reset();complete=true;p=packet(1,8);((struct ip_hdr *)p->payload)->proto=6;
 ((struct ip_hdr *)p->payload)->checksum=0;
 ((struct ip_hdr *)p->payload)->checksum=inet_chksum(p->payload,20);
 before=forwarded;assert(ye_router_ip4_input(p,&input)==1&&forwarded==before);
 reset();
 router_ip4_stats_t stats;router_ip4_get_stats(&stats);
 assert(stats.fragments>0&&stats.reassembled==1&&stats.input_drops>0);
 // Exercise MSS through the real hook, including disabled sharing bypass.
 for(unsigned disabled=0;disabled<2;disabled++){
  reset();ready=!disabled;p=packet(0,24);unsigned char *ip=p->payload,*tcp=ip+20;
  ip[9]=6;tcp[12]=0x60;tcp[13]=2;tcp[20]=2;tcp[21]=4;tcp[22]=5;tcp[23]=0xb4;
  before=clones;assert(ye_router_ip4_input(p,&input)==0&&clones==before);
  assert(tcp[22]==5&&tcp[23]==(disabled?0xb4:0x50));pbuf_free(p);
 }
 reset();router_ip4_get_stats(&stats);assert(stats.tcp_mss_clamped==1);
 puts("15 hook paths passed: fragment ownership, reinjection, malformed packets, allocation failures and MSS integration");
}
'''

def main():
    compiler = shutil.which("cc")
    if not compiler:
        raise SystemExit("A host C compiler is required; ESP-IDF is not used.")
    with tempfile.TemporaryDirectory(prefix="router-ip4-test-") as directory:
        work = Path(directory)
        (work / "mock.h").write_text(MOCK)
        (work / "sdkconfig.h").write_text("#define CONFIG_YE_ROUTER_TCP_MSS 1360\n")
        (work / "wifi_share.h").write_text('#include "mock.h"\n')
        for name in ["ip.h", "ip4.h", "ip4_frag.h", "inet_chksum.h", "pbuf.h", "prot/ip4.h", "prot/ip.h"]:
            path = work / "lwip" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "mock.h"\n')
        for name in ["router_ip4.c", "router_ip4.h", "ye_lwip_hooks.h", "router_tcp_mss.c", "router_tcp_mss.h"]:
            shutil.copyfile(BASE / "main" / name, work / name)
        (work / "test.c").write_text(TEST)
        executable = work / "check"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DIP_REASSEMBLY=1",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(work),
                        str(work / "router_ip4.c"), str(work / "router_tcp_mss.c"), str(work / "test.c"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

if __name__ == "__main__":
    main()
