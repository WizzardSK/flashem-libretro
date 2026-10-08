#pragma once
#include <stdint.h>
#include <stdio.h>
#include "arm9.h"

/* The V.Flash machine: the ARM core runs the real boot ROM, and everything it
 * talks to is a device model behind a physical memory map. Accesses to
 * addresses nothing models are logged as [HW?], which is how the next device
 * to write is found. */

typedef struct HW HW;
struct CDROM;

HW      *hw_create(ARM9 *cpu, const uint8_t *rom, uint32_t rom_size,
                   struct CDROM *cd, uint32_t *framebuf);
void     hw_destroy(HW *hw);
void     hw_set_input(HW *hw, uint32_t buttons);
unsigned hw_leds(HW *hw);
void     hw_set_audio_sink(HW *hw, void *ctx,
                          void (*push)(void *, const int16_t *, uint32_t));
/* One video frame's worth of emulation; stops early at a breakpoint. */
void     hw_run_frame(HW *hw);
void     hw_screen_size(HW *hw, int *w, int *h);
int      hw_booting(HW *hw);
/* Copies data into SDRAM at a physical address (program loading, before the
 * CPU runs); 0 if it does not fit in the 16 MB at 0x10000000. */
int      hw_load_phys(HW *hw, uint32_t pa, const uint8_t *data, uint32_t len);
/* The memory card, an 8 MB DataFlash on the SPI. Insert copies the image in
 * (shorter ones are padded erased, NULL is a blank card); hw_memcard gives the
 * card's contents to save, hw_memcard_dirty whether it was written since the
 * last clear. */
int      hw_memcard_insert(HW *hw, const uint8_t *data, uint32_t len);
uint8_t *hw_memcard(HW *hw, uint32_t *len);
int      hw_memcard_dirty(HW *hw, int clear);

/* One instruction, with interrupts and timers; returns cycles taken. */
int      hw_step(HW *hw);
void     hw_bp_set(HW *hw, uint32_t addr);
void     hw_bp_clear(HW *hw, uint32_t addr);
void     hw_bp_clear_all(HW *hw);
int      hw_bp_hit(HW *hw);
uint32_t hw_bp_list(HW *hw, uint32_t *out, int maxn);

/* Memory callbacks installed on the ARM core (ctx = HW*), virtual addresses. */
uint32_t hw_read32(void *ctx, uint32_t va);
uint16_t hw_read16(void *ctx, uint32_t va);
uint8_t  hw_read8 (void *ctx, uint32_t va);
void     hw_write32(void *ctx, uint32_t va, uint32_t v);
void     hw_write16(void *ctx, uint32_t va, uint16_t v);
void     hw_write8 (void *ctx, uint32_t va, uint8_t v);

/* vflash_set_uart_output() */
extern FILE *hw_uart_out;
