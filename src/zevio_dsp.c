#include "zevio_dsp.h"
#include "mjp_hle.h"
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
    z->trace = getenv("VFLASH_DSPTRACE") ? atoi(getenv("VFLASH_DSPTRACE")) : 0;
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
/* ------------------------------------------------------------------------
 * High-level emulation of the MIAV movie player firmware (#7). The DSP runs
 * at ~25 M instructions a second through its JPEG decoder; here the stream
 * the ARM feeds it is parsed and decoded natively instead, and the player's
 * protocol answered as the firmware does:
 *   window base + 0x1C000 (0x1083C000): command words; +108 bit 0 = a command
 *   (done: +108 cleared, +110 bit 0, ARM IRQ 18)
 *     0x80 init: reply +40 0x8080, +48 0x7000
 *     0x81 play: ring start, end, window base, 0xA000, Y, U, V, audio ring 1,
 *          ring 2, ring size; reply 0x8081, +48 width, height, fps, flag, frames
 *     0x82 stop
 *   per frame: message at +80 (0x92, flags, audio end pointer, audio bytes,
 *   audio wrap, frame number, input chunk flag, frame size, consumed ring
 *   pointer, ...), +104 bit 0 (ARM IRQ 17); the ARM clears it after writing
 *   at +C0 0x8092, the next Y/U/V buffers and a skip request (+DC).
 * VFLASH_DSPHLE=0 runs the firmware instead. */
static int hle_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("VFLASH_DSPHLE") || atoi(getenv("VFLASH_DSPHLE"));
    return on;
}
static int hle_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("VFLASH_HLELOG") != NULL;
    return on;
}
/* The MIAV player: its decoder's entry points, as loaded by the ARM */
static int is_mjp_player(const ZevioDSP *z) {
    static const struct { uint16_t at, w; } sig[] = {
        { 0x0d00, 0x2694 }, { 0x0d32, 0x1e03 }, { 0x0d3e, 0x144f }, { 0x0d5d, 0x795f },
    };
    for (unsigned i = 0; i < sizeof sig / sizeof sig[0]; i++)
        if ((z->p[2u * sig[i].at] | z->p[2u * sig[i].at + 1] << 8) != sig[i].w) return 0;
    return 1;
}
static uint32_t hle_mbox(ZevioDSP *z) {
    return ((z->control[0x24/4] & 0x7fff) << 17) + 0x1c000;
}
static uint32_t rd32(ZevioDSP *z, uint32_t a) {
    uint8_t *p = bus(z, a, 4);
    return p ? le32(p) : 0;
}
static void wr32(ZevioDSP *z, uint32_t a, uint32_t v) {
    uint8_t *p = bus(z, a, 4);
    if (p) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
}
/* bytes from the input ring at offset off, wrapping */
static void ring_read(ZevioDSP *z, uint32_t off, uint8_t *dst, uint32_t n) {
    MjpPlay *m = &z->play;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = bus(z, m->ring_base + (off + i) % m->ring_size, 1);
        dst[i] = p ? *p : 0;
    }
}
static void audio_put(ZevioDSP *z, uint32_t base, uint32_t *wr, const uint8_t *src, uint32_t n, int *wrapped) {
    MjpPlay *m = &z->play;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = bus(z, base + *wr, 1);
        if (p) *p = src[i];
        if (++*wr >= m->a_size) { *wr = 0; *wrapped = 1; }
    }
}
static void hle_command(ZevioDSP *z) {
    MjpPlay *m = &z->play;
    uint32_t mb = hle_mbox(z), cmd = rd32(z, mb);
    if (hle_log()) fprintf(stderr, "[ZHLE] command %x\n", cmd);
    if (cmd == 0x80) {
        wr32(z, mb + 0x40, 0x8080);
        wr32(z, mb + 0x48, 0x7000);
    } else if (cmd == 0x81) {
        m->ring_base = rd32(z, mb + 8);
        m->ring_size = rd32(z, mb + 12) - m->ring_base;
        m->ybuf = rd32(z, mb + 24); m->ubuf = rd32(z, mb + 28); m->vbuf = rd32(z, mb + 32);
        m->a1_base = rd32(z, mb + 36); m->a2_base = rd32(z, mb + 40); m->a_size = rd32(z, mb + 44);
        m->a1_wr = m->a2_wr = 0;
        uint8_t h[64];
        if (!m->ring_size || m->ring_size > 0x1000000) m->ring_size = 0x96000;
        ring_read(z, 0, h, sizeof h);
        uint32_t hs = le32(h + 4);
        m->frames_total = le32(h + 16);
        m->fps = h[20] | h[21] << 8;
        unsigned flag = h[22] | h[23] << 8;
        m->w = h[24] | h[25] << 8; m->h = h[26] | h[27] << 8;
        if (memcmp(h, "MIAV", 4) || !m->fps || hs >= m->ring_size) {
            fprintf(stderr, "[ZHLE] no MIAV stream at %08x\n", m->ring_base);
            m->playing = 0;
        } else {
            m->rd = hs; m->consumed_chunk = 0; m->frame = 0; m->hdr_len = 0;
            m->playing = 1; m->waiting_ack = 0; m->skip_next = 0;
            m->next_at = m->clock;
        }
        wr32(z, mb + 0x40, 0x8081); wr32(z, mb + 0x44, 0);
        wr32(z, mb + 0x48, m->w); wr32(z, mb + 0x4c, m->h); wr32(z, mb + 0x50, m->fps);
        wr32(z, mb + 0x54, flag); wr32(z, mb + 0x58, m->frames_total);
        if (hle_log()) fprintf(stderr, "[ZHLE] play %ux%u %u fps %u frames, ring %08x+%x, audio %08x/%08x+%x\n",
                               m->w, m->h, m->fps, m->frames_total, m->ring_base, m->ring_size, m->a1_base, m->a2_base, m->a_size);
    } else if (cmd == 0x82) {
        /* stop: the ARM sends it after the last frame (Bratz's intro) */
        wr32(z, mb + 0x40, 0x8082);
        m->playing = 0;
        m->waiting_ack = 0;
    } else {
        fprintf(stderr, "[ZHLE] unknown command %x: stopping\n", cmd);
        m->playing = 0;
    }
    z->control[0x108/4] &= ~1u;
    z->control[0x110/4] |= 1;
}
/* One frame event: the stream up to and including the next video chunk */
static void hle_frame(ZevioDSP *z) {
    MjpPlay *m = &z->play;
    static uint8_t buf[0x40000], y[640 * 480], u[320 * 240], v[320 * 240];
    uint32_t audio1 = 0, size = 0, flags = 0, tag_ok = 1;
    int wrapped = 0, wr2 = 0, have_video = 0;
    for (int guard = 0; guard < 16 && !have_video; guard++) {
        uint8_t ch[12];
        ring_read(z, m->rd, ch, 12);
        uint32_t len = le32(ch + 8);
        if (len > sizeof buf || ch[2] < '0' || (memcmp(ch + 2, "dc", 2) && memcmp(ch + 2, "wb", 2) && memcmp(ch + 2, "db", 2))) {
            tag_ok = 0;
            break;
        }
        ring_read(z, (m->rd + 12) % m->ring_size, buf, len);
        m->rd = (m->rd + 12 + len) % m->ring_size;
        if (!memcmp(ch + 2, "wb", 2)) {
            if (ch[1] == '1') { audio_put(z, m->a1_base, &m->a1_wr, buf, len, &wrapped); audio1 += len; }
            else audio_put(z, m->a2_base, &m->a2_wr, buf, len, &wr2);
            continue;
        }
        have_video = 1;
        size = len;
        for (uint32_t i = 0; i + 1 < len; i += 2) { uint8_t t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t; }
        uint32_t off = 0;
        if (!m->hdr_len && len > 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
            /* frame 0: the tables, up to SOS, kept for every later frame */
            uint32_t i = 2;
            while (i + 4 <= len && buf[i] == 0xFF && buf[i + 1] != 0xDA) i += 2 + (buf[i + 2] << 8 | buf[i + 3]);
            if (i <= sizeof m->hdr) { memcpy(m->hdr, buf, i); m->hdr_len = i; off = i; }
        } else if (len > 2 && buf[0] == 0xFF && buf[1] == 0xD8) {
            uint32_t i = 2;
            while (i + 4 <= len && buf[i] == 0xFF && buf[i + 1] != 0xDA) i += 2 + (buf[i + 2] << 8 | buf[i + 3]);
            off = i;
        }
        if (m->skip_next) {
            flags |= 4;
        } else if (m->hdr_len && off < len) {
            int w, h;
            int r = mjp_decode(m->hdr, m->hdr_len, buf + off, len - off, y, u, v, &w, &h, 640, 480);
            if (!r) {
                uint8_t *py = bus(z, m->ybuf, (uint32_t)(w * h)), *pu = bus(z, m->ubuf, (uint32_t)(w * h / 4)),
                        *pv = bus(z, m->vbuf, (uint32_t)(w * h / 4));
                if (py) memcpy(py, y, (size_t)w * h);
                if (pu) memcpy(pu, u, (size_t)w * h / 4);
                if (pv) memcpy(pv, v, (size_t)w * h / 4);
                /* VFLASH_HLEDUMP=<frame>,<file>: that frame's planes, for tests */
                const char *dump = getenv("VFLASH_HLEDUMP");
                if (dump && (uint32_t)atoi(dump) == m->frame + 1 && strchr(dump, ',')) {
                    FILE *f = fopen(strchr(dump, ',') + 1, "wb");
                    if (f) {
                        fwrite(y, 1, (size_t)w * h, f); fwrite(u, 1, (size_t)w * h / 4, f);
                        fwrite(v, 1, (size_t)w * h / 4, f); fclose(f);
                    }
                }
            } else if (hle_log()) {
                fprintf(stderr, "[ZHLE] frame %u: decode error %d\n", m->frame + 1, r);
            }
        }
    }
    if (!tag_ok && hle_log()) fprintf(stderr, "[ZHLE] frame %u: no chunk at ring +%x\n", m->frame + 1, m->rd);
    m->skip_next = 0;
    m->frame++;
    uint32_t chunk = m->rd / 0x6000, mb = hle_mbox(z) + 0x80;
    wr32(z, mb + 0x00, 0x92);
    wr32(z, mb + 0x08, 0);
    wr32(z, mb + 0x0c, flags);
    wr32(z, mb + 0x10, m->a1_base + m->a1_wr);
    wr32(z, mb + 0x14, audio1);
    wr32(z, mb + 0x18, wrapped);
    wr32(z, mb + 0x1c, m->frame);
    wr32(z, mb + 0x20, chunk != m->consumed_chunk ? 0xc : 0);
    wr32(z, mb + 0x24, size);
    wr32(z, mb + 0x28, 0);
    wr32(z, mb + 0x2c, m->ring_base + chunk * 0x6000);
    wr32(z, mb + 0x30, 0x14d);
    wr32(z, mb + 0x34, 0x80);
    wr32(z, mb + 0x38, 0);
    m->consumed_chunk = chunk;
    if (!have_video || m->frame >= m->frames_total) {
        if (hle_log()) fprintf(stderr, "[ZHLE] end after %u frames\n", m->frame);
        m->playing = 0;
    }
    z->control[0x104/4] |= 1;
    m->waiting_ack = 1;
}
static void hle_ack(ZevioDSP *z) {
    MjpPlay *m = &z->play;
    uint32_t mb = hle_mbox(z) + 0xc0;
    m->waiting_ack = 0;
    if (rd32(z, mb) == 0x8092) {
        m->ybuf = rd32(z, mb + 12); m->ubuf = rd32(z, mb + 16); m->vbuf = rd32(z, mb + 20);
        m->skip_next = rd32(z, mb + 28) != 0;
    }
}
static void hle_run(ZevioDSP *z, uint64_t budget) {
    MjpPlay *m = &z->play;
    m->clock += budget;
    if (m->cmd_pending) { m->cmd_pending = 0; hle_command(z); }
    if (!m->playing || m->waiting_ack || m->clock < m->next_at) return;
    hle_frame(z);
    uint64_t period = (z->cpu_hz ? z->cpu_hz : 100000000u) / (m->fps ? m->fps : 30);
    m->next_at += period;
    if (m->next_at + period < m->clock) m->next_at = m->clock;   /* fell behind: no burst */
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
    if(z->play.active) {
        if(o==0x108 && (v&1)) z->play.cmd_pending=1;
        if(o==0x104 && (old&1) && !(v&1)) hle_ack(z);
    } else {
    if(o==0x108 && (v&1)) zsp400_request_irq(&z->core,4);
    if(o==0x104 && (old&1) && !(v&1)) {
        z->control[0x10c/4]|=1;
        zsp400_request_irq(&z->core,0);
    }
    }
    if (z->trace) fprintf(stderr,"[ZDSP] control +%03x=%08x\n",o,v);
    if(o==0x20) {
        if((v&1) && !z->running) {
            zsp400_reset(&z->core); z->fault_reported=0;
            /* the MIAV movie player: emulated at a high level, the core stays
             * stopped; it reports ready (FC8A = 3) as the firmware's boot does */
            memset(&z->play,0,sizeof z->play);
            if(hle_on() && is_mjp_player(z)) {
                z->play.active=1;
                z->control[0x114/4]=3;
                if(hle_log()) fprintf(stderr,"[ZHLE] movie player firmware: native decoding\n");
            }
        }
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
/* VFLASH_DSPWIN=n (debugging): the first n DSP accesses through the window
 * into the ARM's memory (core_memory's bus() case), and every one to the
 * mailbox registers */
static long win_left = -1;
static void win_log(ZevioDSP *z, uint16_t a, int program, int w, uint16_t v) {
    if (win_left < 0) win_left = getenv("VFLASH_DSPWIN") ? atol(getenv("VFLASH_DSPWIN")) : 0;
    if (!win_left) return;
    win_left--;
    uint32_t packed=z->control[0x24/4];
    uint32_t base=program ? (packed<<1)&0xfffe0000u : (packed&0x7fff)<<17;
    fprintf(stderr,"[ZWIN] PC=%04x %s %c %04x (ARM %08x) %04x\n",z->core.pc,program?"P":"D",w?'W':'R',a,base+2u*a,v);
}
static int in_sram(ZevioDSP *z, uint16_t a, int program) {
    uint16_t mode=z->core.c[15];
    if(program) return a<0x4000 && (!(mode&2)||(mode&0x400));
    return a<0xa000 && !(mode&1);
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
        if(z->trace>1) fprintf(stderr,"[ZDSP] DSP PC=%04x reads peripheral[%04x]=%04x\n",z->core.pc,a,(uint16_t)(z->control[o/4] >> ((o&2)*8)));
        return z->control[o/4] >> ((o&2)*8);
    }
    uint8_t *p=core_memory(z,a,program);
    if(p && !in_sram(z,a,program)) win_log(z,a,program,0,(uint16_t)(p[0]|p[1]<<8));
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
    if(p && !in_sram(z,a,program)) win_log(z,a,program,1,v);
    if(p) { p[0]=v; p[1]=v>>8; return; }
    z->core.fault=3; z->core.fault_pc=z->core.pc; z->core.fault_op=a;
}
/* VFLASH_DSPPROF=n (debugging): every n calls, the DSP's most executed
 * program addresses since the last report, and how much it slept */
static uint32_t prof_pc[0x10000];
static uint64_t prof_sleep, prof_steps;
static void dsp_prof_report(void) {
    uint32_t top[16] = {0};
    for(unsigned k=0;k<16;k++) {
        uint32_t best=0; unsigned at=0;
        for(unsigned i=0;i<0x10000;i++) if(prof_pc[i]>best) { int used=0; for(unsigned j=0;j<k;j++) if(top[j]==i) used=1; if(!used) { best=prof_pc[i]; at=i; } }
        top[k]=at;
        if(best) fprintf(stderr,"[DSPPROF] %04x %u\n",at,best);
    }
    fprintf(stderr,"[DSPPROF] steps %llu slept %llu\n",(unsigned long long)prof_steps,(unsigned long long)prof_sleep);
    /* VFLASH_DSPPROF_FILE=<path>: the whole histogram appended there, one
     * "pc count" line per address that ran, a blank line per report */
    if(getenv("VFLASH_DSPPROF_FILE")) {
        FILE *f=fopen(getenv("VFLASH_DSPPROF_FILE"),"a");
        if(f) {
            for(unsigned i=0;i<0x10000;i++) if(prof_pc[i]) fprintf(f,"%04x %u\n",i,prof_pc[i]);
            fputc('\n',f); fclose(f);
        }
    }
    memset(prof_pc,0,sizeof prof_pc); prof_sleep=prof_steps=0;
}
void zevio_dsp_run(ZevioDSP *z,uint64_t budget) {
    if(!z->running) return;
    if(z->play.active) { hle_run(z,budget); return; }
    static int prof=-1; static unsigned calls;
    static long calls_left=-1;
    if(calls_left<0) calls_left=getenv("VFLASH_DSPCALLS")?atol(getenv("VFLASH_DSPCALLS")):0;
    if(prof<0) prof=getenv("VFLASH_DSPPROF")?atoi(getenv("VFLASH_DSPPROF")):0;
    while(budget) {
        if(z->core.c[15]&0x6000) {   /* asleep: timers only */
            uint64_t slept=zsp400_sleep(&z->core,budget);
            if(slept) { budget-=slept; if(prof) prof_sleep+=slept; continue; }
        }
        if(calls_left) {
            /* VFLASH_DSPCALLS=n: calls, returns and interrupt entries with
             * the instruction count, to see the firmware's call graph */
            uint16_t pc=z->core.pc, w=core_read(z,pc,1);
            unsigned top=w>>12;
            int is_call=top==1 || ((w&0xff0f)==0xa700), is_ret=w==0xbf01 || w==0xbf02;
            uint16_t before=pc;
            if(!zsp400_step(&z->core)) break;
            budget--;
            if(is_call || is_ret || (z->core.pc!=(uint16_t)(before+1) && top!=0 && top!=4 && !is_call && !is_ret && z->core.pc<0x100)) {
                fprintf(stderr,"[ZCALL] %llu %04x %s %04x\n",(unsigned long long)z->core.instructions,before,
                        is_call?"call":is_ret?"ret":"irq",z->core.pc);
                calls_left--;
            }
            continue;
        }
        if(prof) {
            prof_pc[z->core.pc]++; prof_steps++;
            if(!zsp400_step(&z->core)) break;
            budget--;
            continue;
        }
        /* awake: a run of steps; it ends early at a fault or sleep */
        uint64_t done=zsp400_run(&z->core,budget);
        budget-=done;
        if(z->core.fault || (z->core.c[15]&0x8000)) break;
        if(!done) break;
    }
    if(prof && ++calls%(unsigned)prof==0) dsp_prof_report();
    if(z->core.fault && !z->fault_reported) {
        fprintf(stderr,"[ZDSP] stopped fault=%d PC=%04x opcode/address=%04x after %llu instructions\n",z->core.fault,z->core.fault_pc,z->core.fault_op,(unsigned long long)z->core.instructions);
        z->fault_reported=1;
    }
}
