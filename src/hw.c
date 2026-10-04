/* Accurate-mode machine: see hw.h.
 *
 * The V.Flash SoC (LSI ZEVIO 1020) is the chip in the TI-Nspire Classic, so
 * the devices the boot ROM and µMORE touch follow Firebird's "classic" models
 * (github.com/nspire-emus/firebird, core/misc.c and core/interrupt.c). Every
 * access to an address nothing here models is logged once as [HW?]. */
#include "hw.h"
#include "vflash.h"
#include "cdrom.h"
#include "cdsp.h"
#include "ge.h"
#include "zevio_dsp.h"
#include "midi.h"
#include "cdda_dma.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define RAM_BASE   0x10000000u
#define RAM_SIZE   (16u * 1024 * 1024)
/* 0xB8000000 is the video engine: registers at 0x000-0x7FF, 0x1000-0x1FFF
 * and 0x2000+. 0x800-0xFFF is its palette RAM (256 x BGR555 at 0x800, bit 15
 * = transparent); the boot ROM borrows it to run code before video is up. */
#define VE_BASE    0xB8000000u
#define SRAM_BASE  0xB8000800u
#define SRAM_SIZE  0x800u
#define MBOX_BASE  0xF8000000u   /* 8 KB the kernel clears and flags at the top of */
#define MBOX_SIZE  0x2000u

/* Interrupt lines (Firebird interrupt.h). */
#define INT_VIDEO    21
#define INT_WATCHDOG 3
#define INT_POWER    15
/* Timer lines on the V.Flash: the kernel's timer ISR tests bits 5 and 6 of
 * the controller's masked status (0xDC000000), where the Nspire has 17-19. */
static const int timer_line[3] = { 5, 6, 19 };

#define TLB_SIZE 4096

struct HW {
    ARM9          *cpu;
    const uint8_t *rom;
    uint32_t       rom_size;
    uint8_t       *ram;
    struct CDROM  *cd;
    uint32_t      *fb;
    uint32_t       input;
    uint32_t       pad_prev;      /* controller buttons last frame (edge events) */
    uint64_t       frame;

    /* On-chip SRAM at 0xB8000000: the boot ROM copies code here and runs it. */
    uint8_t        sram[SRAM_SIZE];
    uint8_t        mbox[MBOX_SIZE];
    uint32_t       ve[0x800 / 4];     /* video engine 0xB8000000-0x7FF */
    uint32_t       ve_mid[0x1000 / 4];/* video engine 0xB8001000-0x1FFF */
    uint32_t       ve_hi[0x1000 / 4]; /* video engine 0xB8002000-0x2FFF */
    int            ve_log;
    uint64_t       vblanks;
    int            scr_w, scr_h;

    /* Timer pairs at 0x90010000 (fast), 0x900C0000, 0x900D0000: count on a
     * 32 kHz clock through a divider, reload on a completion value, raise an
     * interrupt when timer 0 reaches one. */
    struct { struct { uint16_t ticks, start, value, divider, control; } t[2];
             uint16_t compl[6]; uint8_t int_mask, int_status; } tp[3];

    struct { uint32_t clocks_load, wake_mask, disable, disable2, clocks, done; } pmu;
    uint32_t       boot_status;   /* 0x900A000C: 0 = cold boot */
    time_t         rtc_offset;
    time_t         rtc_boot;      /* RTC seconds at power-on (VFLASH_RTC, else host time) */
    uint32_t       rtc_regs[64];  /* 0x90090000-0xFF: survive a soft reset */

    /* Interrupt controller at 0xDC000000. */
    struct { uint32_t active, raw, sticky_status, status, mask[2];
             uint8_t prev_limit[2], limit[2]; uint32_t noninverted, sticky;
             uint8_t prio[32]; int line[2]; } ic;

    /* 16550-style UARTs at 0x90020000 and 0x90030000. */
    struct { uint8_t ier, lcr, dll, dlm, pending; char line[256]; int len;
             uint8_t rx[128]; int rx_h, rx_n;       /* bytes from the controller */
             uint8_t pk[64]; int pk_len; } uart[2];  /* packet being sent to it */

    /* GPIO banks at 0x90000000 (A) and 0x900D0000 (B): four 8-bit ports each
     * at stride 0x40; +0x10 direction, +0x14 output, +0x18 input. Bank B
     * port 0 carries the power-on source the kernel checks at boot. */
    struct { uint8_t dir[4], out[4], in[4], regs[4][16]; } gpio[2];

    uint32_t       ssp[16];       /* 0x90080000: registers only */
    uint32_t       ser[64];       /* 0xC4000000: serial shifter */
    CDSP           dsp;           /* servo DSP on the serial bus */
    ZevioDSP       zsp;           /* separate programmable SoC DSP */
    Midi           midi;
    uint64_t       audio_acc;
    void          *audio_ctx;
    void         (*audio_push)(void *, const int16_t *, uint32_t);
    unsigned       midi_trace_count;
    uint64_t       cd_next_us;    /* next sector under the pickup */
    uint64_t       cd_tick_us, cd_tmo_us;  /* +0x60 timers: next expiry */
    int            cd_xfer;       /* target found: sectors stream in */
    uint32_t       spi[16];       /* 0xA1000000: SPI master */
    uint32_t       dma[0x200 / 4];/* 0xBC000000: DMA controller */
    uint32_t       mathu[16];     /* 0xA4000000: math unit */
    int            dma_log;
    uint32_t       lcd[0x400];    /* 0xA8000000: display controller */
    int            lcd_log;
    uint32_t       cd_sectors, cd_last_lba;   /* sectors delivered (report) */
    int            lcd_busy;      /* a command list is running */
    uint64_t       lcd_done_us;   /* ... until then */
    GE             ge;            /* the graphics engine behind 0xA8000000 */
    int            spi_log;
    int            ser_log;
    uint32_t       misc_hi[12];   /* 0x900A0010-0x3C */

    uint32_t       cpu_hz;
    uint32_t       timer_hz;      /* timer pair input clock */
    uint64_t       timer_acc;     /* CPU cycles not yet turned into timer ticks, x timer_hz */
    uint64_t       irqs, acks, ic24;
    uint32_t       irq_by_line[32];  /* IRQs taken per line since the last report */

    /* Unmodelled accesses, logged once per address. */
    uint32_t       seen[8192];    /* open-addressed set of (addr|w)+1 */
    int            fault_log;

    /* VFLASH_IOHIST=<from>,<to>: MMIO accesses by (address, PC) over a frame
     * window, to see what an idle kernel is still polling. */
    int            hist_on;
    uint32_t       trace_pc[16];  /* VFLASH_TRACEPC=a[:b...][,n] */
    int            ntrace;
    uint32_t       puts_pc;       /* VFLASH_PUTS=<addr>[,reg]: print the C string in R0 (or Rreg) there */
    int            puts_reg;
    uint64_t       trace_from;    /* VFLASH_TRACEFROM=<frame>: tracing starts there */
    uint32_t       watch_lo, watch_len;   /* VFLASH_WATCH=<addr>,<len> */
    int            prof_on;
    struct { uint32_t pc, n; } prof[4096];  /* VFLASH_PROFILE samples */
    uint32_t       bp[16];
    int            nbp, bp_hit;
    int            trace_left;
    uint64_t       hist_from, hist_to;
    uint32_t       hist_page;     /* optional third field: only this pa >> 16 */
    struct { uint32_t addr, pc, n; } hist[4096];

    /* Software TLB: 4 KB virtual pages -> physical page, plus host pointers
     * for pages wholly in RAM/ROM/mailbox (NULL: take the slow path). */
    struct { uint32_t tag, pa; uint8_t *rd, *wr; } tlb[TLB_SIZE];
    uint32_t       tlb_gen;       /* CP15 tlb_gen the entries belong to */
    uint8_t        pt_page[RAM_SIZE >> 12];  /* RAM pages a table walk has read */
    uint32_t       ge_started;    /* command lists run since power-on */
    uint32_t       idle_slices;   /* 256-cycle slices of idle loop passed over (report) */
};

void hw_set_input(HW *hw, uint32_t b) { hw->input = b; }
void hw_set_audio_sink(HW *hw, void *ctx, void (*push)(void *, const int16_t *, uint32_t)) {
    hw->audio_ctx=ctx; hw->audio_push=push;
}
static int midi_sample(void *ctx, uint32_t a, int16_t *sample) {
    HW *hw=ctx; uint32_t o=a-RAM_BASE;
    if(o>=RAM_SIZE-1) return 0;
    *sample=(int16_t)(hw->ram[o]|hw->ram[o+1]<<8); return 1;
}
static void midi_trace(void *ctx,unsigned voice,const uint32_t *r,uint32_t pc,int on) {
    HW *hw=ctx;
    if(!r[0] && !r[1]) return;
    if(hw->midi_trace_count++>=256) return;
    fprintf(stderr,"[MIDI] %s voice=%u PC=%08x active=%d regs",on?"on":"off",voice,pc,(int)((hw->midi.active>>voice)&1));
    for(int i=0;i<16;++i) fprintf(stderr," %08x",r[i]);
    fprintf(stderr," samples");
    for(int i=0;i<8;++i) { int16_t s; if(!midi_sample(hw,r[0]+2*i,&s)) break; fprintf(stderr," %04x",(uint16_t)s); }
    fputc('\n',stderr);
}

/* ---------------------------------------------------------------- logging */

static void unmodelled(HW *hw, uint32_t pa, int w, int size, uint32_t v) {
    uint32_t key = ((pa & ~3u) | (w ? 1 : 0)) + 1, n = 8192;
    uint32_t h = (key * 2654435761u) >> 19;
    uint32_t k;
    for (k = 0; k < n / 2; k++, h = (h + 1) & (n - 1)) {
        if (hw->seen[h] == key) return;
        if (hw->seen[h] == 0) { hw->seen[h] = key; break; }
    }
    if (k == n / 2) return;   /* table full: stop logging rather than flood */
    if (w)
        printf("[HW?] write%d %08X = %08X  PC=%08X\n", size, pa, v, hw->cpu->r[15]);
    else
        printf("[HW?] read%d  %08X        PC=%08X\n", size, pa, hw->cpu->r[15]);
}

/* ---------------------------------------------------------------- interrupts */

static int ic_current(HW *hw, int fiq) {
    uint32_t m = hw->ic.status & hw->ic.mask[fiq];
    int limit = hw->ic.limit[fiq], cur = -1;
    for (int i = 0; i < 32; i++)
        if ((m >> i & 1) && hw->ic.prio[i] < limit) { cur = i; limit = hw->ic.prio[i]; }
    return cur;
}

/* The IRQ/FIQ outputs only change when the controller's state does, so they
 * are worked out then rather than on every instruction. */
static void ic_recalc(HW *hw) {
    hw->ic.line[0] = ic_current(hw, 0) >= 0;
    hw->ic.line[1] = ic_current(hw, 1) >= 0;
}

static void ic_update(HW *hw) {
    uint32_t prev = hw->ic.raw;
    hw->ic.raw = hw->ic.active ^ ~hw->ic.noninverted;
    hw->ic.sticky_status |= hw->ic.raw & ~prev;
    hw->ic.status = (hw->ic.raw & ~hw->ic.sticky) |
                    (hw->ic.sticky_status & hw->ic.sticky);
    ic_recalc(hw);
}

static void int_set(HW *hw, int n, int on) {
    if (on) hw->ic.active |= 1u << n; else hw->ic.active &= ~(1u << n);
    ic_update(hw);
}

static uint32_t ic_read(HW *hw, uint32_t pa) {
    int group = pa >> 8 & 3;
    if (group < 2) {
        int cur;
        switch (pa & 0xFF) {
        case 0x00: return hw->ic.status & hw->ic.mask[group];
        case 0x04: return hw->ic.status;
        case 0x08: case 0x0C: return hw->ic.mask[group];
        case 0x20: cur = ic_current(hw, group); return cur < 0 ? 0 : cur;
        case 0x24:
            hw->ic24++;
            cur = ic_current(hw, group); if (cur < 0) cur = 0;
            hw->ic.prev_limit[group] = hw->ic.limit[group];
            hw->ic.limit[group] = hw->ic.prio[cur];
            ic_recalc(hw);
            return cur;
        case 0x28: return hw->ic.prev_limit[group];
        case 0x2C: return hw->ic.limit[group];
        }
    } else if (group == 2) {
        switch (pa & 0xFF) {
        case 0x00: return hw->ic.noninverted;
        case 0x04: return hw->ic.sticky;
        case 0x08: return 0;
        }
    } else if (!(pa & 0x80)) {
        return hw->ic.prio[pa >> 2 & 0x1F];
    }
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}

static void ic_write(HW *hw, uint32_t pa, uint32_t v) {
    int group = pa >> 8 & 3;
    if (group < 2) {
        switch (pa & 0xFF) {
        case 0x04: hw->ic.sticky_status &= ~v; ic_update(hw); return;
        case 0x08: hw->ic.mask[group] |= v; ic_recalc(hw); return;
        case 0x0C: hw->ic.mask[group] &= ~v; ic_recalc(hw); return;
        case 0x2C: hw->ic.limit[group] = v & 0x0F; ic_recalc(hw); return;
        }
    } else if (group == 2) {
        switch (pa & 0xFF) {
        case 0x00: hw->ic.noninverted = v; ic_update(hw); return;
        case 0x04: hw->ic.sticky = v; ic_update(hw); return;
        case 0x08: return;
        }
    } else if (!(pa & 0x80)) {
        hw->ic.prio[pa >> 2 & 0x1F] = v & 7;
        ic_recalc(hw);
        return;
    }
    unmodelled(hw, pa, 1, 32, v);
}

/* ---------------------------------------------------------------- timers */

static void timer_int_check(HW *hw, int i) {
    int_set(hw, timer_line[i], (hw->tp[i].int_status & hw->tp[i].int_mask) != 0);
}

static void timer_advance(HW *hw, int i, int ticks) {
    for (int k = 0; k < 2; k++) {
        typeof(hw->tp[i].t[0]) *t = &hw->tp[i].t[k];
        if (t->control & 0x10) continue;
        int nt;
        for (nt = t->ticks + ticks; nt > t->divider; nt -= t->divider + 1) {
            int c = t->control & 7;
            t->ticks = 0;
            if (c == 0 && t->value == 0)
                ;
            else if (c != 0 && c != 7 && t->value == hw->tp[i].compl[c - 1])
                t->value = t->start;
            else
                t->value += (t->control & 8) ? 1 : -1;
            /* Completion interrupt c belongs to the second timer when that
             * timer reloads from compl[c], else to the first. (Game kernels
             * run a 1 ms tick on timer 0 via compl[0] and count wraps of the
             * free-running timer 1, reloading at compl[1], on bit 1 - with
             * both compl values 0, so the owner matters.) */
            int own1 = (hw->tp[i].t[1].control & 7) - 1;
            for (c = 0; c < 6; c++)
                if ((k == 1) == (c == own1) && t->value == hw->tp[i].compl[c]) {
                    hw->tp[i].int_status |= 1 << c;
                    timer_int_check(hw, i);
                }
        }
        t->ticks = nt;
    }
}

static int timer_index(uint32_t pa) {
    switch (pa >> 16) {
    case 0x9001: return 0;
    case 0x900C: return 1;
    default:     return 2;   /* no third pair: 0x900D0000 is GPIO on the V.Flash */
    }
}

static uint32_t timer_read(HW *hw, uint32_t pa) {
    int i = timer_index(pa);
    switch (pa & 0x3F) {
    case 0x00: return hw->tp[i].t[0].value;
    case 0x04: return hw->tp[i].t[0].divider;
    case 0x08: return hw->tp[i].t[0].control;
    case 0x0C: return hw->tp[i].t[1].value;
    case 0x10: return hw->tp[i].t[1].divider;
    case 0x14: return hw->tp[i].t[1].control;
    case 0x18: case 0x1C: case 0x20: case 0x24: case 0x28: case 0x2C:
        return hw->tp[i].compl[((pa & 0x3F) - 0x18) >> 2];
    }
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}

static void timer_write(HW *hw, uint32_t pa, uint32_t v) {
    int i = timer_index(pa);
    switch (pa & 0x3F) {
    case 0x00: hw->tp[i].t[0].start = hw->tp[i].t[0].value = v; return;
    case 0x04: hw->tp[i].t[0].divider = v; return;
    case 0x08: hw->tp[i].t[0].control = v & 0x1F; return;
    case 0x0C: hw->tp[i].t[1].start = hw->tp[i].t[1].value = v; return;
    case 0x10: hw->tp[i].t[1].divider = v; return;
    case 0x14: hw->tp[i].t[1].control = v & 0x1F; return;
    case 0x18: case 0x1C: case 0x20: case 0x24: case 0x28: case 0x2C:
        hw->tp[i].compl[((pa & 0x3F) - 0x18) >> 2] = v; return;
    case 0x30: return;
    }
    unmodelled(hw, pa, 1, 32, v);
}

/* ---------------------------------------------------------------- keys */

/* 0x900A0018 is the key state the kernel's input driver (0x1009711C) samples,
 * active high; its menus react to bits 0, 1, 8, 16 and 24; bit 25 is power. Which physical
 * button owns which bit is not known yet, so the mapping below is provisional
 * and VFLASH_KEYS=<hex>@<from>-<to> forces raw bits over a frame range. */
static uint32_t hw_keys(HW *hw) {
    uint32_t k = 0, in = hw->input;
    if (in & VFLASH_BTN_UP)     k |= 1u << 0;
    if (in & VFLASH_BTN_DOWN)   k |= 1u << 1;
    if (in & VFLASH_BTN_ENTER)  k |= 1u << 8;
    if (in & VFLASH_BTN_RED)    k |= 1u << 16;
    if (in & VFLASH_BTN_YELLOW) k |= 1u << 24;
    /* bit 25 is the power button: pressing it switches the machine off */
    const char *f = getenv("VFLASH_KEYS");
    if (f) {
        unsigned long bits = 0, a = 0, b = 0;
        if (sscanf(f, "%lx@%lu-%lu", &bits, &a, &b) == 3 && hw->frame >= a && hw->frame < b)
            k |= (uint32_t)bits;
    }
    return k;
}

/* ---------------------------------------------------------------- reset */

/* Device state a reset puts back. SDRAM, SRAM, the RTC scratch registers and
 * the PMU clock setting survive. */
static void devices_reset(HW *hw) {
    memset(hw->tp, 0, sizeof hw->tp);
    for (int i = 0; i < 3; i++)
        hw->tp[i].t[0].control = hw->tp[i].t[1].control = 0x10;
    memset(&hw->ic, 0, sizeof hw->ic);
    hw->ic.noninverted = 0xFFFFFFFFu;
    hw->ic.limit[0] = hw->ic.limit[1] = 8;
    ic_update(hw);
    memset(hw->uart, 0, sizeof hw->uart);
    memset(hw->gpio, 0, sizeof hw->gpio);
    /* B0 holds the wake sources the kernel checks at power-on (0x100113FC):
    * bit 0 the power key, which boots to the system menu, bit 2 the disc
    * wake, which mounts the disc and starts its BOOT.BIN (task 9,
    * 0x100115F0). Both are momentary: the ROM only jumps into a loaded
    * BOOT.BIN once bit 2 has dropped (0x100126B8). With a disc in, the
    * emulator wakes by the disc for the first 120 frames. */
    hw->gpio[1].in[0] = 0x01;   /* see gpio_read for the disc wake */
    /* VFLASH_GPIOA / VFLASH_GPIOB=<hex>: raw input pins, port 0 in the low byte. */
    for (int b = 0; b < 2; b++) {
        const char *e = getenv(b ? "VFLASH_GPIOB" : "VFLASH_GPIOA");
        if (!e) continue;
        uint32_t v = (uint32_t)strtoul(e, NULL, 16);
        for (int p = 0; p < 4; p++) hw->gpio[b].in[p] = (uint8_t)(v >> (8 * p));
    }
    cdsp_reset(&hw->dsp, hw->cd);
    zevio_dsp_reset(&hw->zsp);
    midi_reset(&hw->midi);
    midi_set_memory(&hw->midi,hw,midi_sample);
    if(getenv("VFLASH_MIDITRACE")) midi_set_trace(&hw->midi,hw,midi_trace);
    hw->audio_acc=0;
}

static time_t rtc_now(HW *hw);

static void hw_soft_reset(HW *hw) {
    devices_reset(hw);
    hw->boot_status = 2;
    hw->rtc_boot = rtc_now(hw);   /* the cycle count restarts; the RTC runs on */
    arm9_reset(hw->cpu);
}

/* ---------------------------------------------------------------- misc, PMU, RTC */

/* 0x900A0010-0x3C. The µMORE tick ISR reads the tick pair's (0x900C0000)
 * interrupt mask at +0x38 and status at +0x30, and acknowledges by writing
 * the handled bits back to +0x30 - not Firebird's Nspire layout (+0x18/+0x1C).
 * The rest are held as plain registers and logged until something says what
 * they are. */
static uint32_t *misc_reg(HW *hw, uint32_t pa) {
    return &hw->misc_hi[((pa & 0xFFF) - 0x10) >> 2];
}

static uint32_t misc_read(HW *hw, uint32_t pa) {
    uint32_t r = pa & 0xFFF;
    if ((r >= 0x20 && r <= 0x28) || (r >= 0x100 && r <= 0x118))
        return zevio_dsp_control_read(&hw->zsp, r);
    switch (r) {
    case 0x00: return 0x01000010;
    case 0x04: return 0;
    case 0x0C: return hw->boot_status;
    case 0x2C: return hw->tp[0].int_status;
    case 0x34: return hw->tp[0].int_mask;
    case 0x18: return hw_keys(hw);
    case 0x30: return hw->tp[1].int_status;
    case 0x38: return hw->tp[1].int_mask;
    }
    if (r >= 0x10 && r < 0x40) {
        static int n;
        uint32_t v = *misc_reg(hw, pa);
        if (n++ < 40) printf("[HW] misc read  %08X -> %08X PC=%08X\n", pa, v, hw->cpu->r[15]);
        return v;
    }
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}

static void misc_write(HW *hw, uint32_t pa, uint32_t v) {
    uint32_t r = pa & 0xFFF;
    if ((r >= 0x20 && r <= 0x28) || (r >= 0x100 && r <= 0x118)) {
        zevio_dsp_control_write(&hw->zsp, r, v);
        int_set(hw,17,(hw->zsp.control[0x118/4]&1) && (hw->zsp.control[0x104/4]&1));
        int_set(hw,18,(hw->zsp.control[0x118/4]&2) && (hw->zsp.control[0x110/4]&1));
        return;
    }
    switch (r) {
    case 0x04: return;
    case 0x08:
        /* Software reset. The ROM's first check is bit 1 of 0x900A000C, which
         * sends it down the warm-boot path instead of recalibrating SDRAM. */
        printf("[HW] soft reset via 0x900A0008 (RTC[0x88]=%08X)\n", hw->rtc_regs[0x88 >> 2]);
        hw_soft_reset(hw);
        return;
    case 0x0C: hw->boot_status = v; return;
    /* +0x2C is the fast pair's status (the ISR acknowledges it by writing
     * back what it read); +0x34 as its mask is inferred from the kernel
     * clearing it beside +0x38 at init. */
    case 0x2C: hw->tp[0].int_status &= ~v; timer_int_check(hw, 0); return;
    case 0x34: hw->tp[0].int_mask = v & 0x3F; timer_int_check(hw, 0); return;
    case 0x30: hw->tp[1].int_status &= ~v; timer_int_check(hw, 1); hw->acks++; return;
    case 0x38: hw->tp[1].int_mask = v & 0x3F; timer_int_check(hw, 1); return;
    case 0xF04: return;   /* boot progress code */
    }
    if (r >= 0x10 && r < 0x40) {
        static int n;
        *misc_reg(hw, pa) = v;
        if (n++ < 40) printf("[HW] misc write %08X = %08X PC=%08X\n", pa, v, hw->cpu->r[15]);
        return;
    }
    unmodelled(hw, pa, 1, 32, v);
}

/* The V.Flash clock tree, as the kernel computes it from 0x900B0000 (its
 * clock query at 0x1009770C): base = 300 MHz - 6 MHz * bits[16:12], CPU =
 * base / {2,4,8,16,32,32,32,32}[bits 2:0], APB = CPU / (bits[10:8] + 1), and
 * the timer pairs count at APB / 2. The kernel derives its 1 kHz tick from
 * that last one, so it has to be what the timers really run at. */
static void pmu_set_clocks(HW *hw) {
    static const uint32_t tbl[8] = { 2, 4, 8, 16, 32, 32, 32, 32 };
    uint32_t c = hw->pmu.clocks;
    uint32_t base = 300000000u - 6000000u * (c >> 12 & 0x1F);
    hw->cpu_hz = base / tbl[c & 7];
    hw->timer_hz = hw->cpu_hz / ((c >> 8 & 7) + 1) / 2;
}

static uint32_t pmu_read(HW *hw, uint32_t pa) {
    switch (pa & 0x3F) {
    case 0x00: return hw->pmu.clocks_load;
    case 0x04: return hw->pmu.wake_mask;
    case 0x08: return 0x2000;
    case 0x0C: return 0;
    case 0x14: return hw->pmu.done;
    case 0x18: return hw->pmu.disable;
    case 0x20: return hw->pmu.disable2;
    case 0x24: return hw->pmu.clocks;
    case 0x28: return 0x114;
    }
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}

static void pmu_write(HW *hw, uint32_t pa, uint32_t v) {
    switch (pa & 0x3F) {
    case 0x00: hw->pmu.clocks_load = v; return;
    case 0x04:
        /* The V.Flash ROM applies a clock change by writing 1 here and then
         * polls +0x14 bit 0 for completion (the Nspire uses +0x0C bit 2). */
        hw->pmu.wake_mask = v & 0x1FFFFFF;
        if (v & 1) {
            hw->pmu.clocks = hw->pmu.clocks_load;
            pmu_set_clocks(hw);
            hw->pmu.done |= 1;
            printf("[HW] PMU clocks %08X -> CPU %u Hz\n", hw->pmu.clocks, hw->cpu_hz);
        }
        return;
    case 0x08: return;
    case 0x0C:
        if (v & 4) {
            hw->pmu.clocks = hw->pmu.clocks_load;
            pmu_set_clocks(hw);
            printf("[HW] PMU clocks %08X -> CPU %u Hz\n", hw->pmu.clocks, hw->cpu_hz);
            int_set(hw, INT_POWER, 1);
        }
        return;
    case 0x10: return;
    case 0x14: hw->pmu.done &= ~v; int_set(hw, INT_POWER, 0); return;
    case 0x18: hw->pmu.disable = v; return;
    case 0x20: hw->pmu.disable2 = v; return;
    }
    unmodelled(hw, pa, 1, 32, v);
}

/* The RTC counts emulated seconds from the power-on time, not host time: games
 * time things by it (SpongeBob's loading screen), so a host-clock RTC made runs
 * depend on how fast the host emulates (a faster rasteriser hung SpongeBob at
 * frame ~6760 in the 2026-09-27 speed test). */
static uint64_t hw_us(HW *hw);
static time_t rtc_now(HW *hw) { return hw->rtc_boot + (time_t)(hw_us(hw) / 1000000); }

static uint32_t rtc_read(HW *hw, uint32_t pa) {
    switch (pa & 0xFFFF) {
    case 0x00: return (uint32_t)(rtc_now(hw) - hw->rtc_offset);
    case 0x14: return 0;
    }
    /* The upper registers are scratch the ROM and kernel leave for the next
     * boot (SDRAM calibration at 0xF0-0xFC, the 0xC01DB007 cold-boot magic
     * at 0x88); they keep their contents across a soft reset. */
    if ((pa & 0xFFFF) >= 0x80 && (pa & 0xFFFF) < 0x100)
        return hw->rtc_regs[(pa & 0xFF) >> 2];
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}

static void rtc_write(HW *hw, uint32_t pa, uint32_t v) {
    switch (pa & 0xFFFF) {
    case 0x08: hw->rtc_offset = rtc_now(hw) - v; return;
    case 0x04: case 0x0C: case 0x10: case 0x1C: return;
    }
    if ((pa & 0xFFFF) >= 0x80 && (pa & 0xFFFF) < 0x100) {
        hw->rtc_regs[(pa & 0xFF) >> 2] = v;
        return;
    }
    unmodelled(hw, pa, 1, 32, v);
}

/* ---------------------------------------------------------------- UARTs */

#define INT_SERIAL 1   /* UART0; UART1 is line 2 */

/* The controllers sit on the UARTs (one per port) and talk in packets:
 *   FF, type (01), n, then n payload bytes and a 16-bit checksum packed two
 *   bytes to three: g0 = 0x40 | b0[3:0], g1 = 0x40 | b0[7:4] | b1[1:0] << 4,
 *   g2 = 0x40 | b1[7:2]. The checksum is the sum of every byte after the FF
 *   (type, n, payload), low byte first. (Parser: 0x10A6D72C in Dingo
 *   Rallye's kernel.) The console keeps sending n = 2 [40 03]; a controller
 *   answers with n = 4 reports - 12-bit stick X and Y and a button bit:
 *   [Y & FF, Y >> 8 | (X & F) << 4, X >> 4, buttons]. */
static void uart_rx_push(HW *hw, int u, uint8_t b) {
    typeof(hw->uart[0]) *s = &hw->uart[u];
    if (s->rx_n < (int)sizeof s->rx) s->rx[(s->rx_h + s->rx_n++) % sizeof s->rx] = b;
}

static void pad_send(HW *hw, int u, const uint8_t *p, int n) {
    uint8_t pk[64]; int k = 0;
    unsigned sum = 1 + (unsigned)n;
    uint8_t body[34];
    for (int i = 0; i < n; i++) { body[i] = p[i]; sum += p[i]; }
    body[n] = (uint8_t)sum; body[n + 1] = (uint8_t)(sum >> 8);
    int m = n + 2;
    pk[k++] = 0xFF; pk[k++] = 0x01; pk[k++] = (uint8_t)n;
    for (int i = 0; i < m; i += 2) {
        uint8_t b0 = body[i], b1 = i + 1 < m ? body[i + 1] : 0;
        pk[k++] = 0x40 | (b0 & 15);
        pk[k++] = 0x40 | (b0 >> 4) | (b1 & 3) << 4;
        pk[k++] = 0x40 | (b1 >> 2);
    }
    for (int i = 0; i < k; i++) uart_rx_push(hw, u, pk[i]);
}

/* The buttons the controller sees: the frontend's, plus VFLASH_INPUT=
 * <hex VFLASH_BTN mask>@<from>-<to>[;...] for scripted headless runs. */
static uint32_t pad_buttons(HW *hw) {
    uint32_t in = hw->input;
    const char *e = getenv("VFLASH_INPUT");
    while (e && *e) {
        unsigned long m = 0, a = 0, b = 0;
        if (sscanf(e, "%lx@%lu-%lu", &m, &a, &b) == 3 && hw->frame >= a && hw->frame < b) in |= (uint32_t)m;
        e = strchr(e, ';');
        if (e) e++;
    }
    return in;
}

/* A stick report: the stick is digital here (full deflection per
 * direction), Enter is the report's button bit. The stick's Y axis sense is
 * a guess (up = high). */
static void pad_report(HW *hw, int u) {
    uint32_t in = pad_buttons(hw), x = 0x800, y = 0x800;
    if (in & VFLASH_BTN_LEFT)  x = 0x100;
    if (in & VFLASH_BTN_RIGHT) x = 0xF00;
    if (in & VFLASH_BTN_UP)    y = 0xF00;
    if (in & VFLASH_BTN_DOWN)  y = 0x100;
    uint8_t btn = (in & VFLASH_BTN_ENTER) ? 1 : 0;
    uint8_t p[4] = { (uint8_t)y, (uint8_t)((y >> 8 & 15) | (x & 15) << 4), (uint8_t)(x >> 4), btn };
    pad_send(hw, u, p, 4);
}

/* The joystick controller's n = 6 report (decoded at 0x10A6B694 for port 0,
 * 0x10A6BF2C for port 1): 8-bit stick field 1 (x here: vertical) in byte 0
 * bits 7-4 / byte 1 bits 3-0, field 2 (y: horizontal) in byte 1 bits 7-5 /
 * byte 2 bits 4-0, and 16 button bits: byte 2 bits 7-6, byte 3, byte 4
 * bits 5-0. VFLASH_PADBITS=<hex>@<from>-<to> forces raw button bits. */
static void pad_report6(HW *hw, int u) {
    /* The axes are signed, centred on 0 (the game kernel doubles the
     * magnitude and compares it with a threshold, 0x10B49194): the first
     * field (struct +6) is vertical, up positive, the second (+5)
     * horizontal. Down = -127 steps Dingo Rallye's menu down; the
     * horizontal sign is a guess. */
    uint32_t in = pad_buttons(hw), x = 0, y = 0, b = 0;
    if (in & VFLASH_BTN_UP)    x = 0x7F;
    if (in & VFLASH_BTN_DOWN)  x = 0x81;
    if (in & VFLASH_BTN_RIGHT) y = 0x7F;
    if (in & VFLASH_BTN_LEFT)  y = 0x81;
    if (in & VFLASH_BTN_ENTER) b |= 1u << 2;   /* OK (starts the game from the title) */
    /* Colour buttons also appear here: Multisports' event briefing takes green
     * (bit 12, "Start") and yellow (bit 11, "Anleitung") from this report, not from
     * the n = 2 events - found by trying each bit (2026-09-27). Red 10 / blue 13
     * follow the buttons' order on the pad and are unconfirmed. */
    if (in & VFLASH_BTN_RED)    b |= 1u << 10;
    if (in & VFLASH_BTN_YELLOW) b |= 1u << 11;
    if (in & VFLASH_BTN_GREEN)  b |= 1u << 12;
    if (in & VFLASH_BTN_BLUE)   b |= 1u << 13;
    const char *e = getenv("VFLASH_PADBITS");
    unsigned long m = 0, a = 0, z = 0;
    if (e && sscanf(e, "%lx@%lu-%lu", &m, &a, &z) == 3 && hw->frame >= a && hw->frame < z) b |= (uint32_t)m;
    unsigned long sx, sy;   /* VFLASH_STICK=<hex x>,<hex y>@<from>-<to>: raw stick values */
    e = getenv("VFLASH_STICK");
    if (e && sscanf(e, "%lx,%lx@%lu-%lu", &sx, &sy, &a, &z) == 4 && hw->frame >= a && hw->frame < z) {
        x = (uint32_t)sx; y = (uint32_t)sy;
    }
    uint8_t p[6] = {
        (uint8_t)((x & 15) << 4), (uint8_t)((x >> 4 & 15) | (y & 7) << 5),
        (uint8_t)((y >> 3 & 31) | (b & 3) << 6), (uint8_t)(b >> 2), (uint8_t)(b >> 10 & 63), 0 };
    pad_send(hw, u, p, 6);
}

/* A packet from the console has arrived: answer with a stick report. */
static void pad_reply(HW *hw, int u) { pad_report(hw, u); }

/* Once a frame: the controller on port 0 sends the four colour buttons as
 * n = 2 events when pressed (codes 1-4 set the game's last-button byte,
 * 0x10A6DD18), and a stick report every other frame. Only once the kernel
 * has opened the port (receive interrupt enabled). */
static void uart_int_check(HW *hw, int u);
static void pad_frame(HW *hw) {
    int u = 0;
    if (!(hw->uart[u].ier & 1)) return;
    uint32_t in = pad_buttons(hw), down = in & ~hw->pad_prev;
    hw->pad_prev = in;
    static const uint32_t col[4] = { VFLASH_BTN_RED, VFLASH_BTN_YELLOW, VFLASH_BTN_GREEN, VFLASH_BTN_BLUE };
    for (int i = 0; i < 4; i++)
        if (down & col[i]) { uint8_t ev[2] = { (uint8_t)(i + 1), 0 }; pad_send(hw, u, ev, 2); }
    if (!(hw->frame & 1) || (down & 0x10F)) pad_report(hw, u);
    if (hw->frame & 1) pad_report6(hw, u);
    uart_int_check(hw, u);
}

static void uart_int_check(HW *hw, int u) {
    typeof(hw->uart[0]) *s = &hw->uart[u];
    int on = ((s->ier & 1) && s->rx_n) || (s->pending & s->ier & 2);
    int_set(hw, u ? 2 : INT_SERIAL, on);
}

static void uart_out(HW *hw, int u, uint8_t ch) {
    typeof(hw->uart[0]) *s = &hw->uart[u];
    {
        static int n[2];
        if (n[u]++ < 96)
            printf("[UART%d] tx %02X PC=%08X\n", u, ch, hw->cpu->r[15]);
    }
    /* controller packets */
    if (ch == 0xFF) s->pk_len = 0;
    if (s->pk_len < (int)sizeof s->pk) s->pk[s->pk_len++] = ch;
    if (s->pk_len >= 3 && s->pk[0] == 0xFF &&
        s->pk_len == (s->pk[2] + 1) / 2 * 3 + 6) {
        s->pk_len = 0;
        if (!getenv("VFLASH_NOPAD")) pad_reply(hw, u);
    }
    if (ch == '\n' || s->len == (int)sizeof(s->line) - 1) {
        s->line[s->len] = 0;
        printf("[UART%d] %s\n", u, s->line);
        s->len = 0;
    } else if (ch != '\r') {
        s->line[s->len++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
}

static uint32_t uart_read(HW *hw, uint32_t pa) {
    int u = (pa >> 16) == 0x9003;
    typeof(hw->uart[0]) *s = &hw->uart[u];
    switch (pa & 0x3F) {
    case 0x00: {
        if (s->lcr & 0x80) return s->dll;
        if (!s->rx_n) return 0;
        uint8_t b = s->rx[s->rx_h];
        s->rx_h = (s->rx_h + 1) % sizeof s->rx; s->rx_n--;
        uart_int_check(hw, u);
        return b;
    }
    case 0x04: return (s->lcr & 0x80) ? s->dlm : s->ier;
    case 0x08:   /* IIR, FIFOs enabled: C4 data, C2 transmitter empty, C1 none */
        if ((s->ier & 1) && s->rx_n) return 0xC4;
        if (s->pending & s->ier & 2) { s->pending &= ~2; uart_int_check(hw, u); return 0xC2; }
        return 0xC1;
    case 0x0C: return s->lcr;
    case 0x10: return 0;
    case 0x14: return 0x60 | (s->rx_n ? 1 : 0);
    case 0x18: return 0;
    }
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}
static void uart_write(HW *hw, uint32_t pa, uint32_t v) {
    int u = (pa >> 16) == 0x9003;
    typeof(hw->uart[0]) *s = &hw->uart[u];
    switch (pa & 0x3F) {
    case 0x00:
        if (s->lcr & 0x80) { s->dll = v; return; }
        uart_out(hw, u, (uint8_t)v);
        s->pending |= 2; uart_int_check(hw, u);
        return;
    case 0x04:
        if (s->lcr & 0x80) s->dlm = v; else { s->ier = v & 0x0F; uart_int_check(hw, u); }
        return;
    case 0x0C: s->lcr = v; return;
    case 0x08: case 0x10: case 0x20: return;
    }
    unmodelled(hw, pa, 1, 32, v);
}

/* ---------------------------------------------------------------- GPIO */

static uint32_t gpio_read(HW *hw, uint32_t pa) {
    int b = (pa >> 16) == 0x900D, port = pa >> 6 & 3, r = pa & 0x3F;
    typeof(hw->gpio[0]) *g = &hw->gpio[b];
    switch (r) {
    case 0x10: return g->dir[port];
    case 0x14: return g->out[port];
    case 0x18: {
        static int n;
        /* A pin set as an output (+0x10 bit set) reads back its latch; the
         * others read the input. The kernel writes B0 bit 2 while it is an
         * input, and must still see it drop before starting a BOOT.BIN. */
        uint32_t v = (g->in[port] & ~g->dir[port]) | (g->out[port] & g->dir[port]);
        if (getenv("VFLASH_GPIOOR")) v = g->in[port] | g->out[port];
        if (b && port == 0 && getenv("VFLASH_B0SET") && hw->frame >= strtoull(getenv("VFLASH_B0SET"), NULL, 0))
            v |= 1;
        else if (b && port == 0 && hw->cd && hw->cd->is_open && !getenv("VFLASH_GPIOB"))
            v &= ~1u;
        if (b && port == 0 && hw->cd && hw->cd->is_open && hw->frame < 120 &&
            !getenv("VFLASH_GPIOB"))
            v = (v & ~1u) | 4;
        if (b && port == 0 && getenv("VFLASH_PWRHOLD") &&
            hw->frame >= strtoull(getenv("VFLASH_PWRHOLD"), NULL, 0))
            v &= ~5u;   /* VFLASH_PWRHOLD=<frame>: wake inputs (B0 bits 0, 2) released */
        /* A1 bit 6 (pin 14) is the disc lid switch, high while the lid is
         * open; the kernel debounces it (0x10053828). VFLASH_LID=<from>-<to>
         * holds the lid open over a frame range. */
        if (!b && port == 1) {
            const char *l = getenv("VFLASH_LID");
            unsigned long a0 = 0, a1 = 0;
            if (l && sscanf(l, "%lu-%lu", &a0, &a1) == 2 && hw->frame >= a0 && hw->frame < a1)
                v |= 0x40;
        }
        static int n2;
        if (n++ < 60 || (b && port == 0 && n < 400) || (hw->frame >= 2300 && n2++ < 80))
            printf("[GPIO] read %c%d in -> %02X (in %02X out %02X dir %02X) PC=%08X\n", 'A' + b, port,
                   v, g->in[port], g->out[port], g->dir[port], hw->cpu->r[15]);
        return v;
    }
    }
    if (!(pa & 0xF00)) return g->regs[port][r >> 2];
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}

static void gpio_write(HW *hw, uint32_t pa, uint32_t v) {
    int b = (pa >> 16) == 0x900D, port = pa >> 6 & 3, r = pa & 0x3F;
    typeof(hw->gpio[0]) *g = &hw->gpio[b];
    switch (r) {
    case 0x10: g->dir[port] = v; return;
    case 0x14: g->out[port] = v; return;
    }
    if (!(pa & 0xF00)) { g->regs[port][r >> 2] = v; return; }
    unmodelled(hw, pa, 1, 32, v);
}

/* ---------------------------------------------------------------- serial shifter */

/* 0xC4000000: serial bus to the CD servo/decoder chip. A transfer shifts
 * +0x00 bits of +0x08/+0x0C out; bit 0 of +0x14 starts it and bit 0 of +0x64
 * reports it done (write 1 to clear). A read also sets bit 1 of +0x14, asks
 * for +0x04 bits back, waits for bit 1 of +0x64 and takes them from +0x10.
 * The chip itself is modelled in cdsp.c. */
static uint64_t hw_us(HW *hw) {
    return hw->cpu->cycles * 1000000ull / hw->cpu_hz;
}

static uint32_t ser_read(HW *hw, uint32_t pa) {
    uint32_t r = pa & 0xFF;
    if (r == 0x18)   /* bit 1: the DSP's SENS output */
        return cdsp_sens(&hw->dsp, hw_us(hw)) ? 2 : 0;
    if (r < 0x100) return hw->ser[r >> 2];
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}

static void cd_int_check(HW *hw);
#define CD_FOUND   0x04    /* decoder interrupt bits, see "CD decoder" */
#define CD_END     0x10
static uint64_t cd_timer_unit_us(HW *hw);
static void cd_release(HW *hw, uint32_t v);
static void cd_kick(HW *hw);

static void ser_write(HW *hw, uint32_t pa, uint32_t v) {
    uint32_t r = pa & 0xFF;
    switch (r) {
    case 0x64: hw->ser[0x64 >> 2] &= ~v; cd_int_check(hw); return;
    case 0x68: hw->ser[0x68 >> 2] = v; cd_int_check(hw); return;
    case 0x74: cd_release(hw, v); return;
    case 0x40:
        if ((v & 4) && !(hw->ser[0x40 >> 2] & 4)) { hw->cd_xfer = 0; cd_kick(hw); }
        if (!(v & 4) && (hw->ser[0x40 >> 2] & 4)) {   /* stopped by the CPU */
            hw->cd_xfer = 0;
            hw->ser[0x64 >> 2] |= CD_END;
            hw->ser[0x40 >> 2] = v;
            cd_int_check(hw);
        }
        if ((v & 4) && !(hw->ser[0x40 >> 2] & 4) && cdsp_log)
            printf("[CD] %llu ms search armed: target %08X len %08X slots %02X, pickup at lba %d\n",
                   (unsigned long long)(hw_us(hw) / 1000), hw->ser[0x1C >> 2], hw->ser[0x20 >> 2],
                   hw->ser[0x70 >> 2], cdsp_lba(&hw->dsp));
        break;
    case 0x1C: hw->cd_xfer = 0; break;
    case 0x60: {
        uint32_t old = hw->ser[0x60 >> 2];
        hw->ser[0x60 >> 2] = v;
        if (v != old && cdsp_log) {
            static int n;
            if (n++ < 20000) printf("[CD] %llu ms timers +60 = %08X\n", (unsigned long long)(hw_us(hw) / 1000), v);
        }
        if (!(v >> 24)) hw->cd_tmo_us = 0;
        else if (v >> 24 != old >> 24)
            hw->cd_tmo_us = hw_us(hw) + (v >> 24) * cd_timer_unit_us(hw);
        if ((v >> 20 & 15) != (old >> 20 & 15)) hw->cd_tick_us = 0;
        return;
    }
    case 0x14:
        if (v & 1) {
            int rd = (v & 2) != 0;
            uint32_t rbits = hw->ser[1];
            uint64_t data = (uint64_t)hw->ser[3] << 32 | hw->ser[2];
            cdsp_command(&hw->dsp, hw_us(hw), hw->ser[0], data);
            uint32_t reply = rd ? cdsp_readback(&hw->dsp, hw_us(hw), hw->ser[0], data, rbits) : 0;
            static int rdlog;
            if (cdsp_log && (rd ? rdlog++ < 30000 : hw->ser_log++ < 400000))
                printf("[SER] %s %2u bits %08X%08X%s ctl=%08X -> %08X PC=%08X\n",
                       rd ? "RD" : "WR", hw->ser[0], hw->ser[3], hw->ser[2],
                       rd ? "" : "", v, reply, hw->cpu->r[15]);
            /* VFLASH_CDSTACK: with each auto-sequence command, the words on the
             * stack that point into kernel code - a rough call chain. */
            if (getenv("VFLASH_CDSTACK") && hw->ser[0] >= 8 &&
                (data >> (hw->ser[0] - 4) & 15) == 4) {
                uint32_t sp = hw->cpu->r[13];
                printf("[CDSTACK] %llu ms cmd %llX:", (unsigned long long)(hw_us(hw) / 1000), (unsigned long long)data);
                for (int i = 0, n = 0; i < 128 && n < 10; i++) {
                    uint32_t w = hw_read32(hw, sp + 4 * i);
                    if (w >= 0x10010000 && w < 0x100A0000 && !(w & 3)) { printf(" %08X", w); n++; }
                }
                printf("\n");
            }
            if (rd) {
                hw->ser[0x10 >> 2] = reply;
                hw->ser[0x64 >> 2] |= 3;
            } else {
                hw->ser[0x64 >> 2] |= 1;
            }
            v &= ~3u;   /* go and read bits clear themselves */
        }
        hw->ser[r >> 2] = v;
        return;
    }
    if (r >= 0x1C && cdsp_log && hw->ser[r >> 2] != v) {
        static int n;
        if (n++ < 200000)
            printf("[CDREG] +%02X = %08X (was %08X) PC=%08X LR=%08X\n", r, v, hw->ser[r >> 2],
                   hw->cpu->r[15], hw->cpu->r[14]);
    }
    hw->ser[r >> 2] = v;
}


/* ---------------------------------------------------------------- CD decoder */

/* The decoder half of the 0xC4000000 block, as the ROM kernel's HAL
 * (0x10061768-0x10063240) drives it. Interrupt line 14; +0x64 status (write
 * 1 to clear; bits 0-1 are the serial bus's done flags) and +0x68 enable:
 *   bit 5   subcode Q frame latched in +0x54 (bit 16 valid, 15-8 control/ADR,
 *           7-0 track), +0x58 (index, M, S, F) and +0x5C (0, AM, AS, AF), BCD
 * +0x40 control: bit 0 subcode/header monitor on, bit 2 decoder running.
 * +0x4C: BCD M:S:F of the last sector header, bit 31 valid.
 * Everything else is still a register file. */
#define INT_CD     14
#define CD_SUBQ    0x20

static void cd_int_check(HW *hw) {
    uint32_t p = hw->ser[0x64 >> 2] & hw->ser[0x68 >> 2] & ~3u;
    if (cdsp_log && getenv("VFLASH_CDINT")) {
        static uint32_t last;
        if (p != last) printf("[CD] %llu us int pending %05X\n", (unsigned long long)hw_us(hw), p);
        last = p;
    }
    int_set(hw, INT_CD, p != 0);
}

static uint32_t bcd8(uint32_t v) { return (v / 10) << 4 | (v % 10); }

static uint8_t *cd_ram(HW *hw, uint32_t pa, uint32_t len) {
    return (pa - RAM_BASE < RAM_SIZE && pa - RAM_BASE + len <= RAM_SIZE) ? hw->ram + (pa - RAM_BASE) : NULL;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* Sector data path, as far as the HAL shows it:
 *   +0x1C  target header M:S:F (BCD); bit 24 = take whatever comes
 *   +0x20  transfer length (bit 24 = unlimited); the header probe
 *          (0x10062BE0) sets 4 with +0x40 mode 01 and waits for bits 4+12
 *   +0x24  header-mode destination
 *   +0x28/+0x2C  data ring start / last word (mode 10/11 reads, 0x1006241C)
 *   +0x3C  descriptor ring: 4 slots of 1 KB. A slot holds status (word 0,
 *          1 = good), the sector data's address (word 1) and the header at
 *          +0x14 (the probe reads its mode byte at +0x17)
 *   +0x40  bits 3-4 data mode, bit5 split CDDA, bit2 run: search for the target, then
 *          transfer while every header follows on from the last
 *   +0x70  bits 1-0 oldest unread slot, bits 4-2 slots filled
 *   +0x74  bit 0 releases the oldest slot, bit 1 releases them all
 * Interrupts: bit 12 a slot is filled; bit 2 the target was found (read
 * state 0 -> 2, 0x10061768); bit 4 the transfer ended - the length ran out
 * or the CPU cleared bit 2. Bit 4 sets read state 3 and posts event 8,
 * whose handler (0x100640B8) restarts a request that arrived while a read
 * was running and was parked (0x100637A0) behind a stop. */
#define CD_SLOT    0x1000

static void cd_deliver(HW *hw, int32_t lba) {
    uint32_t *s = hw->ser;
    uint32_t ctl = s[0x40 >> 2], mode = ctl >> 3 & 3;
    int cdda=(ctl&0x78)==0x60;
    uint32_t a = (uint32_t)lba + 150;
    uint32_t msf = bcd8(a / 4500) << 16 | bcd8(a / 75 % 60) << 8 | bcd8(a % 75);
    /* Every sector must carry the header the target register expects - it
     * advances with each one taken, so a transfer only continues in order
     * and resumes by itself when the pickup comes round again after a
     * pause. Bit 4 only marks the first match after the search is armed. */
    if (!(s[0x1C >> 2] & 0x1000000) && (s[0x1C >> 2] & 0xFFFFFF) != msf) return;
    if (!hw->cd_xfer) {
        hw->cd_xfer = 1;
        s[0x64 >> 2] |= CD_FOUND;
    }
    uint32_t s70 = s[0x70 >> 2], rd = s70 & 3, cnt = s70 >> 2 & 7;
    if (cnt >= 4) {   /* overrun */
        if (!(s[0x70 >> 2] & 0x20) && cdsp_log) printf("[CD] slot overrun at lba %d\n", lba);
        s[0x70 >> 2] |= 0x20;
        return;
    }
    uint8_t *desc = cd_ram(hw, s[0x3C >> 2] + ((rd + cnt) & 3) * 0x400, 0x400);
    if (!desc) return;
    uint8_t hdr[4] = { (uint8_t)(msf >> 16), (uint8_t)(msf >> 8), (uint8_t)msf, 1 };
    uint32_t ptr;
    if(cdda) {
        uint8_t raw[2352];
        if(!cdrom_read_raw(hw->cd,(uint32_t)lba,raw)) return;
        ptr=s[0x24/4];
        int result=cdda_dma_sector(s,raw,hw->ram,RAM_SIZE,RAM_BASE);
        if(!(result&CDDA_DMA_DONE)) return;
        if(result&CDDA_DMA_MARK) s[0x64/4]|=0x10000;
    } else if (mode == 1) {
        ptr = s[0x24 >> 2];
        uint8_t *p = cd_ram(hw, ptr, 4);
        if (p) memcpy(p, hdr, 4);
    } else {
        uint32_t start = s[0x28 >> 2], end = s[0x2C >> 2] + 4;
        uint32_t dma = s[0x24 >> 2];
        if (dma < start || dma + 2048 > end) dma = start;
        ptr = dma;
        uint8_t *p = cd_ram(hw, ptr, 2048);
        if (p && !cdrom_read_sector(hw->cd, (uint32_t)lba, p)) memset(p, 0, 2048);
        dma += 2048;
        if (dma + 2048 > end) dma = start;
        s[0x24 >> 2] = dma;
        /* +0x80: interrupt bit 16 when the DMA write address passes it
         * (0x10062750 arms it a few sectors into the ring) - including
         * a mark at the ring's start, reached when the address wraps.
         * +0x84 bit 0 also ends the transfer at this boundary. Cars'
         * streaming HAL (0x109F54B4) disables IRQ16 in this mode and uses
         * the normal transfer-end IRQ to pause before unread ring data.
         * Its producer sets the guard one sector behind the read pointer. */
        if ((s[0x80 >> 2] > ptr && s[0x80 >> 2] <= ptr + 2048) || s[0x80 >> 2] == dma) {
            s[0x64 >> 2] |= 0x10000;
            if (s[0x84 >> 2] & 1) {
                hw->cd_xfer = 0;
                s[0x40 >> 2] &= ~4u;
                s[0x64 >> 2] |= CD_END;
            }
        }
    }
    /* The slot carries the rest of the raw sector for the kernel's software
     * ECC (0x10072128), which sees one sector as header = slot+0x14, data =
     * the 2 KB in the ring, then slot+0x18.. for raw bytes 0x810-0x92F (EDC,
     * zeros, P and Q parity); slot+0x138 holds 294 bytes of C2 error flags,
     * one bit per raw byte, all clear. */
    memset(desc, 0, 0x138 + 294);
    put32(desc, 1);
    put32(desc + 4, ptr);
    memcpy(desc + 0x14, hdr, 4);
    {
        static uint8_t raw[2352];
        if (!cdda && cdrom_read_raw(hw->cd, (uint32_t)lba, raw)) {
            memcpy(desc + 0x14, raw + 12, 4);
            memcpy(desc + 0x18, raw + 0x810, 0x120);
        }
    }
    /* The target follows the transfer, so a read that is paused and re-armed
     * (the driver backs off two tracks between bursts) picks up where it
     * left off. */
    {
        uint32_t n = a + 1;
        s[0x1C >> 2] = (s[0x1C >> 2] & 0xFF000000u) |
                       bcd8(n / 4500) << 16 | bcd8(n / 75 % 60) << 8 | bcd8(n % 75);
    }
    s[0x70 >> 2] = (s70 & ~0x1Cu) | (cnt + 1) << 2;
    s[0x64 >> 2] |= CD_SLOT;
    cd_kick(hw);
    hw->cd_sectors++; hw->cd_last_lba = (uint32_t)lba;
    if (!(s[0x20 >> 2] & 0x1000000)) {
        uint32_t left = s[0x20 >> 2] & 0xFFFFFF;
        left = mode == 1 ? 0 : left ? left - 1 : 0;
        s[0x20 >> 2] = (s[0x20 >> 2] & 0xFF000000u) | left;
        if (!left) { hw->cd_xfer = 0; s[0x40 >> 2] &= ~4u; s[0x64 >> 2] |= CD_END; }
    }
    if (cdsp_log) {
        static int n;
        if (n++ < 200000)
            printf("[CD] %llu ms deliver lba %d mode %u -> %08X slot %u left %06X\n",
                   (unsigned long long)(hw_us(hw) / 1000), lba, mode, ptr,
                   (rd + cnt) & 3, s[0x20 >> 2] & 0xFFFFFF);
    }
}

static void cd_release(HW *hw, uint32_t v) {
    uint32_t s70 = hw->ser[0x70 >> 2], rd = s70 & 3, cnt = s70 >> 2 & 7;
    if (v & 2) cnt = 0;
    else if ((v & 1) && cnt) { cnt--; rd = (rd + 1) & 3; }
    hw->ser[0x70 >> 2] = (s70 & ~0x3Fu) | cnt << 2 | rd;
    if (cnt) hw->ser[0x64 >> 2] |= CD_SLOT;
    cd_int_check(hw);
}

/* One sector's worth of disc passing under the pickup. */
static void cd_sector(HW *hw) {
    CDSP *d = &hw->dsp;
    uint32_t *s = hw->ser;
    if (!cdsp_reading(d)) return;
    int32_t lba = cdsp_lba(d);
    uint8_t q[10];
    cdsp_subq(d, lba, q);
    s[0x54 >> 2] = 1u << 16 | q[0] << 8 | q[1];
    s[0x58 >> 2] = (uint32_t)q[2] << 24 | q[3] << 16 | q[4] << 8 | q[5];
    s[0x5C >> 2] = (uint32_t)q[6] << 24 | q[7] << 16 | q[8] << 8 | q[9];
    /* data sectors, including the 2 s pregap before track 1 (headers from 00:00:00) */
    if (lba >= -150 && hw->cd && lba < (int32_t)hw->cd->sector_count) {
        uint32_t a = (uint32_t)lba + 150;
        s[0x4C >> 2] = 0x80000000u | bcd8(a / 4500) << 16 | bcd8(a / 75 % 60) << 8 | bcd8(a % 75);
    }
    if ((s[0x40 >> 2] & 4) && ((s[0x40 >> 2] & 0x18) || (s[0x40 >> 2]&0x78)==0x60) && lba >= 0 &&
        hw->cd && lba < (int32_t)hw->cd->sector_count)
        cd_deliver(hw, lba);
    s[0x64 >> 2] |= CD_SUBQ;
    cd_int_check(hw);
    if (cdsp_log) {
        static int n; static int32_t prev = -99999;
        int jump = lba != prev + 1; prev = lba;
        if (jump && n++ < 300000)
            printf("[CD] lba %d Q %02X %02X %02X %02X:%02X:%02X %02X %02X:%02X:%02X ctl=%08X en=%08X s70=%02X tgt=%06X\n",
                   lba, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9],
                   s[0x40 >> 2], s[0x68 >> 2], s[0x70 >> 2], s[0x1C >> 2] & 0xFFFFFF);
    }
}

/* +0x60: two timers counting in units of (bits 19-0) APB clocks - the ROM
 * sets 0xB8600, 10 ms at 75 MHz. Bits 23-20 are a lost-signal watchdog
 * (interrupt bit 14): the driver arms it once focus locks (0x1005E50C) and
 * refocuses when it fires, so it only runs out while no frames come off the
 * disc. Bits 31-24 are a one-shot timeout (bit 15). Both restart
 * whenever the field is written. */
static uint64_t cd_timer_unit_us(HW *hw) {
    uint64_t apb = hw->cpu_hz / ((hw->pmu.clocks >> 8 & 7) + 1);
    uint64_t u = (uint64_t)(hw->ser[0x60 >> 2] & 0xFFFFF) * 1000000u / (apb ? apb : 1);
    return u ? u : 1;
}

/* Once a data transfer is under way (+0x40 bit 2, mode bits 4-3, first
 * sector found) only sectors actually taken keep the watchdog quiet: the
 * game kernel's driver can pause the pickup ahead of an unfinished stream
 * and relies on bit 14 to notice (0x10A0BAF8 stops the decoder and the
 * request is re-issued). The search before the first sector is not timed -
 * the ROM arms reads eight sectors early at single speed (107 ms). */
static int cd_decoding(HW *hw) {
    uint32_t ctl=hw->ser[0x40/4];
    return (ctl&4) && ((ctl&0x18) || (ctl&0x78)==0x60) && hw->cd_xfer;
}

/* A sector taken restarts both timers: the timeout (bits 31-24) too, as the
 * game kernel's driver leaves its 2 s read timeout armed across a whole
 * stream and only expects it when data stops coming. */
static void cd_kick(HW *hw) {
    uint32_t tick = hw->ser[0x60 >> 2] >> 20 & 15, tmo = hw->ser[0x60 >> 2] >> 24;
    if (tick && hw->cd_tick_us) hw->cd_tick_us = hw_us(hw) + tick * cd_timer_unit_us(hw);
    if (tmo && hw->cd_tmo_us) hw->cd_tmo_us = hw_us(hw) + tmo * cd_timer_unit_us(hw);
}

static void cd_timers(HW *hw, uint64_t now) {
    uint32_t t = hw->ser[0x60 >> 2];
    uint32_t tick = t >> 20 & 15, tmo = t >> 24;
    if (tick && cdsp_reading(&hw->dsp) && !cd_decoding(hw) && hw->cd_tick_us)
        hw->cd_tick_us = now + tick * cd_timer_unit_us(hw);
    if (tick && now >= hw->cd_tick_us) {
        if (hw->cd_tick_us) { hw->ser[0x64 >> 2] |= 0x4000; cd_int_check(hw); }
        hw->cd_tick_us = now + tick * cd_timer_unit_us(hw);
    }
    if (!tick) hw->cd_tick_us = 0;
    if (tmo && hw->cd_tmo_us && now >= hw->cd_tmo_us) {
        hw->ser[0x64 >> 2] |= 0x8000; cd_int_check(hw);
        hw->cd_tmo_us = 0;
    }
}

static void cd_tick(HW *hw) {
    uint64_t now = hw_us(hw);
    cdsp_advance(&hw->dsp, now);
    cd_timers(hw, now);
    if (now < hw->cd_next_us) return;
    hw->cd_next_us = now + (uint64_t)(1000000.0 / (75.0 * cdsp_speed(&hw->dsp)));
    cd_sector(hw);
}

/* ---------------------------------------------------------------- DMA controller */

/* 0xBC000000: the "Maiko DMAC" (kernel driver 0x100960E0-0x10096D80), IRQ 12.
 * Eight channels of four registers at +0x100 + 16n: +0 count (bits 9-0,
 * elements) | config (byte 2 bits 1-0 = element size 1/2/4), +4 source,
 * +8 destination. Global: +0x08 a 3-bit request line per channel, +0x0C
 * busy channels, +0x10 start (1 << n), +0x18 interrupt enable, +0x1C
 * end-of-transfer status (write 1 to clear). Transfers here are memory to
 * memory and finish at once. */
#define INT_DMAC 12

static void dmac_int_check(HW *hw) {
    int_set(hw, INT_DMAC, (hw->dma[0x1C >> 2] & hw->dma[0x18 >> 2]) != 0);
}

static void dmac_run(HW *hw, int ch) {
    uint32_t *c = &hw->dma[(0x100 >> 2) + ch * 4];
    static const uint32_t esize[4] = { 1, 2, 4, 4 };
    uint32_t len = (c[0] & 0x3FF) * esize[c[0] >> 16 & 3];
    uint32_t src = c[1], dst = c[2];
    uint8_t *s = cd_ram(hw, src, len), *d = cd_ram(hw, dst, len);
    if (s && d) memmove(d, s, len);
    else {
        /* A device on one side: move it a word at a time over the bus. */
        for (uint32_t i = 0; i < len; i += 4)
            hw_write32(hw, dst + i, hw_read32(hw, src + i));
    }
    if (hw->watch_len && dst < hw->watch_lo + hw->watch_len && hw->watch_lo < dst + len)
        printf("[WATCH] DMAC ch%d %08X -> %08X, %u bytes frame %lu\n", ch, src, dst, len,
               (unsigned long)hw->frame);
    if (hw->dma_log++ < 50 || getenv("VFLASH_DMALOG"))
        printf("[DMAC] ch%d %08X -> %08X, %u bytes (cfg %08X)\n", ch, src, dst, len, c[0]);
    hw->dma[0x1C >> 2] |= 1u << ch;
    hw->dma[0x0C >> 2] &= ~(1u << ch);
}

static uint32_t dmac_read(HW *hw, uint32_t pa) {
    uint32_t r = pa & 0xFFF;
    if (r < 0x200) return hw->dma[r >> 2];
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}

static void dmac_write(HW *hw, uint32_t pa, uint32_t v) {
    uint32_t r = pa & 0xFFF;
    if (r >= 0x200) { unmodelled(hw, pa, 1, 32, v); return; }
    switch (r) {
    case 0x10:
        for (int ch = 0; ch < 8; ch++) if (v >> ch & 1) dmac_run(hw, ch);
        dmac_int_check(hw);
        return;
    case 0x1C: hw->dma[r >> 2] &= ~v; dmac_int_check(hw); return;
    case 0x18: hw->dma[r >> 2] = v; dmac_int_check(hw); return;
    }
    hw->dma[r >> 2] = v;
}

/* ---------------------------------------------------------------- 0xA0000000, 0xA4000000 */

/* 0xA0000000: a window onto an optional external flash. Both kernels look
 * for a "New_Flash_Driver" image there (magic at 0x100A0648, stored with
 * each word byte-swapped) and for a "VFLASHQA" test image at +0xC0, after
 * writing 2 to the SPI master at 0xA1000000. Retail units have neither, so
 * the window reads as erased. */
static uint32_t xflash_read(HW *hw, uint32_t pa) { (void)hw; (void)pa; return 0xFFFFFFFFu; }

/* 0xA4000000: a format converter the game kernels use: write an operand to
 * a register and read the result back at once.
 *   +0x00  IEEE single -> half (the graphics engine's float format: model
 *          vertices are halves; the projection scales 0x78/0x79 are made
 *          here from 120/tan and 256/tan of half the field of view,
 *          0x10A7B8E8)
 *   +0x18  20.12 fixed -> 16 bits, read as 4.12 (matrices), kept as written
 * The other registers keep what was written. */
static uint16_t f32_to_half(uint32_t f) {
    uint32_t s = f >> 16 & 0x8000, e = f >> 23 & 0xFF, m = f & 0x7FFFFF;
    if (e == 0xFF) return (uint16_t)(s | 0x7C00 | (m ? 0x200 : 0));
    int he = (int)e - 127 + 15;
    if (he >= 31) return (uint16_t)(s | 0x7C00);
    if (he <= 0) {                       /* subnormal or zero */
        if (he < -10) return (uint16_t)s;
        m |= 0x800000;
        uint32_t sh = (uint32_t)(14 - he), hm = m >> sh;
        if ((m >> (sh - 1)) & 1) hm++;
        return (uint16_t)(s | hm);
    }
    uint32_t h = s | (uint32_t)he << 10 | m >> 13;
    if (m & 0x1000) h++;                 /* round half up */
    return (uint16_t)h;
}

/* The converters produce/consume the engine's 16-bit float (ge_f16_to):
 * +0x00 IEEE single -> f16, +0x04 f16 -> single, +0x18 20.12 -> f16,
 * +0x1C f16 -> 20.12 (0x10A6E6B0 and its software inverse 0x10A6E658 in
 * Disney Princess). VFLASH_HALF=1 restores the old half / echo model. */
static uint32_t mathu_read(HW *hw, uint32_t pa) {
    static int old = -1;
    if (old < 0) old = getenv("VFLASH_HALF") && atoi(getenv("VFLASH_HALF"));
    uint32_t v = hw->mathu[(pa & 0x3F) >> 2];
    if (old) return (pa & 0x3F) == 0 ? f32_to_half(v) : v;
    union { uint32_t u; float f; } c;
    switch (pa & 0x3F) {
    case 0x00: c.u = v; return ge_f16_from(c.f);
    case 0x04: c.f = (float)ge_f16_to((uint16_t)v); return c.u;
    case 0x18: return ge_f16_from((int32_t)v / 4096.0);
    case 0x1C: return (uint32_t)(int32_t)lround(ge_f16_to((uint16_t)v) * 4096.0);
    }
    return v;
}

static void mathu_write(HW *hw, uint32_t pa, uint32_t v) {
    if (getenv("VFLASH_MATHLOG")) {
        static int n;
        if (n++ < 3000 && (pa & 0x3F) == 0)
            printf("[MATH] +%02X <- %08X PC=%08X LR=%08X\n", pa & 0x3F, v, hw->cpu->r[15], hw->cpu->r[14]);
    }
    hw->mathu[(pa & 0x3F) >> 2] = v;
}

/* ---------------------------------------------------------------- SPI master */

/* 0xA1000000: byte-wide SPI master. +0x1C holds the bytes to send (MSB
 * first), +0x04 the length in bits 16+ and a start bit, +0x10 bit 0 is "done",
 * +0x20 the bytes received. The kernel's first command is 0xAB (a serial
 * flash's "release from power-down / read signature"). No device is attached
 * yet: transfers finish at once and MISO floats high. */
static uint32_t spi_read(HW *hw, uint32_t pa) {
    switch (pa & 0xFF) {
    case 0x10: return 1;
    case 0x20: return 0xFFFFFFFFu;
    }
    return hw->spi[(pa & 0x3F) >> 2];
}

static void spi_write(HW *hw, uint32_t pa, uint32_t v) {
    if ((pa & 0xFF) == 0x04 && (v & 1) && hw->spi_log++ < 40)
        printf("[SPI] tx %08X len=%u PC=%08X\n", hw->spi[0x1C >> 2], v >> 16, hw->cpu->r[15]);
    hw->spi[(pa & 0x3F) >> 2] = v;
}

/* ---------------------------------------------------------------- display */

/* 0xA8000000: the display controller. The kernel clears 0xCD registers, then
 * sets +0x90/+0x94 = 240 << 16 | 512 and +0x98 = 0x10800000, a framebuffer in
 * SDRAM. Held as a register file and logged while its layout is worked out. */
static uint32_t lcd_read(HW *hw, uint32_t pa) {
    return hw->lcd[(pa & 0xFFF) >> 2];
}

/* The block is also a command-list engine, as the game kernels drive it
 * (0x10A36B7C in a BOOT.BIN kernel): +0x28 takes a list's address, writing
 * 1 to +0x00 runs it, and the end is reported on IRQ 8 with status +0x14 =
 * 0xFD08 (list end opcode in bits 15-8, bit 3 done) - here at the next
 * vblank, as the lists are not timed yet; the handler clears it
 * by writing 0x3F. The list itself is not executed yet - it is logged. */
#define INT_LCD 8

static void lcd_int_check(HW *hw) {
    int_set(hw, INT_LCD, (hw->lcd[0x14 >> 2] & 0x3F) != 0);
}

/* VFLASH_GECAP=<file>,<frame>[,<frames>]: record the graphics engine's
 * input for gereplay (tools/gereplay.c) - the GE state and all of RAM before
 * the first list from <frame> on, then before each later list only the 4 KB
 * pages the CPU changed (what the GE itself drew is left out, so a replay
 * shows what the current ge.c draws), with the list address and surface.
 * Records:
 *   "GECAP2" u32 sizeof(GE), GE
 *   'P' u32 addr, 4096 bytes       a RAM page
 *   'L' u32 list, u32 surface (row 0, as GE.surface), u32 top, u32 frame
 *   'E'                            end */
static uint8_t *ge_cap_shadow;
static int ge_cap_on;

/* after a list has run: take what the GE drew into the shadow */
static void ge_capture_done(HW *hw) {
    if (ge_cap_on) memcpy(ge_cap_shadow, hw->ram, RAM_SIZE);
}

static void ge_capture(HW *hw, uint32_t list) {
    static FILE *f;
    uint8_t *shadow = ge_cap_shadow;
    static uint64_t from, to;
    static int state;   /* 0 unparsed, 1 waiting, 2 recording, 3 done */
    if (state == 0) {
        const char *e = getenv("VFLASH_GECAP");
        char path[512];
        unsigned long fr = 0, n = 1;
        state = 3;
        if (!e || sscanf(e, "%511[^,],%lu,%lu", path, &fr, &n) < 2) return;
        if (!(f = fopen(path, "wb")) || !(shadow = ge_cap_shadow = calloc(1, RAM_SIZE))) return;
        from = fr; to = fr + n; state = 1;
    }
    if (state == 1 && hw->frame >= from) {
        uint32_t sz = sizeof(GE);
        fwrite("GECAP2", 1, 6, f);
        fwrite(&sz, 4, 1, f);
        fwrite(&hw->ge, sizeof(GE), 1, f);
        memset(shadow, 0xA5, RAM_SIZE);   /* forces every page out first */
        state = 2;
        ge_cap_on = 1;
    }
    if (state != 2) return;
    if (hw->frame >= to) {
        fputc('E', f); fclose(f); f = NULL; state = 3; ge_cap_on = 0;
        printf("[GECAP] done\n");
        return;
    }
    for (uint32_t o = 0; o < RAM_SIZE; o += 4096)
        if (memcmp(shadow + o, hw->ram + o, 4096)) {
            uint32_t a = 0x10000000u + o;
            memcpy(shadow + o, hw->ram + o, 4096);
            fputc('P', f); fwrite(&a, 4, 1, f); fwrite(hw->ram + o, 1, 4096, f);
        }
    uint32_t rec[4] = { list, hw->ge.surface, (uint32_t)hw->ge.top, (uint32_t)hw->frame };
    fputc('L', f); fwrite(rec, 4, 4, f);
}

static void lcd_run_list(HW *hw) {
    uint32_t a = hw->lcd[0x28 >> 2];
    if (getenv("VFLASH_LCDLOG")) {
        static int n;
        if (n++ < 200) {
            printf("[LCD] run list at %08X:", a);
            for (int k = 0; k < 64; k++) {
                uint32_t w = hw_read32(hw, a + 4 * k);
                printf(" %08X", w);
                if ((w & 0xFFFF0000u) == 0x14000000u && (w & 0xFF00) == 0xFD00) break;
            }
            printf("\n");
        }
    }
    hw->ge.ram = hw->ram;
    hw->ge.ram_size = RAM_SIZE;
    hw->ge.top = (int)(hw->lcd[0x90 >> 2] >> 16 & 0xFFF);
    hw->ge.surface = hw->lcd[0x98 >> 2] - (uint32_t)(hw->ge.top >> 3) * 16384;
    hw->ge.log = getenv("VFLASH_GELOG") != NULL;
    hw->ge.nodepth = getenv("VFLASH_NODEPTH") != NULL;
    hw->ge.no3d = getenv("VFLASH_NO3D") != NULL;
    /* VFLASH_GETRACE=<frame> traces 3 frames, <a>-<b> the frames a to b */
    hw->ge.trace = 0;
    if (getenv("VFLASH_GETRACE")) {
        char *e;
        unsigned long long a0 = strtoull(getenv("VFLASH_GETRACE"), &e, 10);
        unsigned long long b0 = *e == '-' ? strtoull(e + 1, NULL, 10) : a0 + 2;
        hw->ge.trace = hw->frame >= a0 && hw->frame <= b0;
    }
    if (hw->ge.trace) printf("[GET] list %08X surface %08X (row 0)\n", a, hw->ge.surface);
    if (getenv("VFLASH_SURFLOG")) {
        static uint32_t l90, l94, l98;
        if (hw->lcd[0x90 >> 2] != l90 || hw->lcd[0x94 >> 2] != l94 || hw->lcd[0x98 >> 2] != l98)
            printf("[SURF] frame %lu list %08X: +90 %08X +94 %08X +98 %08X\n", (unsigned long)hw->frame, a,
                   l90 = hw->lcd[0x90 >> 2], l94 = hw->lcd[0x94 >> 2], l98 = hw->lcd[0x98 >> 2]);
    }
    ge_capture(hw, a);
    uint32_t px0 = hw->ge.pixels;
    ge_run(&hw->ge, a);
    ge_capture_done(hw);
    if (hw->ge.log && hw->frame % 60 == 0)
        printf("[GE] frame %lu: %u sprites, %u pixels so far; tex (%d,%d) %d bpp pal (%d,%d)\n",
               (unsigned long)hw->frame, hw->ge.sprites, hw->ge.pixels, hw->ge.tex_x, hw->ge.tex_y,
               hw->ge.tex_bpp, hw->ge.pal_x, hw->ge.pal_y);
    /* The engine fills about a pixel every 10 ns; a list ends that long
     * after it starts (the kernel waits for it before its next frame). */
    hw->lcd_busy = 1;
    hw->lcd_done_us = hw_us(hw) + 20 + (hw->ge.pixels - px0) / 100;

}

static void lcd_write(HW *hw, uint32_t pa, uint32_t v) {
    uint32_t i = (pa & 0xFFF) >> 2;
    if (i == 0x14 >> 2) { hw->lcd[i] &= ~(v & 0x3F); lcd_int_check(hw); return; }
    if (hw->lcd[i] != v && i != 0x28 >> 2 && hw->lcd_log++ < 400)
        printf("[LCD] %08X = %08X (was %08X) PC=%08X\n", pa, v, hw->lcd[i], hw->cpu->r[15]);
    hw->lcd[i] = v;
    if (i == 0 && (v & 1)) { hw->ge_started++; lcd_run_list(hw); }
}

/* ---------------------------------------------------------------- MMIO histogram */

static void hist_note(HW *hw, uint32_t pa, int w) {
    uint32_t key = (pa & ~3u) | (w ? 1 : 0), pc = hw->cpu->r[15];
    uint32_t h = ((key * 2654435761u) ^ (pc * 40503u)) & 4095;
    for (int k = 0; k < 4096; k++, h = (h + 1) & 4095) {
        if (hw->hist[h].n && hw->hist[h].addr == key && hw->hist[h].pc == pc) { hw->hist[h].n++; return; }
        if (!hw->hist[h].n) { hw->hist[h].addr = key; hw->hist[h].pc = pc; hw->hist[h].n = 1; return; }
    }
}

static int hist_cmp(const void *a, const void *b) {
    const uint32_t *x = a, *y = b;
    return x[2] < y[2] ? 1 : x[2] > y[2] ? -1 : 0;
}

static void hist_print(HW *hw) {
    qsort(hw->hist, 4096, sizeof hw->hist[0], hist_cmp);
    printf("[IOHIST] frames %lu-%lu:\n", (unsigned long)hw->hist_from, (unsigned long)hw->hist_to);
    for (int i = 0; i < 400 && hw->hist[i].n; i++)
        printf("[IOHIST] %8u %s %08X PC=%08X\n", hw->hist[i].n,
               (hw->hist[i].addr & 1) ? "W" : "R", hw->hist[i].addr & ~1u, hw->hist[i].pc);
}

/* ---------------------------------------------------------------- video engine */

/* +0x00 is interrupt status (write 1 to clear), +0x04 its mask; the kernel's
 * handler (0x10090E3C) takes line 21 and treats bit 1 as the field/vblank
 * event, from which it resumes the task that asked to be woken on the next
 * frame. Everything else is a logged register file for now. */
static void ve_int_check(HW *hw) {
    int_set(hw, INT_VIDEO, (hw->ve[0] & hw->ve[1]) != 0);
}

static uint32_t ve_read(HW *hw, uint32_t pa) {
    uint32_t off = pa - VE_BASE;
    if (off < 0x800) return hw->ve[off >> 2];
    if (off >= 0x1000 && off < 0x2000) return hw->ve_mid[(off - 0x1000) >> 2];
    if (off >= 0x2000 && off < 0x3000) return hw->ve_hi[(off - 0x2000) >> 2];
    unmodelled(hw, pa, 0, 32, 0);
    return 0;
}

static void ve_write(HW *hw, uint32_t pa, uint32_t v) {
    uint32_t off = pa - VE_BASE, *r;
    if (off == 0x00) { hw->ve[0] &= ~v; ve_int_check(hw); return; }
    if (off < 0x800) r = &hw->ve[off >> 2];
    else if (off >= 0x1000 && off < 0x2000) r = &hw->ve_mid[(off - 0x1000) >> 2];
    else if (off >= 0x2000 && off < 0x3000) r = &hw->ve_hi[(off - 0x2000) >> 2];
    else { unmodelled(hw, pa, 1, 32, v); return; }
    if ((off == 0x160 || off == 0x164) && *r != v && getenv("VFLASH_VELOG"))
        printf("[VE] f%lu layer %u address %08X (was %08X) PC=%08X\n", (unsigned long)hw->frame,
               (off - 0x160) / 4, v, *r, hw->cpu->r[15]);
    if (*r != v && (hw->ve_log++ < 300 || (getenv("VFLASH_VELOG") && hw->ve_log < 3000)))
        printf("[VE] %08X = %08X (was %08X) PC=%08X\n", pa, v, *r, hw->cpu->r[15]);
    *r = v;
    if (off == 0x04) ve_int_check(hw);
}

/* Scan-out, as the kernel's video driver (0x1008EF00-0x10091200) programs it:
 *   +0x0C          screen size: (w-1) | (h-1) << 16
 *   +0x100         layer enables (bits 0-3 tile layers, 4-5 colour layers)
 *   +0x104         per tile layer, byte i bits 0-1: map width = 16 << n tiles
 *   +0x140 + 4i    tile layer i's map: 16-bit entries, one per 8x8 tile
 *   +0x150 + 4i    tile layer i's tiles: 64 bytes each, 8-bit palette indices
 *   +0x10C         background colour (BGR555), where every layer is clear
 *   +0x160         colour layer 0: 16-bit BGR555, bit 15 = transparent
 *   +0x164/168/16C colour layer 1: linear Y/U/V 4:2:0 planes
 *   +0x130/+0x138  windows of colour layers 0/1: start, end = x | y << 16
 * All tile layers share the palette at 0xB8000800, whose bit 15 is also
 * transparent. Drawn back to front over the background:
 * movie colour layer 1, RGB colour layer 0, then tile layers 0-3. The tile entries' upper
 * bits and the per-layer position registers (+0x110 + 4i) are not worked out
 * yet; entries are taken as plain tile numbers and positions as zero. */
/* Colours are xBGR1555 - red in the low bits. The kernel's BMP loader
 * (0x1001318C) builds them that way from 24-bit palette entries. */
static inline uint32_t rgb555(uint16_t c) {
    uint32_t r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

static unsigned ve_clip8(int v) { return v < 0 ? 0u : v > 255 ? 255u : (unsigned)v; }

/* Full-range JPEG YCbCr from the native DSP. Chroma is shared by each2x2
 * luma block. Exact analogue conversion/filtering awaits a hardware capture. */
static uint32_t ve_yuv(unsigned y, unsigned u, unsigned v) {
    int cb=(int)u-128, cr=(int)v-128;
    unsigned r=ve_clip8((int)y+((91881*cr+32768)>>16));
    unsigned g=ve_clip8((int)y-((22554*cb+46802*cr+32768)>>16));
    unsigned b=ve_clip8((int)y+((116130*cb+32768)>>16));
    return 0xff000000u | r<<16 | g<<8 | b;
}

static const uint8_t *ram_at(HW *hw, uint32_t pa, uint32_t len) {
    return (pa - RAM_BASE < RAM_SIZE && pa - RAM_BASE + len <= RAM_SIZE) ? hw->ram + (pa - RAM_BASE) : NULL;
}

void hw_screen_size(HW *hw, int *w, int *h) { *w = hw->scr_w; *h = hw->scr_h; }

/* The ROM is still getting a disc's game going: from power-on until the game
 * kernel starts using the graphics engine (Dingo Rallye: frame ~790; the
 * ROM runs one list while it sets the engine up, a game dozens a second), capped
 * at 30 s in case a game never does. Only the ROM's splash and the loading
 * of BOOT.BIN happen in this time, so frontends may run it unthrottled. */
int hw_booting(HW *hw) {
    return hw->cd && hw->cd->is_open && hw->ge_started < 16 && hw->frame < 1800;
}

static void ve_render(HW *hw) {
    uint32_t sz = hw->ve[0x0C >> 2];
    int W = (sz & 0x3FF) + 1, H = (sz >> 16 & 0x3FF) + 1;
    if (!sz || W > VFLASH_FB_MAX_W || H > VFLASH_FB_MAX_H) { W = 320; H = 240; }
    hw->scr_w = W; hw->scr_h = H;
    uint32_t en = hw->ve[0x100 >> 2];
    uint32_t *fb = hw->fb;
    uint32_t bg = rgb555((uint16_t)hw->ve[0x10C >> 2]);
    for (int i = 0; i < W * H; i++) fb[i] = bg;

    /* The RGB surface masks the movie through its bit-15 transparency.
     * Spider-Man keeps layer 1 enabled after movie stop and paints opaque
     * menus over it; active movie windows in Multisports/Scooby are clear
     * in RGB. Drawing video last exposes stale planes over those menus. */
    for (int j = 1; j >= 0; j--) {
        if (!(en >> (4 + j) & 1)) continue;
        uint32_t st = hw->ve[(0x130 + 8 * j) >> 2], end = hw->ve[(0x134 + 8 * j) >> 2];
        int x0 = st & 0x3FF, y0 = st >> 16 & 0x3FF;
        int w = (end & 0x3FF) - x0 + 1, h = (end >> 16 & 0x3FF) - y0 + 1;
        if(w <= 0 || h <= 0) continue;
        /* Multisports ARM109F6114 sets all three plane addresses together;
         * the DSP DMA fills them with independently verified planar4:2:0.
         * This layer was previously misread as packed RGB, doubling pixels. */
        if(j==1) {
            int cw=(w+1)/2, ch=(h+1)/2;
            const uint8_t *yp=ram_at(hw,hw->ve[0x164>>2],(uint32_t)(w*h));
            const uint8_t *up=ram_at(hw,hw->ve[0x168>>2],(uint32_t)(cw*ch));
            const uint8_t *vp=ram_at(hw,hw->ve[0x16c>>2],(uint32_t)(cw*ch));
            if(!yp || !up || !vp) continue;
            for(int y=0;y<h && y0+y<H;y++)
                for(int x=0;x<w && x0+x<W;x++) {
                    unsigned c=(unsigned)((y/2)*cw+x/2);
                    fb[(y0+y)*W+x0+x]=ve_yuv(yp[y*w+x],up[c],vp[c]);
                }
            continue;
        }
        /* +0x108 bit 2 + j: the layer is in the graphics engine's layout, 8x8
         * tiles on a 1024-pixel-wide surface (see ge.c), not a linear w x h. */
        int tiled = hw->ve[0x108 >> 2] >> (2 + j) & 1;
        const uint8_t *src = ram_at(hw, hw->ve[(0x160 >> 2) + j],
                                    tiled ? (uint32_t)(((h + 7) >> 3) * 16384) : (uint32_t)(w * h * 2));
        if (!src || w <= 0 || h <= 0) continue;
        for (int y = 0; y < h && y0 + y < H; y++)
            for (int x = 0; x < w && x0 + x < W; x++) {
                uint32_t o = tiled ? (uint32_t)((y >> 3) * 16384 + (x >> 5) * 512 + (y & 7) * 64 + (x & 31) * 2)
                                   : (uint32_t)(y * w + x) * 2;
                uint16_t c = (uint16_t)(src[o] | src[o + 1] << 8);
                if (!(c & 0x8000)) fb[(y0 + y) * W + x0 + x] = rgb555(c);
            }
    }

    const uint8_t *pal = hw->sram;   /* 0xB8000800 */
    uint32_t fmt = hw->ve[0x104 >> 2];
    for (int i = 0; i < 4; i++) {
        if (!(en >> i & 1)) continue;
        int mapw = 16 << (fmt >> (8 * i) & 3), maph = 64;
        uint32_t mbase = hw->ve[(0x140 >> 2) + i], tbase = hw->ve[(0x150 >> 2) + i];
        const uint8_t *map = ram_at(hw, mbase, (uint32_t)(mapw * maph * 2));
        if (!map || tbase - RAM_BASE >= RAM_SIZE) continue;
        /* +0x110 + 4i: the layer's scroll, signed x | y << 16 - the screen
         * shows the layer from (x, y). The menu cursor moves with it. */
        uint32_t sc = hw->ve[(0x110 >> 2) + i];
        int sx = (int16_t)(sc & 0xFFFF), sy = (int16_t)(sc >> 16);
        int lw = mapw * 8, lh = maph * 8;
        for (int y = 0; y < H; y++) {
            int ly = ((y + sy) % lh + lh) % lh;
            for (int x = 0; x < W; x++) {
                int lx = ((x + sx) % lw + lw) % lw;
                const uint8_t *e8 = map + ((ly >> 3) * mapw + (lx >> 3)) * 2;
                uint32_t e = e8[0] | e8[1] << 8;
                const uint8_t *t = ram_at(hw, tbase + e * 64, 64);
                if (!t) continue;
                uint8_t v = t[(ly & 7) * 8 + (lx & 7)];
                if (!v) continue;   /* palette index 0 is transparent */
                uint16_t c = (uint16_t)(pal[v * 2] | pal[v * 2 + 1] << 8);
                if (!(c & 0x8000)) fb[y * W + x] = rgb555(c);
            }
        }
    }
}

static void lcd_int_check(HW *hw);

static void lcd_poll(HW *hw) {
    if (hw->lcd_busy && hw_us(hw) >= hw->lcd_done_us) {
        hw->lcd_busy = 0;
        hw->lcd[0x14 >> 2] = 0xFD08;
        lcd_int_check(hw);
    }
}

static void ve_vblank(HW *hw) {
    ve_render(hw);
    hw->vblanks++;
    hw->ve[0] |= 2;
    ve_int_check(hw);
}

/* ---------------------------------------------------------------- physical bus */

static uint32_t mmio_rd(HW *hw, uint32_t pa, int size) {
    if (hw->hist_on && (!hw->hist_page || pa >> 16 == hw->hist_page)) hist_note(hw, pa, 0);
    switch (pa >> 16) {
    case 0x9000: case 0x900D: return gpio_read(hw, pa);
    case 0x9002: case 0x9003: return uart_read(hw, pa);
    case 0x9008: return hw->ssp[(pa & 0x3F) >> 2];
    case 0x9001: case 0x900C: return timer_read(hw, pa);
    case 0x9009: return rtc_read(hw, pa);
    case 0x900A: return misc_read(hw, pa);
    case 0x900B: return pmu_read(hw, pa);
    case 0xDC00: return ic_read(hw, pa);
    case 0xC400: return ser_read(hw, pa);
    case 0xA100: return spi_read(hw, pa);
    case 0xA800: return lcd_read(hw, pa);
    case 0xB800: return ve_read(hw, pa);
    case 0xA000: return xflash_read(hw, pa);
    case 0xA400: return mathu_read(hw, pa);
    case 0xBC00: return dmac_read(hw, pa);
    case 0xC000: return zevio_dsp_dma_read(&hw->zsp, pa & 0xFFFF);
    case 0xB000: return midi_read(&hw->midi, pa & 0xFFFF);
    }
    unmodelled(hw, pa, 0, size, 0);
    return 0;
}

static void mmio_wr(HW *hw, uint32_t pa, uint32_t v, int size) {
    if (hw->hist_on && (!hw->hist_page || pa >> 16 == hw->hist_page)) hist_note(hw, pa, 1);
    switch (pa >> 16) {
    case 0x8FFF: return;  /* SDRAM controller: timings, nothing to model */
    case 0x9002: case 0x9003: uart_write(hw, pa, v); return;
    case 0x9008: hw->ssp[(pa & 0x3F) >> 2] = v; return;
    case 0x9000: case 0x900D: gpio_write(hw, pa, v); return;
    case 0x9001: case 0x900C: timer_write(hw, pa, v); return;
    case 0x9009: rtc_write(hw, pa, v); return;
    case 0x900A: misc_write(hw, pa, v); return;
    case 0x900B: pmu_write(hw, pa, v); return;
    case 0xDC00: ic_write(hw, pa, v); return;
    case 0xC400: ser_write(hw, pa, v); return;
    case 0xA100: spi_write(hw, pa, v); return;
    case 0xA800: lcd_write(hw, pa, v); return;
    case 0xB800: ve_write(hw, pa, v); return;
    case 0xA400: mathu_write(hw, pa, v); return;
    case 0xBC00: dmac_write(hw, pa, v); return;
    case 0xC000: zevio_dsp_dma_write(&hw->zsp, pa & 0xFFFF, v); return;
    case 0xB000: midi_write(&hw->midi,pa & 0xFFFF,v,hw->cpu->r[15]-(hw->cpu->thumb?4:8)); return;
    }
    unmodelled(hw, pa, 1, size, v);
}

static inline uint8_t *mem_ptr(HW *hw, uint32_t pa, int wr) {
    if (pa - RAM_BASE < RAM_SIZE) return hw->ram + (pa - RAM_BASE);
    if (pa - SRAM_BASE < SRAM_SIZE) return hw->sram + (pa - SRAM_BASE);
    if (pa - MBOX_BASE < MBOX_SIZE) return hw->mbox + (pa - MBOX_BASE);
    if (!wr && pa < hw->rom_size) return (uint8_t *)hw->rom + pa;
    return NULL;
}

static uint32_t phys_rd32(HW *hw, uint32_t pa) {
    uint8_t *p = mem_ptr(hw, pa, 0);
    if (p) { uint32_t v; memcpy(&v, p, 4); return v; }
    return mmio_rd(hw, pa, 32);
}

/* ---------------------------------------------------------------- MMU */

/* Two-level ARMv5 page-table walk. Sets *fault on a translation fault;
 * permissions and domains are not checked yet. *small is set for a 1 KB
 * tiny page, which the 4 KB TLB cannot hold. */
static void tlb_flush(HW *hw);

/* A page first seen holding a table may still have write pointers cached. */
static uint32_t pt_read(HW *hw, uint32_t pa) {
    if (pa - RAM_BASE < RAM_SIZE && !hw->pt_page[(pa - RAM_BASE) >> 12]) {
        hw->pt_page[(pa - RAM_BASE) >> 12] = 1;
        tlb_flush(hw);
    }
    return phys_rd32(hw, pa);
}

static uint32_t walk(HW *hw, uint32_t va, int *fault, int *small) {
    CP15 *cp = &hw->cpu->cp15;
    *fault = 0; *small = 0;
    if (!cp->mmu_enabled) return va;
    uint32_t l1 = pt_read(hw, (cp->ttb & 0xFFFFC000u) | ((va >> 20) << 2));
    uint32_t l2 = 0;
    switch (l1 & 3) {
    case 2: return (l1 & 0xFFF00000u) | (va & 0x000FFFFFu);
    case 1: l2 = pt_read(hw, (l1 & 0xFFFFFC00u) | (((va >> 12) & 0xFF) << 2)); break;
    case 3: l2 = pt_read(hw, (l1 & 0xFFFFF000u) | (((va >> 10) & 0x3FF) << 2)); break;
    }
    switch ((l1 & 3) ? (l2 & 3) : 0) {
    case 1: return (l2 & 0xFFFF0000u) | (va & 0xFFFFu);
    case 2: return (l2 & 0xFFFFF000u) | (va & 0xFFFu);
    case 3: *small = 1; return (l2 & 0xFFFFFC00u) | (va & 0x3FFu);
    }
    *fault = 1;
    if (hw->fault_log < 20) {
        hw->fault_log++;
        printf("[HW] translation fault VA=%08X PC=%08X\n", va, hw->cpu->r[15]);
    }
    return 0;
}

/* Also moves CP15's generation on, which drops the CPU's fetch page. */
static void tlb_flush(HW *hw) {
    memset(hw->tlb, 0, sizeof hw->tlb);
    hw->tlb_gen = ++hw->cpu->cp15.tlb_gen;
}

/* A whole 4 KB physical page of plain memory, or NULL. The video engine's
 * palette RAM shares its page with registers, so it stays on the slow path. */
static uint8_t *page_ptr(HW *hw, uint32_t pa, int wr) {
    if (pa - RAM_BASE < RAM_SIZE) return hw->ram + (pa - RAM_BASE);
    if (pa - MBOX_BASE < MBOX_SIZE) return hw->mbox + (pa - MBOX_BASE);
    /* not pa + 0x1000 <= rom_size: for the top page that wraps to 0 */
    if (!wr && pa < hw->rom_size && hw->rom_size - pa >= 0x1000) return (uint8_t *)hw->rom + pa;
    return NULL;
}

/* The entry for va's page, filled by a walk on a miss; NULL on a fault (or
 * a tiny page, with the address in *pa for the caller to use once). The
 * debugging watches (VFLASH_WATCH / RWATCH) keep host pointers out, so
 * every access still reaches them; page-table pages get no write pointer,
 * so a store to one comes through hw_write* and flushes. */
static uint32_t rwatch_len;
static inline int tlb_get(HW *hw, uint32_t va, uint32_t *pa) {
    if (hw->tlb_gen != hw->cpu->cp15.tlb_gen) tlb_flush(hw);
    uint32_t i = va >> 12 & (TLB_SIZE - 1), tag = (va & ~0xFFFu) | 1;
    if (hw->tlb[i].tag == tag) { *pa = hw->tlb[i].pa | (va & 0xFFF); return 1; }
    int f, small;
    uint32_t p = walk(hw, va, &f, &small);
    if (f) return 0;
    *pa = p;
    if (small) return 1;
    hw->tlb[i].tag = tag;
    hw->tlb[i].pa = p & ~0xFFFu;
    uint32_t pg = p & ~0xFFFu;
    hw->tlb[i].rd = rwatch_len ? NULL : page_ptr(hw, pg, 0);
    hw->tlb[i].wr = hw->watch_len ? NULL : page_ptr(hw, pg, 1);
    if (hw->tlb[i].wr && pg - RAM_BASE < RAM_SIZE && hw->pt_page[(pg - RAM_BASE) >> 12])
        hw->tlb[i].wr = NULL;
    return 1;
}

/* The CPU's instruction-fetch page (arm9.h mem_page). */
static const uint8_t *hw_mem_page(void *ctx, uint32_t va) {
    HW *hw = ctx; uint32_t pa;
    if (!tlb_get(hw, va, &pa)) return NULL;
    uint32_t i = va >> 12 & (TLB_SIZE - 1);
    return hw->tlb[i].tag == ((va & ~0xFFFu) | 1) ? hw->tlb[i].rd : NULL;
}

/* Slow-path stores check this: a CPU write into memory a walk has read
 * (a page table) drops every cached translation. */
static inline void pt_store(HW *hw, uint32_t pa) {
    if (pa - RAM_BASE < RAM_SIZE && hw->pt_page[(pa - RAM_BASE) >> 12]) tlb_flush(hw);
}

/* The core masks 32-bit addresses and rotates unaligned loads itself; the bus
 * moves naturally aligned units. */
/* VFLASH_WATCH=<addr>,<len>: report writes to a physical range (first 200). */
static void watch_hit(HW *hw, uint32_t pa, uint32_t v, int size) {
    static int n;
    if (n++ < 200)
        printf("[WATCH] write%d %08X = %08X PC=%08X LR=%08X frame %lu\n", size, pa, v,
               hw->cpu->r[15], hw->cpu->r[14], (unsigned long)hw->frame);
}

/* VFLASH_RWATCH=<addr>,<len>[,<from frame>]: report each distinct PC that
 * reads a physical range (first 64 PCs). */
static uint32_t rwatch_lo;
static uint64_t rwatch_from;
static void rwatch_hit(HW *hw, uint32_t pa, int size) {
    static uint32_t seen[64];
    static int n;
    if (hw->frame < rwatch_from) return;
    uint32_t pc = hw->cpu->r[15];
    for (int i = 0; i < n; i++) if (seen[i] == pc) return;
    if (n >= 64) return;
    seen[n++] = pc;
    printf("[RWATCH] read%d %08X PC=%08X LR=%08X frame %lu\n", size, pa, pc, hw->cpu->r[14],
           (unsigned long)hw->frame);
}
#define RWATCH(pa, size) \
    if (rwatch_len && (pa) - rwatch_lo < rwatch_len) rwatch_hit(hw, pa, size)

#define TLB_FAST(va, ptr) \
    (hw->tlb_gen == hw->cpu->cp15.tlb_gen && \
     hw->tlb[(va) >> 12 & (TLB_SIZE - 1)].tag == (((va) & ~0xFFFu) | 1) && \
     hw->tlb[(va) >> 12 & (TLB_SIZE - 1)].ptr)
#define TLB_HOST(va, ptr) (hw->tlb[(va) >> 12 & (TLB_SIZE - 1)].ptr + ((va) & 0xFFF))

uint32_t hw_read32(void *ctx, uint32_t va) {
    HW *hw = ctx; uint32_t pa, v;
    va &= ~3u;
    if (TLB_FAST(va, rd)) { memcpy(&v, TLB_HOST(va, rd), 4); return v; }
    if (!tlb_get(hw, va, &pa)) return 0;
    RWATCH(pa, 32);
    return phys_rd32(hw, pa);
}

uint16_t hw_read16(void *ctx, uint32_t va) {
    HW *hw = ctx; uint32_t pa; uint16_t v;
    va &= ~1u;
    if (TLB_FAST(va, rd)) { memcpy(&v, TLB_HOST(va, rd), 2); return v; }
    if (!tlb_get(hw, va, &pa)) return 0;
    RWATCH(pa, 16);
    uint8_t *p = mem_ptr(hw, pa, 0);
    if (p) { memcpy(&v, p, 2); return v; }
    return (uint16_t)(mmio_rd(hw, pa & ~3u, 16) >> ((pa & 2) * 8));
}

uint8_t hw_read8(void *ctx, uint32_t va) {
    HW *hw = ctx; uint32_t pa;
    if (TLB_FAST(va, rd)) return *TLB_HOST(va, rd);
    if (!tlb_get(hw, va, &pa)) return 0;
    RWATCH(pa, 8);
    uint8_t *p = mem_ptr(hw, pa, 0);
    if (p) return *p;
    return (uint8_t)(mmio_rd(hw, pa & ~3u, 8) >> ((pa & 3) * 8));
}

void hw_write32(void *ctx, uint32_t va, uint32_t v) {
    HW *hw = ctx; uint32_t pa;
    va &= ~3u;
    if (TLB_FAST(va, wr)) { memcpy(TLB_HOST(va, wr), &v, 4); return; }
    if (!tlb_get(hw, va, &pa)) return;
    if (hw->watch_len && pa - hw->watch_lo < hw->watch_len) watch_hit(hw, pa, v, 32);
    uint8_t *p = mem_ptr(hw, pa, 1);
    if (p) { memcpy(p, &v, 4); pt_store(hw, pa); return; }
    mmio_wr(hw, pa, v, 32);
}

void hw_write16(void *ctx, uint32_t va, uint16_t v) {
    HW *hw = ctx; uint32_t pa;
    va &= ~1u;
    if (TLB_FAST(va, wr)) { memcpy(TLB_HOST(va, wr), &v, 2); return; }
    if (!tlb_get(hw, va, &pa)) return;
    if (hw->watch_len && pa - hw->watch_lo < hw->watch_len) watch_hit(hw, pa, v, 16);
    uint8_t *p = mem_ptr(hw, pa, 1);
    if (p) { memcpy(p, &v, 2); pt_store(hw, pa); return; }
    mmio_wr(hw, pa & ~3u, (uint32_t)v * 0x00010001u, 16);
}

void hw_write8(void *ctx, uint32_t va, uint8_t v) {
    HW *hw = ctx; uint32_t pa;
    if (TLB_FAST(va, wr)) { *TLB_HOST(va, wr) = v; return; }
    if (!tlb_get(hw, va, &pa)) return;
    if (hw->watch_len && pa - hw->watch_lo < hw->watch_len) watch_hit(hw, pa, v, 8);
    uint8_t *p = mem_ptr(hw, pa, 1);
    if (p) { *p = v; pt_store(hw, pa); return; }
    mmio_wr(hw, pa & ~3u, (uint32_t)v * 0x01010101u, 8);
}

/* ---------------------------------------------------------------- machine */

HW *hw_create(ARM9 *cpu, const uint8_t *rom, uint32_t rom_size,
              struct CDROM *cd, uint32_t *framebuf) {
    HW *hw = calloc(1, sizeof(HW));
    hw->rtc_boot = getenv("VFLASH_RTC") ? (time_t)strtoll(getenv("VFLASH_RTC"), NULL, 10) : time(NULL);
    hw->cpu = cpu;
    hw->rom = rom;
    hw->rom_size = rom_size;
    hw->ram = calloc(1, RAM_SIZE);
    zevio_dsp_init(&hw->zsp, hw->ram, RAM_BASE, RAM_SIZE);
    hw->cd = cd;
    hw->fb = framebuf;
    hw->scr_w = 320; hw->scr_h = 240;

    devices_reset(hw);
    hw->pmu.clocks = hw->pmu.clocks_load = 0x141002;
    pmu_set_clocks(hw);

    cpu->mem_ctx     = hw;
    cpu->mem_read32  = hw_read32;
    cpu->mem_read16  = hw_read16;
    cpu->mem_read8   = hw_read8;
    cpu->mem_write32 = hw_write32;
    cpu->mem_write16 = hw_write16;
    cpu->mem_write8  = hw_write8;
    cpu->mem_page    = hw_mem_page;
    arm9_reset(cpu);
    memset(framebuf, 0, 320 * 240 * 4);
    {
        if (getenv("VFLASH_TRACEFROM")) hw->trace_from = strtoull(getenv("VFLASH_TRACEFROM"), NULL, 0);
        if (getenv("VFLASH_WATCH")) {
            char *e;
            hw->watch_lo = (uint32_t)strtoul(getenv("VFLASH_WATCH"), &e, 16);
            hw->watch_len = *e == 0x2C ? (uint32_t)strtoul(e + 1, NULL, 0) : 4;
        }
        if (getenv("VFLASH_PUTS")) {
            char *e;
            hw->puts_pc = (uint32_t)strtoul(getenv("VFLASH_PUTS"), &e, 16);
            hw->puts_reg = *e == 0x2C ? atoi(e + 1) & 15 : 0;   /* ",<reg>" of the string */
        }
        if (getenv("VFLASH_RWATCH")) {
            unsigned long lo = 0, len = 0, from = 0;
            sscanf(getenv("VFLASH_RWATCH"), "%lx,%lx,%lu", &lo, &len, &from);
            rwatch_lo = (uint32_t)lo; rwatch_len = (uint32_t)len; rwatch_from = from;
        }
        const char *t = getenv("VFLASH_TRACEPC");
        if (t) {
            char *e = (char *)t;
            while (hw->ntrace < 16) {
                hw->trace_pc[hw->ntrace++] = (uint32_t)strtoul(e, &e, 16);
                if (*e != ':') break;
                e++;
            }
            const char *comma = strchr(t, ',');
            hw->trace_left = comma ? atoi(comma + 1) : 20;
        }
    }
    printf("[HW] Booting the ROM at 0\n");
    return hw;
}

void hw_destroy(HW *hw) {
    if (!hw) return;
    free(hw->ram);
    free(hw);
}

/* Turn CPU cycles into timer-clock ticks, carrying the remainder. */
static void timers_run(HW *hw, uint64_t cycles) {
    zevio_dsp_run(&hw->zsp, cycles);
    int_set(hw,17,(hw->zsp.control[0x118/4]&1) && (hw->zsp.control[0x104/4]&1));
    int_set(hw,18,(hw->zsp.control[0x118/4]&2) && (hw->zsp.control[0x110/4]&1));
    hw->audio_acc += cycles * 44100;
    while(hw->audio_acc >= hw->cpu_hz) {
        uint64_t frames=hw->audio_acc/hw->cpu_hz;
        if(frames>256) frames=256;
        int16_t samples[512];
        midi_render(&hw->midi,samples,(unsigned)frames);
        if(hw->audio_push) hw->audio_push(hw->audio_ctx,samples,(uint32_t)frames*2);
        hw->audio_acc-=frames*hw->cpu_hz;
    }
    hw->timer_acc += cycles * hw->timer_hz;
    uint64_t ticks = hw->timer_acc / hw->cpu_hz;
    hw->timer_acc -= ticks * hw->cpu_hz;
    while (ticks) {
        int n = ticks > 0x10000 ? 0x10000 : (int)ticks;
        timer_advance(hw, 0, n);
        timer_advance(hw, 1, n);
        ticks -= n;
    }
}

static void check_irq(HW *hw) {
    ARM9 *c = hw->cpu;
    if (hw->ic.line[1] && !(c->cpsr & ARM9_FLAG_F)) { arm9_fiq(c); return; }
    if (hw->ic.line[0] && !(c->cpsr & ARM9_FLAG_I)) {
        int l = ic_current(hw, 0);
        if (l >= 0) hw->irq_by_line[l]++;
        arm9_irq(c); hw->irqs++;
    }
}

/* ---------------------------------------------------------------- debugger */

static int is_traced(HW *hw, uint32_t pc) {
    for (int i = 0; i < hw->ntrace; i++)
        if (hw->trace_pc[i] == pc) return 1;
    return 0;
}

static int at_bp(HW *hw, uint32_t pc) {
    for (int i = 0; i < 16; i++)
        if (hw->bp[i] == pc && hw->bp[i]) return 1;
    return 0;
}

void hw_bp_set(HW *hw, uint32_t addr) {
    for (int i = 0; i < 16; i++) if (hw->bp[i] == addr) return;
    for (int i = 0; i < 16; i++)
        if (!hw->bp[i]) { hw->bp[i] = addr; hw->nbp++; return; }
    fprintf(stderr, "[DBG] No free breakpoint slots\n");
}

void hw_bp_clear(HW *hw, uint32_t addr) {
    for (int i = 0; i < 16; i++)
        if (hw->bp[i] == addr && addr) { hw->bp[i] = 0; hw->nbp--; }
}

void hw_bp_clear_all(HW *hw) { memset(hw->bp, 0, sizeof hw->bp); hw->nbp = 0; }
int  hw_bp_hit(HW *hw) { return hw->bp_hit; }

uint32_t hw_bp_list(HW *hw, uint32_t *out, int maxn) {
    int n = 0;
    for (int i = 0; i < 16 && n < maxn; i++)
        if (hw->bp[i]) out[n++] = hw->bp[i];
    return (uint32_t)n;
}

int hw_step(HW *hw) {
    ARM9 *c = hw->cpu;
    uint64_t before = c->cycles;
    arm9_step(c);
    check_irq(hw);
    timers_run(hw, c->cycles - before);
    cd_tick(hw);
    hw->bp_hit = hw->nbp && at_bp(hw, c->r[15]);
    return (int)(c->cycles - before);
}

/* ---------------------------------------------------------------- frame */

/* VFLASH_PROFILE=<from>,<to>: sample the PC every 256 cycles over a frame
 * range and print the 40 (VFLASH_PROFILE_N) hottest addresses at the end of it. */
static void prof_note(HW *hw, uint32_t pc) {
    uint32_t h = (pc * 2654435761u) >> 20;
    for (int k = 0; k < 4096; k++, h = (h + 1) & 4095) {
        if (hw->prof[h].n && hw->prof[h].pc == pc) { hw->prof[h].n++; return; }
        if (!hw->prof[h].n) { hw->prof[h].pc = pc; hw->prof[h].n = 1; return; }
    }
}

static int prof_cmp(const void *a, const void *b) {
    const uint32_t *x = a, *y = b;
    return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
}

static void prof_frame(HW *hw) {
    const char *p = getenv("VFLASH_PROFILE");
    unsigned long a = 0, b = 0;
    if (!p || sscanf(p, "%lu,%lu", &a, &b) != 2) return;
    hw->prof_on = hw->frame >= a && hw->frame < b;
    if (hw->frame == b) {
        qsort(hw->prof, 4096, sizeof hw->prof[0], prof_cmp);
        int top = getenv("VFLASH_PROFILE_N") ? atoi(getenv("VFLASH_PROFILE_N")) : 40;
        for (int i = 0; i < top && i < 4096 && hw->prof[i].n; i++)
            printf("[PROF] %8u %08X\n", hw->prof[i].n, hw->prof[i].pc);
    }
}

/* ---------------------------------------------------------------- idle skip */

/* µMORE's idle task: the lowest-priority task spins on a counter nobody
 * reads until an interrupt makes another task ready. Both kernels have one -
 * the ROM's (-O0, counters on the stack, 0x10010994) and the game kernels'
 * (register only, 0x109E4BBC in Dingo Rallye). The loops are position
 * independent, so they are recognised by their words wherever they sit. */
static const uint32_t idle_rom[] = {
    0xE3A03000, 0xE5CD3007, 0xE3A03000, 0xE58D3000, 0xE59D3000, 0xE35300FE,
    0x9A000000, 0xEAFFFFF7, 0xE5DD3007, 0xE2833001, 0xE5CD3007, 0xE59D3000,
    0xE2833001, 0xE58D3000, 0xEAFFFFF4 };
static const uint32_t idle_game[] = {
    0xE3A03000, 0xE2833001, 0xE35300FE, 0x9AFFFFFC, 0xEAFFFFFA };
static const struct { const uint32_t *w; int n; } idle_sig[] = {
    { idle_rom, 15 }, { idle_game, 5 } };

static int in_idle_loop(HW *hw, uint32_t pc) {
    uint32_t w = hw_read32(hw, pc);
    for (unsigned k = 0; k < sizeof idle_sig / sizeof idle_sig[0]; k++)
        for (int i = 0; i < idle_sig[k].n; i++) {
            if (idle_sig[k].w[i] != w) continue;
            uint32_t base = pc - 4u * (uint32_t)i;
            int j = 0;
            while (j < idle_sig[k].n && hw_read32(hw, base + 4u * (uint32_t)j) == idle_sig[k].w[j]) j++;
            if (j == idle_sig[k].n) return 1;
        }
    return 0;
}

/* A slice of the idle loop can be passed over rather than run: interrupt
 * sources only change between slices (timers, CD and display run there) or
 * on a device access, which the loop never makes, so with nothing pending
 * now the CPU would spin through the whole slice. Only its private counters
 * differ. VFLASH_NOIDLE=1 runs it instruction by instruction instead. */
static int idle_skippable(HW *hw) {
    ARM9 *c = hw->cpu;
    static int off = -1;
    if (off < 0) off = getenv("VFLASH_NOIDLE") != NULL;
    if (off || hw->nbp || hw->trace_left || hw->puts_pc || (c->cpsr & ARM9_FLAG_T)) return 0;
    if (hw->ic.line[1] && !(c->cpsr & ARM9_FLAG_F)) return 0;
    if (hw->ic.line[0] && !(c->cpsr & ARM9_FLAG_I)) return 0;
    return in_idle_loop(hw, c->r[15]);
}

/* ---------------------------------------------------------------- run control */

/* VFLASH_EXIT=<frame> ends a (headless) run cleanly after that frame. */
static void exit_hook(HW *hw) {
    const char *cl = getenv("VFLASH_CDLOG");
    cdsp_log = cl && hw->frame >= strtoull(cl, NULL, 10);
    const char *s = getenv("VFLASH_EXIT");
    if (s && hw->frame >= strtoull(s, NULL, 10)) {
        printf("[HW] exit at frame %lu\n", (unsigned long)hw->frame);
        fflush(stdout);
        exit(0);
    }
}


void hw_run_frame(HW *hw) {
    ARM9 *c = hw->cpu;
    exit_hook(hw);
    pad_frame(hw);
    prof_frame(hw);
    uint64_t end = c->cycles + hw->cpu_hz / 60;
    int first = 1;
    hw->bp_hit = 0;
    while (c->cycles < end) {
        uint64_t start = c->cycles, stop = start + 256;
        if (idle_skippable(hw)) { c->cycles = stop; hw->idle_slices++; first = 0; }
        while (c->cycles < stop) {
            /* A breakpoint ends the frame early; the one resumed from is not
             * hit again on its first instruction. */
            if (hw->nbp && !first && at_bp(hw, c->r[15])) {
                hw->bp_hit = 1;
                timers_run(hw, c->cycles - start);
                return;
            }
            first = 0;
            if (hw->puts_pc && c->r[15] == hw->puts_pc) {
                char buf[256]; int k;
                int sr = hw->puts_reg;
                for (k = 0; k < 255 && (buf[k] = (char)hw_read8(hw, c->r[sr] + k)); k++)
                    if (buf[k] == 10 || buf[k] == 13) buf[k] = 32;
                buf[k] = 0;
                printf("[PUTS] f%lu %s  (next args %d %d %d, LR=%08X)\n", (unsigned long)hw->frame, buf,
                       (int)c->r[(sr + 1) & 15],
                       (int)c->r[(sr + 2) & 15], (int)c->r[(sr + 3) & 15], c->r[14]);
            }
            if (hw->trace_left && is_traced(hw, c->r[15]) && hw->frame >= hw->trace_from) {
                hw->trace_left--;
                printf("[TRACE] %08X R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X R6=%08X "
                       "R7=%08X R8=%08X R12=%08X SP=%08X LR=%08X CPSR=%08X t=%lluus\n", c->r[15],
                       c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6],
                       c->r[7], c->r[8], c->r[12], c->r[13], c->r[14], c->cpsr,
                       (unsigned long long)hw_us(hw));
                if (getenv("VFLASH_TRACEMEM")) {   /* and 8 words at R<n>, n = its value */
                    int rn = atoi(getenv("VFLASH_TRACEMEM")) & 15;
                    printf("[TRACE]   [R%d]", rn);
                    for (int k = 0; k < 8; k++) printf(" %08X", hw_read32(hw, c->r[rn] + 4 * k));
                    printf("\n");
                }
            }
            arm9_step(c);
            if (hw->ic.line[0] | hw->ic.line[1]) check_irq(hw);
        }
        timers_run(hw, c->cycles - start);
        cd_tick(hw);
        lcd_poll(hw);
        if (hw->prof_on) prof_note(hw, c->r[15]);
    }
    ve_vblank(hw);
    hw->frame++;

    {
        const char *h = getenv("VFLASH_IOHIST");
        if (h && !hw->hist_to) {
            unsigned long a = 0, b = 0;
            unsigned long pg = 0;
            sscanf(h, "%lu,%lu,%lx", &a, &b, &pg);
            hw->hist_page = (uint32_t)pg;
            hw->hist_from = a; hw->hist_to = b ? b : a + 60;
        }
        if (h) {
            hw->hist_on = hw->frame >= hw->hist_from && hw->frame < hw->hist_to;
            if (hw->frame == hw->hist_to) hist_print(hw);
        }
    }

    /* VFLASH_RAMDUMP=<path> [VFLASH_RAMDUMP_FRAME=N, default 600]: SDRAM
     * (base 0x10000000) and SRAM (base 0xB8000000) for offline disassembly. */
    const char *dump = getenv("VFLASH_RAMDUMP");
    if (dump) {
        const char *fr = getenv("VFLASH_RAMDUMP_FRAME");
        if (hw->frame == (uint64_t)(fr ? atoll(fr) : 600)) {
            char path[1024];
            FILE *f = fopen(dump, "wb");
            if (f) { fwrite(hw->ram, 1, RAM_SIZE, f); fclose(f); }
            snprintf(path, sizeof path, "%s.sram", dump);
            if ((f = fopen(path, "wb"))) { fwrite(hw->sram, 1, SRAM_SIZE, f); fclose(f); }
            printf("[HW] dumped RAM to %s at frame %lu PC=%08X LR=%08X SP=%08X CPSR=%08X\n",
                   dump, (unsigned long)hw->frame, c->r[15], c->r[14], c->r[13], c->cpsr);
            for (int i = 0; i < 0x200; i++)
                if (hw->ve[i]) printf("[HW]   VE +%03X = %08X\n", i * 4, hw->ve[i]);
            for (int i = 0; i < 0x40; i++)
                if (hw->lcd[i]) printf("[HW]   LCD +%03X = %08X\n", i * 4, hw->lcd[i]);
        }
    }

    if (hw->frame % 60 == 0) {
        printf("[HW] frame %lu PC=%08X CPSR=%08X irqs=%lu acks=%lu ic24=%lu IC st=%08X m=%08X/%08X lim=%d\n",
               (unsigned long)hw->frame, c->r[15], c->cpsr, (unsigned long)hw->irqs,
               (unsigned long)hw->acks, (unsigned long)hw->ic24,
               hw->ic.status, hw->ic.mask[0], hw->ic.mask[1], hw->ic.limit[0]);
        printf("[HW]   cd: %u sectors, last lba %u; irqs by line:", hw->cd_sectors, hw->cd_last_lba);
        for (int l = 0; l < 32; l++)
            if (hw->irq_by_line[l]) printf(" %d:%u", l, hw->irq_by_line[l]);
        printf("\n");
        memset(hw->irq_by_line, 0, sizeof hw->irq_by_line);
        printf("[HW]   idle: %u%% of slices skipped\n", (unsigned)(hw->idle_slices * 256ull * 100 / (hw->cpu_hz ? hw->cpu_hz : 1)));
        hw->idle_slices = 0;
        printf("[HW]   ge: %u lists, %u sprites, %u fills, %u model calls, %u triangles, %u pixels total\n",
               hw->ge.lists, hw->ge.sprites, hw->ge.fills, hw->ge.calls, hw->ge.tris, hw->ge.pixels);
        hw->ge.lists = hw->ge.sprites = hw->ge.fills = hw->ge.calls = hw->ge.tris = 0;
        if (hw->ge.log) {   /* unknown GE opcodes met since the last report, op:count */
            static uint32_t seen[256];
            int any = 0;
            for (int i = 0; i < 256; i++)
                if (hw->ge.unknown[i] != seen[i]) {
                    if (!any++) printf("[HW]   ge unknown:");
                    printf(" %02X:%u", i, hw->ge.unknown[i] - seen[i]);
                    seen[i] = hw->ge.unknown[i];
                }
            if (any) printf("\n");
        }
        for (int i = 0; i < 2; i++)
            printf("[HW]   timer%d: v=%04X div=%04X ctl=%02X | v=%04X div=%04X ctl=%02X "
                   "compl=%04X,%04X st=%02X mask=%02X\n", i,
                   hw->tp[i].t[0].value, hw->tp[i].t[0].divider, hw->tp[i].t[0].control,
                   hw->tp[i].t[1].value, hw->tp[i].t[1].divider, hw->tp[i].t[1].control,
                   hw->tp[i].compl[0], hw->tp[i].compl[1],
                   hw->tp[i].int_status, hw->tp[i].int_mask);
    }
}
