#!/usr/bin/env python3
"""Run actual MSS source against packet/checksum regressions on the host only."""
from pathlib import Path
import shutil
import subprocess
import tempfile

BASE = Path(__file__).resolve().parents[1]
TEST = r'''
#include "router_tcp_mss.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned read16(const unsigned char *p){return ((unsigned)p[0]<<8)|p[1];}
static void write16(unsigned char *p,unsigned x){p[0]=x>>8;p[1]=x;}
static unsigned checksum(const unsigned char *p,unsigned n,unsigned initial){
 unsigned sum=initial;
 for(unsigned i=0;i<n;i+=2)sum+=((unsigned)p[i]<<8)|(i+1<n?p[i+1]:0);
 while(sum>>16)sum=(sum&65535)+(sum>>16);
 return (~sum)&65535;
}
static unsigned pseudo(const unsigned char *p){
 unsigned sum=6+read16(p+2)-(p[0]&15)*4;
 for(unsigned i=12;i<20;i+=2)sum+=read16(p+i);
 return sum;
}
static unsigned make(unsigned char *p,unsigned ihl,unsigned nops,unsigned mss,unsigned flags){
 memset(p,0,128);p[0]=0x40|ihl/4;p[9]=6;p[8]=64;
 p[12]=192;p[13]=168;p[14]=4;p[15]=2;p[16]=203;p[17]=0;p[18]=113;p[19]=7;
 unsigned char *t=p+ihl;write16(t,54321);write16(t+2,443);t[13]=flags;
 unsigned pos=20;
 for(unsigned i=0;i<nops;i++)t[pos++]=1;
 t[pos++]=2;t[pos++]=4;write16(t+pos,mss);pos+=2;
 unsigned thl=(pos+3)&~3; t[12]=(thl/4)<<4;
 write16(p+2,ihl+thl);write16(t+16,checksum(t,thl,pseudo(p)));
 write16(p+10,checksum(p,ihl,0));return ihl+thl;
}
int main(void){
 unsigned char p[128],original[128];unsigned n;
 // Full checksum recomputation independently checks incremental adjustment,
 // odd option alignment, IP options, SYN-ACK, and all 16-bit MSS values.
 for(unsigned ihl=20;ihl<=24;ihl+=4)for(unsigned nop=0;nop<4;nop++)
 for(unsigned mss=1;mss<=65535;mss++){
  n=make(p,ihl,nop,mss,mss&1?2:18);memcpy(original,p,128);
  assert(router_tcp_clamp_mss(p,n,n,1360)==(mss>1360));
  assert(read16(p+ihl+22+nop)==(mss>1360?1360:mss));
  assert(checksum(p+ihl,n-ihl,pseudo(p))==0);
  assert(checksum(p,ihl,0)==0);
  if(mss<=1360)assert(!memcmp(original,p,128));
  assert(router_tcp_clamp_mss(p,n,n,1360)==0);
 }
 // No MSS option, non-SYN, RST, UDP, fragments, bad option lengths, EOL,
 // truncated TCP/IP headers, and packet chains must not be over-read or edited.
 for(unsigned test=0;test<12;test++){
  n=make(p,20,0,1460,2);unsigned contiguous=n,total=n;
  switch(test){
   case 0:p[40]=0;break;
   case 1:p[33]=16;break;
   case 2:p[33]=6;break;
   case 3:p[9]=17;break;
   case 4:p[6]=0x20;break;
   case 5:p[7]=1;break;
   case 6:p[41]=0;break;
   case 7:p[41]=5;break;
   case 8:contiguous=39;break;
   case 9:contiguous=n-1;break;
   case 10:total=n-1;break;
   case 11:p[32]=0xf0;break;
  }
  memcpy(original,p,128);assert(!router_tcp_clamp_mss(p,contiguous,total,1360));
  assert(!memcmp(original,p,128));
 }
 // Valid first MSS followed by a malformed option must remain wholly unchanged.
 n=make(p,20,0,1460,2);p[32]=0x70;write16(p+2,48);p[44]=3;p[45]=9;
 memcpy(original,p,128);assert(!router_tcp_clamp_mss(p,48,48,1360));assert(!memcmp(original,p,128));
 // Valid non-MSS option before MSS, at an odd offset.
 n=make(p,20,3,1460,2);p[40]=3;p[41]=3;p[42]=7;p[36]=p[37]=0;
 write16(p+36,checksum(p+20,n-20,pseudo(p)));
 assert(router_tcp_clamp_mss(p,n,n,1360)==1);assert(!checksum(p+20,n-20,pseudo(p)));
 // Deterministic malformed-input fuzzing with arbitrary lengths under sanitizers.
 unsigned random=7;
 for(unsigned test=0;test<20000;test++){
  for(unsigned i=0;i<128;i++){random=random*1664525+1013904223;p[i]=random>>24;}
  unsigned len=test%129;router_tcp_clamp_mss(p,len,len,1360);
 }
 puts("524280 MSS/checksum combinations, 14 boundary cases, and 20000 malformed packets passed");
}
'''

def main():
    compiler = shutil.which("cc")
    if not compiler:
        raise SystemExit("A host C compiler is required; ESP-IDF is not used.")
    with tempfile.TemporaryDirectory(prefix="router-mss-test-") as directory:
        work = Path(directory)
        (work / "test.c").write_text(TEST)
        output = work / "check"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(BASE / "main"),
                        str(BASE / "main/router_tcp_mss.c"), str(work / "test.c"), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)

if __name__ == "__main__":
    main()
