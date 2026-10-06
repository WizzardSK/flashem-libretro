/* retro_smoke: a minimal libretro frontend to check a core build without RetroArch.
 *
 *   retro_smoke <core.dll|.so> <game.cue> <system dir> [frames] [out.ppm]
 *
 * Loads the core, points its system directory at <system dir> (where 70004.bin
 * lives), runs [frames] frames (default 600) with no input and writes the last
 * video frame as a PPM. Build: gcc tools/retro_smoke.c -Isrc -o retro_smoke -ldl
 * (Linux) or x86_64-w64-mingw32-gcc tools/retro_smoke.c -Isrc -o retro_smoke.exe. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "libretro.h"

#ifdef _WIN32
#include <windows.h>
#define LIB_OPEN(p)    ((void *)LoadLibraryA(p))
#define LIB_SYM(h, s)  ((void *)GetProcAddress((HMODULE)(h), s))
#else
#include <dlfcn.h>
#define LIB_OPEN(p)    dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define LIB_SYM(h, s)  dlsym(h, s)
#endif

static const char *sysdir;
static const uint32_t *last_fb;
static unsigned last_w, last_h;
static size_t last_pitch;
static unsigned long frames_seen, samples_seen;
static unsigned long nonzero_samples;

static void log_printf(enum retro_log_level level, const char *fmt, ...) {
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static bool environ_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = sysdir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_printf;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (data) { last_fb = data; last_w = w; last_h = h; last_pitch = pitch; }
    frames_seen++;
}
static void audio_cb(int16_t l, int16_t r) { nonzero_samples += l != 0 || r != 0; samples_seen++; }
static size_t audio_batch_cb(const int16_t *d, size_t n) {
    for (size_t i=0;i<n;i++) nonzero_samples += d[i*2] != 0 || d[i*2+1] != 0;
    samples_seen += n; return n;
}
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    (void)port; (void)dev; (void)idx; (void)id; return 0;
}

#define SYM(name) name##_t name = (name##_t)LIB_SYM(h, #name); \
    if (!name) { fprintf(stderr, "missing %s\n", #name); return 1; }

typedef void (*retro_init_t)(void);
typedef void (*retro_deinit_t)(void);
typedef void (*retro_run_t)(void);
typedef bool (*retro_load_game_t)(const struct retro_game_info *);
typedef void (*retro_unload_game_t)(void);
typedef void (*retro_get_system_info_t)(struct retro_system_info *);
typedef void (*retro_set_environment_t)(retro_environment_t);
typedef void (*retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void (*retro_set_audio_sample_t)(retro_audio_sample_t);
typedef void (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
typedef void (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void (*retro_set_input_state_t)(retro_input_state_t);

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: retro_smoke <core> <game.cue|-> <system dir> [frames] [out.ppm]\n");
        return 2;
    }
    sysdir = argv[3];
    long frames = argc > 4 ? atol(argv[4]) : 600;
    const char *out = argc > 5 ? argv[5] : "retro_smoke.ppm";

    void *h = LIB_OPEN(argv[1]);
    if (!h) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    SYM(retro_init) SYM(retro_deinit) SYM(retro_run) SYM(retro_load_game) SYM(retro_unload_game)
    SYM(retro_get_system_info) SYM(retro_set_environment) SYM(retro_set_video_refresh)
    SYM(retro_set_audio_sample) SYM(retro_set_audio_sample_batch) SYM(retro_set_input_poll)
    SYM(retro_set_input_state)

    struct retro_system_info si;
    memset(&si, 0, sizeof si);
    retro_get_system_info(&si);
    printf("core: %s %s (extensions %s)\n", si.library_name, si.library_version, si.valid_extensions);

    retro_set_environment(environ_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    /* "-" for no content: the console's own menu */
    struct retro_game_info gi = { argv[2], NULL, 0, NULL };
    if (!retro_load_game(strcmp(argv[2], "-") ? &gi : NULL)) { fprintf(stderr, "retro_load_game failed\n"); return 1; }
    for (long i = 0; i < frames; i++) {
        unsigned long before = samples_seen;
        retro_run();
        /* Use only for a short run wholly inside accelerated boot. */
        if (getenv("RETRO_SMOKE_SILENT_BOOT") &&
            (samples_seen-before != 735 || nonzero_samples)) {
            fprintf(stderr,"invalid fast-boot audio at call %ld: %lu frames, %lu nonzero\n",
                    i,samples_seen-before,nonzero_samples);
            return 1;
        }
    }
    printf("ran %ld frames: %lu video callbacks, %lu audio samples, last frame %ux%u\n",
           frames, frames_seen, samples_seen, last_w, last_h);

    if (last_fb) {
        FILE *f = fopen(out, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", last_w, last_h);
            for (unsigned y = 0; y < last_h; y++)
                for (unsigned x = 0; x < last_w; x++) {
                    uint32_t p = ((const uint32_t *)((const char *)last_fb + y * last_pitch))[x];
                    unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
            printf("wrote %s\n", out);
        }
    }
    retro_unload_game();
    retro_deinit();
    return 0;
}
