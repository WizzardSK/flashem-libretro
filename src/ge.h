#pragma once
#include <stdint.h>
#include <stddef.h>

/* The ZEVIO 1020's graphics engine (the 3D core co-developed with KOTO),
 * driven through the 0xA8000000 block by command lists in SDRAM. See ge.c
 * for what is known of the list format. */

typedef struct GE {
    uint8_t  *ram;          /* SDRAM, based at 0x10000000 */
    uint32_t  ram_size;
    uint32_t  surface;      /* where the surface's row y = 0 would be (A8 +0x98 holds
                             * the address of row +0x90 >> 16); may lie below RAM */
    int       top;          /* +0x90 >> 16: rows above it are not the surface's */
    uint32_t  reg[256];     /* registers set by 0x20 commands */
    int       clip_x0, clip_y0, clip_x1, clip_y1;   /* 0x82 / 0x83 */
    int       org_x, org_y;                         /* 0x81 */
    int       tex_x, tex_y, tex_bpp;                /* 0x87 */
    int       pal_x, pal_y, pal_pending;            /* 0x8F-0x94 */
    uint32_t  tex_fmt;                              /* 0x86 */
    uint32_t  unknown[256]; /* opcode (top byte) seen but not handled: count */
    int       log;
    int       trace;        /* print every command (VFLASH_GETRACE=<frame>) */
    uint32_t  sprites, pixels;   /* statistics for VFLASH_GELOG */
    uint32_t  lists, fills, calls, tris;   /* per-report statistics */
    uint32_t  vb;                           /* 0x0A vertex buffer */
    int       nodepth;                      /* VFLASH_NODEPTH: no depth test */
    int       no3d;                         /* VFLASH_NO3D: skip triangles */
} GE;

void ge_run(GE *g, uint32_t list);
/* Save states: the engine's own buffers and registers (one engine) */
size_t ge_state_size(void);
void   ge_state_save(uint8_t *p);
void   ge_state_load(const uint8_t *p);

/* The engine's 16-bit float: sign (bit 15), exponent (bits 14-9, bias 31),
 * mantissa (bits 8-0, leading 1 implied); 0x0000 reads as 0. The game
 * kernels decode it in software at 0x10A6E658 (Disney Princess), the
 * inverse of the math unit's 20.12 -> 16 converter. VFLASH_HALF=1 goes
 * back to reading it as an IEEE half (for comparison). */
double   ge_f16_to(uint16_t h);
uint16_t ge_f16_from(double v);
