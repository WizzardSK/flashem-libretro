#include "zevio_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t core_read(void *ctx, uint16_t a, int program);
static void core_write(void *ctx, uint16_t a, uint16_t v, int program);

void zevio_dsp_reset(ZevioDSP *z) {
    memset(z->dma, 0, sizeof z->dma);
    memset(z->control, 0, sizeof z->control);
    z->dma[0x84/4] = 0x10; /* transfer engine idle */
    z->dma[0xc4/4] = 0x10;
    z->transfers = z->words = 0;
    zsp400_reset(&z->core);
    z->running=z->fault_reported=0;
}
void zevio_dsp_init(ZevioDSP *z, uint8_t *ram, uint32_t base, uint32_t size) {
    memset(z, 0, sizeof *z);
    z->ram = ram; z->ram_base = base; z->ram_size = size;
    z->trace = getenv("VFLASH_DSPTRACE") != NULL;
    z->core.ctx=z; z->core.read=core_read; z->core.write=core_write;
    z->core.iram=z->p; z->core.dram=z->d;
    zevio_dsp_reset(z);
}
static uint8_t *bus(ZevioDSP *z, uint32_t a, uint32_t n) {
    uint32_t o = a - z->ram_base;
    if (o < z->ram_size && n <= z->ram_size - o) return z->ram + o;
    o = a - 0xdc000000u;
    if (o < sizeof z->d && n <= sizeof z->d - o) return z->d + o;
    o = a - 0xdc020000u;
    if (o < sizeof z->p && n <= sizeof z->p - o) return z->p + o;
    return NULL;
}
static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void transfer(ZevioDSP *z,unsigned channel) {
    unsigned base=0x80+channel*0x40, bit=1u<<channel;
    uint32_t node = z->dma[(base+8)/4];
    unsigned nodes = 0;
    z->dma[0] &= ~bit;
    z->dma[0x40/4] &= ~bit;
    z->dma[(base+4)/4] &= ~0x10u;
    while (node) {
        uint8_t *p = bus(z, node, 32);
        if (!p || (node & 3) || ++nodes > 4096) goto fault;
        uint32_t h0=le32(p), h1=le32(p+4), src=le32(p+8), dst=le32(p+12);
        uint32_t next=le32(p+16), n=le32(p+20), si=le32(p+24), di=le32(p+28);
        if (z->trace) fprintf(stderr,"[ZDSP] DMA %08x: %08x -> %08x words=%u step=%u/%u next=%08x\n",node,src,dst,n,si,di,next);
        /* Only the halfword descriptor format established by firmware.
         * Unsupported configurations fail instead of reporting completion. */
        if (h0 != 0x00400000 || h1 != 0x1f004800 || n > 0x10000 || si > 2 || di > 2) goto fault;
        for (uint32_t i=0; i<n; ++i) {
            uint8_t *s=bus(z,src,2), *d=bus(z,dst,2);
            if (!s || !d) goto fault;
            uint8_t lo=s[0], hi=s[1]; d[0]=lo; d[1]=hi;
            src += si; dst += di; ++z->words;
        }
        ++z->transfers; node=next;
    }
    z->dma[0] |= bit;
    z->dma[(base+4)/4] |= 0x10;
    return;
fault:
    fprintf(stderr,"[ZDSP] unsupported/invalid DMA descriptor %08x\n",node);
    z->dma[0x40/4] |= bit;
    z->dma[(base+4)/4] |= 0x10;
}
uint32_t zevio_dsp_dma_read(ZevioDSP *z, uint32_t o) {
    return o < sizeof z->dma ? z->dma[o/4] : 0;
}
void zevio_dsp_dma_write(ZevioDSP *z, uint32_t o, uint32_t v) {
    if (o >= sizeof z->dma) return;
    if (o == 0x10) { z->dma[0] &= ~v; return; }
    if (o == 0x50) { z->dma[0x40/4] &= ~v; return; }
    z->dma[o/4]=v;
    if ((o == 0x80 || o==0xc0) && (v & 2)) transfer(z,(o-0x80)/0x40);
}
uint32_t zevio_dsp_control_read(ZevioDSP *z, uint32_t o) {
    return o < sizeof z->control ? z->control[o/4] : 0;
}
void zevio_dsp_control_write(ZevioDSP *z, uint32_t o, uint32_t v) {
    if (o >= sizeof z->control) return;
    uint32_t old=z->control[o/4];
    if(o==0x110) { z->control[o/4]&=~v; return; }
    z->control[o/4]=v;
    /* IRQ4's firmware handler sets the command-pending flag; completing the
     * command clears FC84 (this register). IRQ0 is a separate stream event. */
    if(o==0x108 && (v&1)) zsp400_request_irq(&z->core,4);
    if(o==0x104 && (old&1) && !(v&1)) {
        z->control[0x10c/4]|=1;
        zsp400_request_irq(&z->core,0);
    }
    if (z->trace) fprintf(stderr,"[ZDSP] control +%03x=%08x\n",o,v);
    if(o==0x20) {
        if((v&1) && !z->running) { zsp400_reset(&z->core); z->fault_reported=0; }
        z->running=!!(v&1);
    }
    /* No fabricated ready response: execution must produce the handshake. */
}

static uint8_t *core_memory(ZevioDSP *z, uint16_t a, int program) {
    uint16_t mode=z->core.c[15];
    /* Physical SRAM extents inferred from the alternative loader's 32/80
     * KiB clear operations. Keep this assumption explicit until measured. */
    if(program && a<0x4000 && (!(mode&2)||(mode&0x400))) return z->p+2u*a;
    if(!program && a<0xa000 && !(mode&1)) return z->d+2u*a;
    uint32_t packed=z->control[0x24/4];
    uint32_t base=program ? (packed<<1)&0xfffe0000u : (packed&0x7fff)<<17;
    return bus(z,base+2u*a,2);
}
static uint16_t core_read(void *ctx,uint16_t a,int program) {
    ZevioDSP *z=ctx;
    if(!program && a>=0xfd00 && a<0xfd80) {
        unsigned o=2u*(a-0xfd00);
        return zevio_dsp_dma_read(z,o&~3u)>>((o&2)*8);
    }
    /* Paired firmware wrappers identify a 2:1 DSP-word/ARM-byte register
     * mirror: FC8A ready on DSP corresponds to ARM 900A0114. */
    if(!program && a>=0xfc80 && a<=0xfc8d) {
        unsigned o=0x100+(a-0xfc80)*2;
        return z->control[o/4] >> ((o&2)*8);
    }
    uint8_t *p=core_memory(z,a,program);
    if(p) return p[0]|p[1]<<8;
    z->core.fault=2; z->core.fault_pc=z->core.pc; z->core.fault_op=a;
    return 0;
}
static void core_write(void *ctx,uint16_t a,uint16_t v,int program) {
    ZevioDSP *z=ctx;
    if(!program && a>=0xfd00 && a<0xfd80) {
        unsigned o=2u*(a-0xfd00),shift=(o&2)*8;
        uint32_t combined=(z->dma[o/4]&~(0xffffu<<shift))|((uint32_t)v<<shift);
        if(o&2) z->dma[o/4]=combined;
        else zevio_dsp_dma_write(z,o,combined);
        return;
    }
    if(!program && a>=0xfc80 && a<=0xfc8d) {
        unsigned o=0x100+(a-0xfc80)*2, shift=(o&2)*8;
        uint32_t old=z->control[o/4];
        if(o==0x10c) { z->control[o/4]&=~(uint32_t)v; return; }
        z->control[o/4]=(z->control[o/4]&~(0xffffu<<shift))|((uint32_t)v<<shift);
        /* Inferred command-latch completion: DSP clears the host request
         * after writing its shared-memory reply; host IRQ18 acks +110. */
        if(o==0x108 && (old&1) && !(v&1)) z->control[0x110/4]|=1;
        if(z->trace) fprintf(stderr,"[ZDSP] DSP PC=%04x peripheral[%04x]=%04x (ARM +%03x)\n",z->core.pc,a,v,o);
        return;
    }
    uint8_t *p=core_memory(z,a,program);
    if(p) { p[0]=v; p[1]=v>>8; return; }
    z->core.fault=3; z->core.fault_pc=z->core.pc; z->core.fault_op=a;
}
void zevio_dsp_run(ZevioDSP *z,uint64_t budget) {
    if(!z->running) return;
    while(budget) {
        uint64_t slept=zsp400_sleep(&z->core,budget);   /* asleep: timers only */
        if(slept) { budget-=slept; continue; }
        if(!zsp400_step(&z->core)) break;
        budget--;
    }
    if(z->core.fault && !z->fault_reported) {
        fprintf(stderr,"[ZDSP] stopped fault=%d PC=%04x opcode/address=%04x after %llu instructions\n",z->core.fault,z->core.fault_pc,z->core.fault_op,(unsigned long long)z->core.instructions);
        z->fault_reported=1;
    }
}
