/* The frontend-facing side of the emulator: finds the boot ROM, opens the
 * disc and hands everything else to the machine in hw.c. */
#include "vflash.h"
#include "arm9.h"
#include "cdrom.h"
#include "audio.h"
#include "hw.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ROM_MAX (2u * 1024 * 1024)

struct VFlash {
    ARM9      cpu;
    CDROM    *cd;
    Audio    *audio;
    HW       *hw;
    uint8_t  *rom;
    uint32_t  rom_size;
    uint32_t  framebuf[VFLASH_FB_MAX_W * VFLASH_FB_MAX_H];
};

static char s_bios_dir[1024];
static void push_hardware_audio(void *ctx,const int16_t *samples,uint32_t count) {
    audio_push_samples((Audio *)ctx,samples,count);
}

void vflash_set_bios_dir(const char *dir) {
    if (!dir) { s_bios_dir[0] = '\0'; return; }
    snprintf(s_bios_dir, sizeof(s_bios_dir), "%s", dir);
}

/* The directory a frontend handed us (RetroArch's system directory), then
 * $FLASHEM_BIOS, then beside the binary for a standalone run. */
static int load_rom(VFlash *vf) {
    char sys_sub[1100], sys_rom[1100];
    const char *paths[5];
    int n = 0;

    if (s_bios_dir[0]) {
        snprintf(sys_sub, sizeof(sys_sub), "%s/flashem/70004.bin", s_bios_dir);
        snprintf(sys_rom, sizeof(sys_rom), "%s/70004.bin", s_bios_dir);
        paths[n++] = sys_sub;
        paths[n++] = sys_rom;
    }
    if (getenv("FLASHEM_BIOS"))
        paths[n++] = getenv("FLASHEM_BIOS");
    paths[n++] = "70004.bin";
    paths[n] = NULL;

    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && (unsigned long)sz <= ROM_MAX) {
            vf->rom = malloc((size_t)sz);
            if (vf->rom && fread(vf->rom, 1, (size_t)sz, f) == (size_t)sz) {
                vf->rom_size = (uint32_t)sz;
                fclose(f);
                printf("[VFlash] Boot ROM: %s (%ld bytes)\n", paths[i], sz);
                return 1;
            }
            free(vf->rom);
            vf->rom = NULL;
        }
        fclose(f);
    }
    return 0;
}

VFlash *vflash_create(const char *disc_path) {
    VFlash *vf = calloc(1, sizeof(VFlash));
    if (!vf) return NULL;
    vf->cd    = cdrom_create();
    vf->audio = audio_create();

    if (!load_rom(vf)) {
        fprintf(stderr, "[VFlash] Boot ROM 70004.bin not found (system directory, "
                        "$FLASHEM_BIOS or the working directory)\n");
        vflash_destroy(vf);
        return NULL;
    }
    if (disc_path && !cdrom_open(vf->cd, disc_path)) {
        fprintf(stderr, "[VFlash] Failed to open disc: %s\n", disc_path);
        vflash_destroy(vf);
        return NULL;
    }
    vf->hw = hw_create(&vf->cpu, vf->rom, vf->rom_size, vf->cd, vf->framebuf);
    hw_set_audio_sink(vf->hw,vf->audio,push_hardware_audio);
    return vf;
}

int vflash_memcard_load(VFlash *vf, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return hw_memcard_insert(vf->hw, NULL, 0);
    uint8_t *data = malloc(8u << 20);
    size_t n = data ? fread(data, 1, 8u << 20, f) : 0;
    fclose(f);
    int ok = data && hw_memcard_insert(vf->hw, data, (uint32_t)n);
    free(data);
    return ok;
}

int vflash_memcard_save(VFlash *vf, const char *path) {
    uint32_t len;
    uint8_t *mc = hw_memcard(vf->hw, &len);
    if (!mc || !hw_memcard_dirty(vf->hw, 0)) return 1;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(mc, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    if (ok) hw_memcard_dirty(vf->hw, 1);
    return ok;
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int vflash_load_program(VFlash *vf, const char *path, uint32_t load_addr, uint32_t entry) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[VFlash] Cannot open program: %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size > 0 ? malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "[VFlash] Cannot read program: %s\n", path);
        free(data); fclose(f); return 0;
    }
    fclose(f);

    int ok = 1;
    uint32_t start;
    if (size >= 52 && !memcmp(data, "\x7F" "ELF", 4)) {
        /* 32-bit (1), little-endian (1), ARM (40) */
        if (data[4] != 1 || data[5] != 1 || rd16le(data + 18) != 40) {
            fprintf(stderr, "[VFlash] %s is not a 32-bit little-endian ARM ELF\n", path);
            free(data); return 0;
        }
        uint32_t phoff = rd32le(data + 28);
        uint16_t phentsize = rd16le(data + 42), phnum = rd16le(data + 44);
        start = rd32le(data + 24);
        for (uint16_t i = 0; ok && i < phnum; i++) {
            const uint8_t *ph = data + phoff + (uint32_t)i * phentsize;
            if (ph + 32 > data + size || rd32le(ph) != 1)   /* PT_LOAD */
                continue;
            uint32_t off = rd32le(ph + 4), paddr = rd32le(ph + 12);
            uint32_t filesz = rd32le(ph + 16), memsz = rd32le(ph + 20);
            if ((uint64_t)off + filesz > (uint64_t)size || filesz > memsz) { ok = 0; break; }
            uint8_t *seg = calloc(1, memsz ? memsz : 1);
            memcpy(seg, data + off, filesz);
            ok = hw_load_phys(vf->hw, paddr, seg, memsz);
            if (!ok) fprintf(stderr, "[VFlash] ELF segment 0x%08X+0x%X is outside RAM\n", paddr, memsz);
            else fprintf(stderr, "[VFlash] Loaded segment 0x%08X (%u bytes, %u in file)\n", paddr, memsz, filesz);
            free(seg);
        }
    } else if (size >= 0x14 && !memcmp(data, "BOOT", 4)) {
        uint32_t base = load_addr != VFLASH_ADDR_NONE ? load_addr : rd32le(data + 8);
        ok = hw_load_phys(vf->hw, base, data, (uint32_t)size);
        start = base + 0x10;
        if (ok) fprintf(stderr, "[VFlash] Loaded BOOT.BIN at 0x%08X (%ld bytes)\n", base, size);
    } else {
        if (load_addr == VFLASH_ADDR_NONE) {
            fprintf(stderr, "[VFlash] %s is a raw binary: give it a load address\n", path);
            free(data); return 0;
        }
        ok = hw_load_phys(vf->hw, load_addr, data, (uint32_t)size);
        start = load_addr;
        if (ok) fprintf(stderr, "[VFlash] Loaded %s at 0x%08X (%ld bytes)\n", path, load_addr, size);
    }
    free(data);
    if (!ok) { fprintf(stderr, "[VFlash] %s does not fit in RAM (0x10000000, 16 MB)\n", path); return 0; }

    if (entry != VFLASH_ADDR_NONE) start = entry;
    /* The power-on state arm9_reset leaves, only at the entry instead of 0;
     * bit 0 of the address selects Thumb, as for a BX. */
    vf->cpu.r[15] = start & ~1u;
    /* The stacks the boot ROM gives each mode (its code at 0x22C). Programs
     * set up their own SVC stack, but their interrupt handlers run on the
     * IRQ/FIQ ones the ROM left, and with those at 0 the first push of a
     * handler went to 0xFFFFFFxx. A user-mode stack the
     * ROM reads from memory; this one sits below the IRQ stack. */
    vf->cpu.r13_irq = 0x107FB000u;
    vf->cpu.r13_fiq = 0x107FC000u;
    vf->cpu.r13_und = 0x107FD000u;
    vf->cpu.r13_abt = 0x107FE000u;
    vf->cpu.r13_usr = 0x107FA000u;
    vf->cpu.r[13] = 0x10800000u;
    vf->cpu.cpsr = ARM9_MODE_SVC | ARM9_FLAG_I | ARM9_FLAG_F | ((start & 1u) ? ARM9_FLAG_T : 0);
    fprintf(stderr, "[VFlash] Starting at 0x%08X%s\n", start & ~1u, (start & 1u) ? " (Thumb)" : "");
    return 1;
}

void vflash_destroy(VFlash *vf) {
    if (!vf) return;
    hw_destroy(vf->hw);
    cdrom_destroy(vf->cd);
    audio_destroy(vf->audio);
    free(vf->rom);
    free(vf);
}

void      vflash_run_frame(VFlash *vf)                  { hw_run_frame(vf->hw); }

int vflash_fast_booting(VFlash *vf) {
    static int on = -1;
    if (on < 0) on = !getenv("VFLASH_FASTBOOT") || atoi(getenv("VFLASH_FASTBOOT")) != 0;
    return on && hw_booting(vf->hw);
}
void      vflash_set_input(VFlash *vf, uint32_t b)      { hw_set_input(vf->hw, b); }
unsigned  vflash_leds(VFlash *vf)                       { return hw_leds(vf->hw); }
void      vflash_set_uart_output(FILE *f)               { hw_uart_out = f; }
uint32_t *vflash_get_framebuffer(VFlash *vf)            { return vf->framebuf; }
void      vflash_get_screen_size(VFlash *vf, int *w, int *h) { hw_screen_size(vf->hw, w, h); }
void     *vflash_get_audio(VFlash *vf)                  { return vf ? vf->audio : NULL; }
void      vflash_init_audio(VFlash *vf)                 { audio_init_sdl(vf->audio); }
void      vflash_set_debug(VFlash *vf, int on)          { (void)vf; (void)on; }

/* ---- Debugger API ---- */

uint32_t vflash_get_pc(VFlash *vf)         { return vf->cpu.r[15]; }
uint32_t vflash_get_reg(VFlash *vf, int r) { return (r >= 0 && r < 16) ? vf->cpu.r[r] : 0; }
void     vflash_set_reg(VFlash *vf, int r, uint32_t val) { if (r >= 0 && r < 16) vf->cpu.r[r] = val; }
uint32_t vflash_get_cpsr(VFlash *vf)       { return vf->cpu.cpsr; }
int      vflash_is_thumb(VFlash *vf)       { return (vf->cpu.cpsr >> 5) & 1; }

uint32_t vflash_read32(VFlash *vf, uint32_t a)             { return hw_read32(vf->hw, a); }
uint8_t  vflash_read8(VFlash *vf, uint32_t a)              { return hw_read8(vf->hw, a); }
void     vflash_write32(VFlash *vf, uint32_t a, uint32_t v) { hw_write32(vf->hw, a, v); }

int      vflash_step(VFlash *vf)                          { return hw_step(vf->hw); }
void     vflash_bp_set(VFlash *vf, uint32_t addr)         { hw_bp_set(vf->hw, addr); }
void     vflash_bp_clear(VFlash *vf, uint32_t addr)       { hw_bp_clear(vf->hw, addr); }
void     vflash_bp_clear_all(VFlash *vf)                  { hw_bp_clear_all(vf->hw); }
int      vflash_bp_hit(VFlash *vf)                        { return hw_bp_hit(vf->hw); }
uint32_t vflash_bp_list(VFlash *vf, uint32_t *out, int n) { return hw_bp_list(vf->hw, out, n); }
