#pragma once
#include <stdint.h>

/* Incremental architectural interpreter. Unsupported operations stop with
 * fault_pc/fault_op; they are never treated as NOPs. Cycle counts currently
 * count instructions, not superscalar issue groups. */
typedef struct {
    uint16_t r[16], c[32], pc, flags;
    uint16_t fault_pc, fault_op;
    uint64_t instructions;
    int fault;
    uint16_t shadow[8], timer_reload[2];
    uint8_t timer_div[2];
    void *ctx;
    uint16_t (*read)(void *, uint16_t, int program);
    void (*write)(void *, uint16_t, uint16_t, int program);
    /* Optional: the on-chip program and data SRAM, read and written
     * directly where the memory mode maps them (else read/write) */
    uint8_t *iram, *dram;
} ZSP400;
void zsp400_reset(ZSP400 *s);
int zsp400_step(ZSP400 *s);
void zsp400_request_irq(ZSP400 *s, unsigned line);
/* 1 while the core sleeps with nothing of its own to wake it - no timer
 * running, no interrupt pending - so steps would change nothing */
int zsp400_idle(const ZSP400 *s);
