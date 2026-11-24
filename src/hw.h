#pragma once
#include <stdint.h>
#include <stddef.h>
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
/* The system flash, 64 KB on the SPI: the console's settings and the games'
 * records (Bratz's profile). It is always there, erased at the start; insert
 * copies an image in (shorter ones are padded erased, NULL erases it),
 * hw_sysflash gives its contents to save. */
int      hw_sysflash_insert(HW *hw, const uint8_t *data, uint32_t len);
uint8_t *hw_sysflash(HW *hw, uint32_t *len);
/* Writes the console's settings in the given language into the system flash
 * (0x7A Dutch ... 0x7F English US, see hw.c; 0 erases the settings, and the
 * ROM picks its own); 0 for any other code. Before the first instruction,
 * after the system flash's image is inserted. */
int      hw_set_language(HW *hw, int code);
/* Save states (see hw.c): the size, and save / load into a buffer of at
 * least that size; 0 on failure (a load leaves the machine as it was when
 * the state does not fit this build). */
size_t   hw_state_size(HW *hw);
int      hw_state_save(HW *hw, void *data, size_t size);
int      hw_state_load(HW *hw, const void *data, size_t size);

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
