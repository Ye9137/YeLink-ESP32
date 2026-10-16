// Copyright (c) 2026 Ye；仅修改握手包，普通 TCP 数据包不增加复制。
#include "router_tcp_mss.h"

static uint16_t read16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static void write16(uint8_t *p, uint16_t value) { p[0]=(uint8_t)(value>>8); p[1]=(uint8_t)value; }

unsigned router_tcp_clamp_mss(uint8_t *ip, size_t contiguous, size_t total, uint16_t limit)
{
    if (!ip || !limit || contiguous<20 || ip[0]>>4!=4 || ip[9]!=6) return 0;
    unsigned ihl=(ip[0]&15)*4, length=read16(ip+2);
    if (ihl<20 || length>total || length<ihl+20 || contiguous<ihl+20 ||
        (read16(ip+6)&0x3fff)) return 0;
    uint8_t *tcp=ip+ihl;
    if (!(tcp[13]&2) || (tcp[13]&4)) return 0;
    unsigned thl=(tcp[12]>>4)*4;
    if (thl<20 || ihl+thl>length || ihl+thl>contiguous) return 0;
    // First validate all options, so a malformed later option cannot leave a partial edit.
    for (unsigned i=20;i<thl;) {
        if (tcp[i]==0) break;
        if (tcp[i]==1) { i++; continue; }
        if (i+1>=thl || tcp[i+1]<2 || i+tcp[i+1]>thl ||
            (tcp[i]==2 && tcp[i+1]!=4)) return 0;
        i+=tcp[i+1];
    }
    unsigned changed=0;
    uint32_t sum=(uint16_t)~read16(tcp+16);
    for (unsigned i=20;i<thl;) {
        if (tcp[i]==0) break;
        if (tcp[i]==1) { i++; continue; }
        if (tcp[i]==2) {
            uint16_t old=read16(tcp+i+2);
            if (old>limit) {
                uint16_t before=old, after=limit;
                // NOP can place MSS on an odd byte; checksum words then use swapped bytes.
                if (i&1) { before=(uint16_t)((before<<8)|(before>>8)); after=(uint16_t)((after<<8)|(after>>8)); }
                sum+=(uint16_t)~before;
                sum+=after;
                while (sum>>16) sum=(sum&0xffff)+(sum>>16);
                write16(tcp+i+2,limit);
                changed++;
            }
        }
        i+=tcp[i+1];
    }
    if (changed) write16(tcp+16,(uint16_t)~sum);
    return changed;
}
