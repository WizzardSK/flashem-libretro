#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* V.Flash (V.Smile Pro)
 * SoC: LSI Logic ZEVIO 1020 - ARM926EJ-S @ 150MHz, the chip in the TI-Nspire Classic
 * RAM: 16MB SDRAM
 * Media: CD-ROM, ISO 9660, no copy protection
 * OS: µMORE v4.0 RTOS, booted by the 2MB boot ROM (70004.bin, required)
 *
 * The machine itself is src/hw.c; this is the interface frontends use. */

#define VFLASH_SCREEN_W     320
#define VFLASH_SCREEN_H     240
/* Largest picture the video engine produces (PAL, 352x288); the framebuffer
 * is this big, and vflash_get_screen_size() says how much of it is used. */
#define VFLASH_FB_MAX_W     512
#define VFLASH_FB_MAX_H     288

/* Input buttons */
#define VFLASH_BTN_UP       (1 << 0)
#define VFLASH_BTN_DOWN     (1 << 1)
#define VFLASH_BTN_LEFT     (1 << 2)
#define VFLASH_BTN_RIGHT    (1 << 3)
#define VFLASH_BTN_RED      (1 << 4)
#define VFLASH_BTN_YELLOW   (1 << 5)
#define VFLASH_BTN_GREEN    (1 << 6)
#define VFLASH_BTN_BLUE     (1 << 7)
#define VFLASH_BTN_ENTER    (1 << 8)

typedef struct VFlash VFlash;

/* Directory to look in for the boot ROM (70004.bin), as <dir>/flashem/70004.bin
 * or <dir>/70004.bin. Set it before vflash_create(); a frontend passes its own
 * system directory here. $FLASHEM_BIOS and ./70004.bin are tried after it. */
void      vflash_set_bios_dir(const char *dir);
/* NULL if the boot ROM cannot be found or the disc image cannot be opened. */
VFlash   *vflash_create(const char *disc_path);
/* Loads a program into RAM and starts the CPU at its entry point instead of the
 * boot ROM's reset vector: a cold start, as at power-on - SVC mode, IRQ and
 * FIQ masked, MMU and caches off, the stacks where the ROM puts them -
 * with nothing else the ROM would have set up.
 * Call it right after vflash_create(), before the first frame.
 * - ELF (32-bit ARM): each PT_LOAD segment at its physical address, the rest
 *   of its memory size zeroed; the entry is e_entry.
 * - BOOT.BIN (starts with "BOOT"): the whole file at the load address in its
 *   header, entered at +0x10 (the "ldr pc" the boot ROM jumps to).
 * - Anything else: the raw file at load_addr.
 * load_addr / entry override the file's own when not VFLASH_ADDR_NONE (entry
 * defaults to the load address for a raw file). Returns 1 on success. */
#define VFLASH_ADDR_NONE 0xFFFFFFFFu
int       vflash_load_program(VFlash *vf, const char *path, uint32_t load_addr, uint32_t entry);
/* The memory card. vflash_memcard_load inserts the card image at path, or a
 * blank card when the file does not exist yet (0 if it cannot be read);
 * vflash_memcard_save writes the card back when it was written to since it was
 * inserted or last saved (1 when there was nothing to do or it went well). */
int       vflash_memcard_load(VFlash *vf, const char *path);
int       vflash_memcard_save(VFlash *vf, const char *path);
void      vflash_destroy(VFlash *vf);
void      vflash_run_frame(VFlash *vf);
/* 1 while the ROM is still starting a disc's game (see hw_booting) and fast
 * boot is on (the default; VFLASH_FASTBOOT=0 turns it off): frontends may
 * run frames back to back and show only some of them. */
int       vflash_fast_booting(VFlash *vf);
void      vflash_set_input(VFlash *vf, uint32_t buttons);
uint32_t *vflash_get_framebuffer(VFlash *vf);
/* Current picture size; the framebuffer's pitch is width * 4 bytes. */
void      vflash_get_screen_size(VFlash *vf, int *w, int *h);
void      vflash_init_audio(VFlash *vf);
/* The audio ring buffer, for a frontend that takes the samples itself
 * (see audio_init_external / audio_pull_samples). */
struct Audio;
void     *vflash_get_audio(VFlash *vf);
void      vflash_set_debug(VFlash *vf, int on);

/* ---- Debugger API ---- */
uint32_t  vflash_get_pc(VFlash *vf);
uint32_t  vflash_get_reg(VFlash *vf, int r);
void      vflash_set_reg(VFlash *vf, int r, uint32_t val);
uint32_t  vflash_get_cpsr(VFlash *vf);
int       vflash_is_thumb(VFlash *vf);
/* Memory through the MMU, as the CPU sees it (virtual addresses). */
uint32_t  vflash_read32(VFlash *vf, uint32_t addr);
uint8_t   vflash_read8(VFlash *vf, uint32_t addr);
void      vflash_write32(VFlash *vf, uint32_t addr, uint32_t val);
/* Execute exactly one instruction; returns cycles consumed */
int       vflash_step(VFlash *vf);
/* Breakpoints (max 16). vflash_run_frame stops at one. */
void      vflash_bp_set(VFlash *vf, uint32_t addr);
void      vflash_bp_clear(VFlash *vf, uint32_t addr);
void      vflash_bp_clear_all(VFlash *vf);
int       vflash_bp_hit(VFlash *vf);   /* 1 if the last step or frame hit one */
uint32_t  vflash_bp_list(VFlash *vf, uint32_t *out, int maxn);
