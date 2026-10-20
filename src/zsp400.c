#include "zsp400.h"
#include <string.h>

enum { F_Z=8, F_GT=16, F_GE=32, F_C=64, F_SV=256, F_V=1024 };
static int sx(unsigned n, unsigned bits) { return (int)(n & ((1u<<bits)-1)) - ((n & (1u<<(bits-1))) ? (1<<bits) : 0); }
void zsp400_reset(ZSP400 *s) {
    memset(s->r,0,sizeof s->r); memset(s->c,0,sizeof s->c);
    s->pc=0xf800; s->flags=0; s->fault=0; s->instructions=0;
    memset(s->shadow,0,sizeof s->shadow);
    memset(s->timer_reload,0,sizeof s->timer_reload);
    memset(s->timer_div,0,sizeof s->timer_div);
}
static void flags(ZSP400 *s, int32_t v, uint32_t result, int carry) {
    s->flags=(s->flags & ~(F_Z|F_GT|F_GE|F_C)) |
        (result==0 ? F_Z:0) | (v>=0 ? F_GE:0) | (v>0 ? F_GT:0) | (carry ? F_C:0);
}
static uint16_t arithmetic(ZSP400 *s,uint16_t a,uint16_t b,int sub,int compare) {
    uint32_t raw=sub ? (uint32_t)a+(uint16_t)~b+1 : (uint32_t)a+b;
    int32_t exact=sub ? (int32_t)(int16_t)a-(int16_t)b : (int32_t)(int16_t)a+(int16_t)b;
    int overflow=exact>32767 || exact< -32768;
    uint16_t result=(uint16_t)raw;
    if(!compare && (s->c[0]&16) && overflow) result=exact<0?0x8000:0x7fff;
    flags(s,compare?exact:(int16_t)result,result,raw>65535);
    s->flags=(s->flags&~F_V)|(overflow?(F_V|F_SV):0);
    return result;
}
static uint16_t control(ZSP400 *s,unsigned n) { return n==16 ? s->pc : n==8 ? s->flags : s->c[n]; }
static uint32_t pair(ZSP400 *s,unsigned x) { return s->r[x]|((uint32_t)s->r[x+1]<<16); }
static void put_pair(ZSP400 *s,unsigned x,uint32_t v) { s->r[x]=v; s->r[x+1]=v>>16; }
static unsigned normalize(uint32_t v,unsigned bits) {
    uint32_t mask=bits==32?UINT32_MAX:0xffff;
    v &= mask;
    if(!v) return 0;
    if(v==mask) return bits-1;
    if(v & (1u<<(bits-1))) v=(~v)&mask;
    unsigned leading=0;
    while(v >>= 1) ++leading;
    return bits-2-leading;
}
static int64_t product16(ZSP400 *s,uint16_t a,uint16_t b) {
    int64_t p=(int32_t)(int16_t)a*(int16_t)b;
    return s->c[0]&4?(p==0x40000000?0x7fffffff:p*2):p;
}
static uint32_t arithmetic32(ZSP400 *s,uint32_t a,uint32_t b,int sub,int compare,int carry_in) {
    uint64_t raw=sub ? (uint64_t)a+(uint32_t)~b+carry_in : (uint64_t)a+b+carry_in;
    int64_t exact=sub ? (int64_t)(int32_t)a-(int32_t)b-(1-carry_in) : (int64_t)(int32_t)a+(int32_t)b+carry_in;
    int overflow=exact>INT32_MAX || exact<INT32_MIN;
    uint32_t result=(uint32_t)raw;
    if(!compare && (s->c[0]&16) && overflow) result=exact<0?0x80000000u:0x7fffffff;
    int cmp=exact>0?1:exact<0?-1:0;
    flags(s,compare?cmp:(int32_t)result,result,raw>UINT32_MAX);
    s->flags=(s->flags&~F_V)|(overflow?(F_V|F_SV):0);
    return result;
}
static uint32_t shift32(ZSP400 *s,uint32_t a,unsigned n,unsigned op) {
    uint32_t v; int carry=0,overflow=0; n&=31;
    if(op==2 || op==4) {
        v=a<<n; carry=n?(a>>(32-n))&1:0;
        if(op==4) {
            int64_t exact=(int64_t)(int32_t)a*(INT64_C(1)<<n);
            overflow=exact>INT32_MAX || exact<INT32_MIN;
            if((s->c[0]&16) && overflow) v=exact<0?0x80000000u:0x7fffffff;
        }
    } else {
        carry=n?(a>>(n-1))&1:0;
        v=op==5?(uint32_t)((int32_t)a>>n):a>>n;
        if(op==5 && (s->c[0]&2) && carry) ++v;
    }
    flags(s,(int32_t)v,v,carry);
    s->flags=(s->flags&~F_V)|(overflow?(F_V|F_SV):0);
    return v;
}
static uint16_t shift16(ZSP400 *s,uint16_t a,unsigned n,unsigned op) {
    uint32_t v; int carry=0, overflow=0;
    n &= 15;
    if(op==2 || op==4) {
        v=(uint32_t)a<<n;
        carry=n ? (a>>(16-n))&1 : 0;
        if(op==4) {
            int32_t exact=(int32_t)(int16_t)a*(1<<n);
            overflow=exact>32767 || exact< -32768;
            if((s->c[0]&16) && overflow) v=exact<0?0x8000:0x7fff;
        }
    } else {
        carry=n ? (a>>(n-1))&1 : 0;
        v=op==5 ? (uint16_t)((int16_t)a>>n) : a>>n;
        if(op==5 && (s->c[0]&2) && carry) v=(uint16_t)(v+1);
    }
    flags(s,(int16_t)v,(uint16_t)v,carry);
    s->flags=(s->flags&~F_V)|(overflow?(F_V|F_SV):0);
    return v;
}
static void set_control(ZSP400 *s,unsigned n,uint16_t v,uint16_t *next) {
    if(n==15 && ((s->c[n]^v)&0x1000))
        for(unsigned i=0;i<8;i++) { uint16_t t=s->r[i+2]; s->r[i+2]=s->shadow[i]; s->shadow[i]=t; }
    if(n==8) s->flags=v;
    if(n==23 || n==24) {
        if(s->c[n] && !v) s->c[9] |= 1u<<(n-18);
        s->timer_reload[n-23]=v; s->timer_div[n-23]=0;
    }
    s->c[n]=v; if(n==16) *next=v;
}
void zsp400_request_irq(ZSP400 *s,unsigned line) { if(line<14) s->c[9]|=1u<<line; }
int zsp400_idle(const ZSP400 *s) {
    if(s->fault || !(s->c[15]&0x6000) || (s->c[15]&0x8000)) return 0;
    for(unsigned i=0;i<2;i++) if((s->c[1]>>(8*i))&128 && s->c[23+i]) return 0;
    return !((s->c[2]&0x8000) && (s->c[9]&s->c[2]&0x3fff)) && (s->flags&4u)==(s->c[9]?4u:0);
}
/* The on-chip SRAM where the memory mode maps it (zevio_dsp.c core_memory) */
static uint8_t *sram(ZSP400 *s,uint16_t a,int program) {
    uint16_t mode=s->c[15];
    if(program) return s->iram && a<0x4000 && (!(mode&2)||(mode&0x400)) ? s->iram+2u*a : NULL;
    return s->dram && a<0xa000 && !(mode&1) ? s->dram+2u*a : NULL;
}
static void tick(ZSP400 *s) {
    for(unsigned i=0;i<2;i++) {
        unsigned t=(s->c[1]>>(8*i))&255;
        if((t&128) && s->c[23+i] && ++s->timer_div[i]>=(t&63)+1) {
            s->timer_div[i]=0;
            if(--s->c[23+i]==0) {
                s->c[9]|=1u<<(5+i);
                if(t&64) s->c[23+i]=s->timer_reload[i];
            }
        }
    }
    s->flags=(s->flags&~4u)|(s->c[9]?4:0);
    if(!(s->c[2]&0x8000)) return;
    unsigned pending=s->c[9]&s->c[2]&0x3fff, best=14, priority=0;
    for(unsigned i=0;i<14;i++) if(pending&(1u<<i)) {
        unsigned p=i<8 ? (s->c[4]>>(2*i))&3 : (s->c[3]>>(2*(i-8)))&3;
        if(p>=(s->c[3]>>14) && (best==14 || p>=priority)) { best=i; priority=p; }
    }
    if(best==14) return;
    s->c[18]=s->pc; /* between instructions: PC is already the next instruction */
    s->c[2]=(s->c[2]&0x3fff)|((s->c[2]&0x8000)>>1);
    s->c[3]=(s->c[3]&0x0fff)|((s->c[3]&0xc000)>>2)|(priority<<14);
    s->c[9]&=~(1u<<best);
    s->pc=((s->c[15]&128)?0:0xf800)+0x80-8*best;
    s->c[15]&=~0x6000;
}
int zsp400_step(ZSP400 *s) {
    if(s->fault || (s->c[15]&0x8000)) return 0;
    tick(s);
    if(s->c[15]&0x6000) return 1; /* peripheral clocks still advance */
    uint16_t pc=s->pc, next=pc+1, w;
    const uint8_t *ip=sram(s,pc,1);
    w=ip ? (uint16_t)(ip[0]|ip[1]<<8) : s->read(s->ctx,pc,1);
    unsigned top=w>>12, op=(w>>8)&15, x=(w>>4)&15, y=w&15;
    uint16_t a=s->r[x], b=s->r[y];
    if(top==0) next=pc+sx(w,12);
    else if(top==1) { s->c[17]=next; next=pc+2*sx(w,12); }
    else if(top==2) s->r[op]=(s->r[op]&0xff00)|(w&255);
    else if(top==3) s->r[op]=(s->r[op]&255)|(w<<8);
    else if(top==4) {
        int take=0;
        switch(op) {
        case 0: take=!!(s->flags&F_Z); break;
        case 1: take=!(s->flags&F_Z); break;
        case 2: take=!!(s->flags&F_GE); break;
        case 3: take=!(s->flags&F_GE); break;
        case 4: take=!!(s->flags&F_GT); break;
        case 5: take=!(s->flags&F_GT); break;
        case 6: take=!!(s->flags&F_V); break;
        case 7: take=!(s->flags&F_V); break;
        case 8: take=!!(s->flags&F_C); break;
        case 9: take=!(s->flags&F_C); break;
        case 12: take=s->c[25]-- != 0; break;
        case 13: take=s->c[26]-- != 0; break;
        case 14: take=s->c[5]-- != 0; break;
        case 15: take=s->c[6]-- != 0; break;
        default: goto unsupported;
        }
        if(take) next=pc+(op>=12 ? (int)(w&255)-256 : sx(w,8));
    } else if(top==5 && (op&7)!=6) {
        unsigned dest=op&8?2:0, guard_shift=dest*4;
        unsigned kind=op&7;
        int accumulate=kind>=4 || !(op&2);
        int64_t product=product16(s,a,b);
        if(kind>=4) {
            if(w&0x11) goto unsupported;
            if(kind==7) product=product16(s,a,s->r[y+1])+product16(s,s->r[x+1],b);
            else if(kind==5) product=product16(s,s->r[x+1],s->r[y+1])-product;
            else product+=product16(s,s->r[x+1],s->r[y+1]);
        } else if(op&1) product=-product;
        uint32_t old=accumulate?pair(s,dest):0;
        int64_t rounded=product+((s->c[0]&1)?0x8000:0);
        int64_t exact=(int32_t)old+rounded;
        int overflow=exact>INT32_MAX || exact<INT32_MIN;
        int64_t full=accumulate?((int64_t)(int8_t)(s->c[7]>>guard_shift)*INT64_C(0x100000000)+old):0;
        full+=rounded;
        int guard_overflow=full>INT64_C(0x7fffffffff) || full< -INT64_C(0x8000000000);
        uint32_t result=(uint32_t)full;
        if((s->c[0]&16) && overflow) result=exact<0?0x80000000u:0x7fffffffu;
        if((s->c[0]&33)==33) result &= 0xffff0000u;
        put_pair(s,dest,result);
        if(accumulate) s->c[7]=(s->c[7]&~(255u<<guard_shift))|(((uint64_t)full>>32&255)<<guard_shift);
        uint64_t unsigned_sum=(uint64_t)old+(uint32_t)rounded;
        s->flags=(s->flags&~(F_V|F_C|F_GE))|(overflow?(F_V|F_SV):0)|
            (accumulate && unsigned_sum>UINT32_MAX?F_C:0)|((int32_t)result>=0?F_GE:0);
        if(accumulate) s->flags=(s->flags&~512u)|(guard_overflow?(512|128):0);
    } else if(top==6 || top==7) {
        uint16_t addr=b, updated=b; int update=0, count=1;
        if(op<8) addr=b+sx(op,3);
        else if(op==8 || op==11) { count=2; update=1; updated=b+(op==8?2:-2); if(op==11) --addr; if(x==15) goto unsupported; }
        else if(op==12 || op==13) { if(y==15) goto unsupported; addr=b+s->r[y+1]; updated=addr; update=op==13; }
        else { int inc=op==9?1:op==10?2:op==14?-2:-1; update=1; updated=b+inc; }
        unsigned cb_begin=0,cb_end=0;
        if((op==8 || op==9 || op==10) && ((y==14&&(s->c[15]&8)) || (y==15&&(s->c[15]&4)))) {
            unsigned c=19+(y-14);
            cb_begin=s->c[c]; cb_end=s->c[c+2];
            if(cb_end<cb_begin+2 || b<cb_begin || b>=cb_end) cb_end=0;
            if(cb_end && (unsigned)b+(op==9?1:2)>=cb_end)
                updated=cb_begin+(unsigned)b+(op==9?1:2)-cb_end;
        }
        if(update && y<=12 && (s->c[14]&(top==7?1:2))) goto unsupported;
        int program=!!(s->c[15] & (top==7?32:16));
        uint16_t values[2]={s->r[x],s->r[(x+1)&15]};
        for(int i=0;i<count;++i) {
            uint16_t at=cb_end && (unsigned)addr+i>=cb_end?cb_begin+(unsigned)addr+i-cb_end:(unsigned)addr+i;
            uint8_t *m=sram(s,at,program);
            if(m) { if(top==7) values[i]=(uint16_t)(m[0]|m[1]<<8); else { m[0]=(uint8_t)values[i]; m[1]=(uint8_t)(values[i]>>8); } }
            else if(top==7) values[i]=s->read(s->ctx,at,program);
            else s->write(s->ctx,at,values[i],program);
        }
        if(top==7) { s->r[x]=values[0]; if(count==2) s->r[x+1]=values[1]; }
        if(update) s->r[y]=updated;
    } else if(top==8) {
        uint32_t v;
        switch(op) {
        case 0: s->r[x]=arithmetic(s,a,b,0,0); break;
        case 1: arithmetic(s,a,b,1,1); break;
        case 2: case 3: case 4: case 5:
            s->r[x]=shift16(s,a,b,op); break;
        case 6: s->r[x]=arithmetic(s,a,b,1,0); break;
        case 7: v=normalize(b,16); s->r[x]=v; flags(s,v,v,0); s->flags &= ~F_V; break;
        case 15: s->r[x]=arithmetic(s,0,b,1,0); break;
        case 12:
            v=b==0x8000?0x7fff:(int16_t)b<0?-(int16_t)b:b;
            s->r[x]=v; flags(s,v,v,0); s->flags &= ~F_V; break;
        case 13: case 14: {
            int diff=(int16_t)a-(int16_t)b;
            int select=op==13?diff<=0:diff>=0;
            s->r[x]=select?a:b;
            flags(s,diff,(uint32_t)diff,select); s->flags &= ~F_V; break;
        }
        case 8: case 9: case 10: case 11:
            v=op==8?a&b:op==9?a|b:op==10?a^b:(uint16_t)~b;
            s->r[x]=v;
            flags(s,(int16_t)v,v,0); s->flags &= ~F_V; break;
        default: goto unsupported;
        }
    } else if(top==9 && !(x&1)) {
        if((y&1) && !(op>=2 && op<=5)) goto unsupported;
        uint32_t aa=pair(s,x),bb=(op>=2 && op<=5)?b:pair(s,y),v;
        switch(op) {
        case 0: put_pair(s,x,arithmetic32(s,aa,bb,0,0,0)); break;
        case 1: arithmetic32(s,aa,bb,1,1,1); break;
        case 6: put_pair(s,x,arithmetic32(s,aa,bb,1,0,1)); break;
        case 7: v=normalize(bb,32); s->r[x]=v; flags(s,v,v,0); s->flags &= ~F_V; break;
        case 15: put_pair(s,x,arithmetic32(s,0,bb,1,0,1)); break;
        case 12:
            v=bb==0x80000000u?0x7fffffffu:(int32_t)bb<0?0u-bb:bb;
            put_pair(s,x,v); flags(s,(int32_t)v,v,0); s->flags &= ~F_V; break;
        case 13: case 14: {
            int cmp=(int32_t)aa>(int32_t)bb?1:(int32_t)aa<(int32_t)bb?-1:0;
            int select=op==13?cmp<=0:cmp>=0;
            put_pair(s,x,select?aa:bb);
            flags(s,cmp,(uint32_t)cmp,select); s->flags &= ~F_V; break;
        }
        case 2: case 3: case 4: case 5: put_pair(s,x,shift32(s,aa,b,op)); break;
        case 8: case 9: case 10: case 11:
            v=op==8?aa&bb:op==9?aa|bb:op==10?aa^bb:~bb;
            put_pair(s,x,v); flags(s,(int32_t)v,v,0); s->flags &= ~F_V; break;
        default: goto unsupported;
        }
    } else if(top==10) {
        if(op==0) s->r[x]=arithmetic(s,a,(uint16_t)sx(y,4),0,0);
        else if(op==1) arithmetic(s,a,(uint16_t)sx(y,4),1,1);
        else if(op>=2 && op<=5) {
            s->r[x]=shift16(s,a,y,op);
        }
        else if(op==6) s->r[x]=sx(y,4);
        else if(op==7 && y==0) { s->c[17]=next; next=a; }
        else if(op>=8) {
            uint16_t v=op>=12?control(s,x):a, mask=1u<<y;
            switch(op&3) {
            case 0: v &= ~mask; break;
            case 1: v |= mask; break;
            case 2: v ^= mask; break;
            case 3: s->flags=(s->flags&~F_Z)|((v&mask)?0:F_Z); break;
            }
            if((op&3)!=3) { if(op>=12) set_control(s,x,v,&next); else { s->r[x]=v; s->flags=(s->flags&~F_Z)|(v?0:F_Z); } }
        } else goto unsupported;
    } else if(top==11 && op<8 && !(x&1)) {
        if(op==0) {
            if(y&1) goto unsupported;
            put_pair(s,x,arithmetic32(s,pair(s,y),0x8000,0,0,0));
        } else if(op==1) arithmetic32(s,pair(s,x),(uint32_t)sx(y,4),1,1,1);
        else if(op<=5) put_pair(s,x,shift32(s,pair(s,x),y,op));
        else {
            if(y&1) goto unsupported;
            put_pair(s,x,arithmetic32(s,pair(s,x),pair(s,y),op==7,0,!!(s->flags&F_C)));
        }
    } else if((w&0xfe00)==0xb800) s->r[y]=control(s,(w>>4)&31);
    else if((w&0xfe00)==0xba00) set_control(s,(w>>4)&31,b,&next);
    else if((w&0xff00)==0xbc00) s->r[x]=b;
    else if(w==0xbf01) next=s->c[17];
    else if(w==0xbf02) {
        next=s->c[18]; s->c[2]=(s->c[2]&~0x8000u)|((s->c[2]&0x4000)<<1);
        s->c[3]=(s->c[3]&~0xc000u)|((s->c[3]&0x3000)<<2);
    }
    else if(top==14 && (op==2 || op==3 || op==10 || op==11) && !(w&0x11)) {
        unsigned dest=op&8?2:0;
        uint16_t hi=op&1?s->r[x+1]-s->r[y+1]:s->r[x+1]+s->r[y+1];
        s->r[dest]=op&1?a-b:a+b; s->r[dest+1]=hi;
    }
    else if(top==14 && (op==4 || op==12)) {
        int32_t product=(int32_t)(int16_t)a*(int16_t)b;
        put_pair(s,op==4?0:2,(uint32_t)product);
        s->flags=(s->flags&~(F_V|F_C|F_GE))|(product>=0?F_GE:0);
    }
    else if(top==13) {
        unsigned n=(w>>8)&7;
        uint16_t v=op&8 ? (s->c[n]&255)|(w<<8) : (s->c[n]&0xff00)|(w&255);
        set_control(s,n,v,&next);
    }
    else if(w==0xcf00) { }
    else goto unsupported;
    if(s->fault) return 0;
    s->pc=next; ++s->instructions; return 1;
unsupported:
    s->fault=1; s->fault_pc=pc; s->fault_op=w; return 0;
}
