#pragma once
#include <stdint.h>
#include "zsp400.h"

/* DSP transfer bus is distinct from the ARM bus: DC000000 is DSP data
 * memory here, but the interrupt controller on the ARM side. */
/* The movie player's high-level emulation (zevio_dsp.c, #7): the state of
 * the MIAV stream and the frame events while the DSP core stays stopped */
typedef struct {
    int      active, playing, waiting_ack, skip_next, cmd_pending;
    uint32_t ring_base, ring_size, rd, consumed_chunk;
    uint32_t ybuf, ubuf, vbuf;
    uint32_t a1_base, a2_base, a_size, a1_wr, a2_wr;
    uint32_t frames_total, frame, fps, w, h;
    uint64_t clock, next_at;
    uint32_t hdr_len;
    uint8_t  hdr[2048];
} MjpPlay;

typedef struct {
    uint8_t p[0x20000], d[0x20000];
    uint8_t *ram;
    uint32_t ram_base, ram_size;
    uint32_t dma[0x100/4], control[0x120/4];
    uint64_t transfers, words;
    int trace;
    ZSP400 core;
    int running, fault_reported;
    uint32_t cpu_hz;   /* the ARM clock the run budgets count, for the HLE's pace */
    MjpPlay play;
} ZevioDSP;

void zevio_dsp_init(ZevioDSP *z, uint8_t *ram, uint32_t base, uint32_t size);
void zevio_dsp_reset(ZevioDSP *z);
uint32_t zevio_dsp_dma_read(ZevioDSP *z, uint32_t offset);
void zevio_dsp_dma_write(ZevioDSP *z, uint32_t offset, uint32_t value);
uint32_t zevio_dsp_control_read(ZevioDSP *z, uint32_t offset);
void zevio_dsp_control_write(ZevioDSP *z, uint32_t offset, uint32_t value);
void zevio_dsp_run(ZevioDSP *z, uint64_t budget);
