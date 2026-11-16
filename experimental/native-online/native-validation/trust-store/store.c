/* SPDX-License-Identifier: AGPL-3.0-only */
/* CertStore framing from OpenPak switch-patcher; RomFS from public libnx headers. */
#include "store.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void wr32(uint8_t *p, uint32_t v) {
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));
}
static void wr64(uint8_t *p, uint64_t v) {
    for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i));
}
int rc_store_replace_expired_root(const uint8_t *s, size_t n,
    const uint8_t *ca, size_t cn, uint8_t **out, size_t *on) {
    if (!out || !on) return -1;
    *out=NULL; *on=0;
    if (!s || !ca || n<8 || n>128*1024 || !cn || cn>8192 || rd32(s)!=0x546c7373) return -1;
    const uint32_t count=rd32(s+4);
    if (!count || count>512 || count>(n-8)/16) return -1;
    size_t target=0;
    for (uint32_t i=0;i<count;i++) {
        const size_t p=8+i*16;
        const uint32_t id=rd32(s+p), len=rd32(s+p+8), off=rd32(s+p+12);
        if (!len || off<count*16 || off>n-8 || len>n-8-off) return -1;
        for (uint32_t j=0;j<i;j++) {
            const size_t q=8+j*16;
            const uint32_t jo=rd32(s+q+12), jl=rd32(s+q+8);
            if (rd32(s+q)==id || (off<jo+jl && jo<off+len)) return -1;
        }
        if (id==1033) {
            if (rd32(s+p+4)!=1) return -1;
            target=p;
        }
    }
    if (!target) return -1;
    const size_t end=(n+3)&~(size_t)3;
    uint8_t *b=calloc(1,end+cn);
    if (!b) return -1;
    memcpy(b,s,n);
    wr32(b+target+8,(uint32_t)cn);
    wr32(b+target+12,(uint32_t)(end-8));
    memcpy(b+end,ca,cn);
    *out=b; *on=end+cn;
    return 0;
}

/* A one-file RomFS with three-bucket hash tables and one root directory. */
int rc_store_romfs(const uint8_t *s, size_t n, uint8_t **out, size_t *on) {
    static const char name[]="ssl_TrustedCerts.bdf";
    if (!out || !on) return -1;
    *out=NULL; *on=0;
    if (!s || n<8 || n>144*1024 || rd32(s)!=0x546c7373) return -1;
    const size_t dh=80, dt=92, fh=116, ft=128;
    const size_t fts=(32+sizeof(name)-1+3)&~(size_t)3;
    const size_t data=(ft+fts+15)&~(size_t)15;
    uint8_t *b=calloc(1,data+n);
    if (!b) return -1;
    const uint64_t header[]={80,dh,12,dt,24,fh,12,ft,fts,data};
    for (unsigned i=0;i<10;i++) wr64(b+8*i,header[i]);
    memset(b+dh,0xff,12); memset(b+fh,0xff,12);
    wr32(b+dh+4*(123456789%3),0);
    wr32(b+dt,0); wr32(b+dt+4,UINT32_MAX); wr32(b+dt+8,UINT32_MAX);
    wr32(b+dt+12,0); wr32(b+dt+16,UINT32_MAX); wr32(b+dt+20,0);
    uint32_t hash=123456789;
    for (size_t i=0;i<sizeof(name)-1;i++) hash=((hash>>5)|(hash<<27))^(uint8_t)name[i];
    wr32(b+fh+4*(hash%3),0);
    wr32(b+ft,0); wr32(b+ft+4,UINT32_MAX); wr64(b+ft+8,0); wr64(b+ft+16,n);
    wr32(b+ft+24,UINT32_MAX); wr32(b+ft+28,sizeof(name)-1);
    memcpy(b+ft+32,name,sizeof(name)-1); memcpy(b+data,s,n);
    *out=b; *on=data+n;
    return 0;
}
