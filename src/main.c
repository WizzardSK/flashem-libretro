#include "vflash.h"
#include "debugger.h"
#include "audio.h"
#include "frame_pacer.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "FlashEm - V.Flash emulator\n"
        "Usage: %s [options] [<disc.cue>]\n"
        "       %s [options] --load <program> [<disc.cue>]\n"
        "With no disc the console starts into its menu, with the drive empty.\n\n"
        "Options:\n"
        "  --dbg        Start interactive debugger (paused at boot)\n"
        "  --dbg-run    Start interactive debugger (running)\n"
        "  --headless   Run without display\n"
        "  --scale N    Window scale factor (default: 2)\n"
        "  --load F[@A] Load a program and start it instead of the boot ROM: an ELF,\n"
        "               a BOOT.BIN, or a raw binary at address A (hex)\n"
        "  --entry A    Start the loaded program at address A (hex) instead\n"
        "  --memcard F  Insert the memory card image F (8 MB, blank if F does not\n"
        "               exist); written back on exit when the card was written to\n"
        "  --help       Show this help\n\n"
        "Controls:\n"
        "  Arrow keys   D-Pad\n"
        "  Z / X / C / V  Red/Yellow/Green/Blue\n"
        "  Enter        Enter/OK\n"
        "  F2           Pause/resume debugger\n"
        "  F11          Toggle fullscreen\n"
        "  Esc          Quit\n\n"
        "Debugger commands (on stdin):\n"
        "  s [N]        step N instructions\n"
        "  c            continue\n"
        "  n            step over\n"
        "  b <addr>     breakpoint\n"
        "  bc <addr>    clear breakpoint\n"
        "  bl           list breakpoints\n"
        "  r            registers\n"
        "  m <addr> [N] memory dump\n"
        "  d <addr> [N] disassemble\n"
        "  pc           current instruction\n"
        "  bt           stack dump\n"
        "  setreg r0=1  write register\n"
        "  q            quit\n",
        prog, prog);
}

/* Line-buffer a log stream. The Windows C runtime treats _IOLBF as full
 * buffering, so there it is unbuffered instead - logs must not sit in a
 * buffer when a run is killed. */
static void line_buffered(FILE *f) {
#ifdef _WIN32
    setvbuf(f, NULL, _IONBF, 0);
#else
    setvbuf(f, NULL, _IOLBF, BUFSIZ);
#endif
}

int main(int argc, char **argv) {
    const char *disc_path = NULL;
    int debug    = 0;
    int headless = 0;
    int scale    = 2;
    int dbg_mode = 0;  /* 0=off, 1=paused at boot, 2=running with debugger */
    const char *program = NULL;
    const char *memcard = NULL;
    uint32_t load_addr = VFLASH_ADDR_NONE, entry = VFLASH_ADDR_NONE;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0)        debug    = 1;
        else if (strcmp(argv[i], "--headless") == 0) headless = 1;
        else if (strcmp(argv[i], "--dbg") == 0)      dbg_mode = 1;
        else if (strcmp(argv[i], "--dbg-run") == 0)  dbg_mode = 2;
        else if (strcmp(argv[i], "--help") == 0)    { print_usage(argv[0]); return 0; }
        else if (strcmp(argv[i], "--scale") == 0 && i+1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1 || scale > 4) scale = 2;
        }
        else if (strcmp(argv[i], "--load") == 0 && i+1 < argc) {
            static char path[1024];
            snprintf(path, sizeof(path), "%s", argv[++i]);
            char *at = strrchr(path, '@');
            if (at) { *at = '\0'; load_addr = (uint32_t)strtoul(at + 1, NULL, 16); }
            program = path;
        }
        else if (strcmp(argv[i], "--memcard") == 0 && i+1 < argc)
            memcard = argv[++i];
        else if (strcmp(argv[i], "--entry") == 0 && i+1 < argc)
            entry = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (argv[i][0] != '-') disc_path = argv[i];
    }

    /* Create emulator */
    VFlash *vf = vflash_create(disc_path);
    if (!vf) return 1;
    if (memcard && !vflash_memcard_load(vf, memcard)) {
        fprintf(stderr, "[Main] could not read memory card %s\n", memcard);
        vflash_destroy(vf);
        return 1;
    }
    if (program && !vflash_load_program(vf, program, load_addr, entry)) {
        vflash_destroy(vf);
        return 1;
    }

    if (debug) vflash_set_debug(vf, 1);

    /* Init debugger if requested */
    if (dbg_mode) dbg_init(vf, dbg_mode == 1);

    /* Init SDL */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[SDL] Init failed: %s\n", SDL_GetError());
        vflash_destroy(vf);
        return 1;
    }

    vflash_init_audio(vf);

    SDL_Window   *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture  *tex = NULL;
    int fullscreen = 0;

    if (!headless) {
        win = SDL_CreateWindow(
            "FlashEm",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            VFLASH_SCREEN_W * scale, VFLASH_SCREEN_H * scale,
            SDL_WINDOW_RESIZABLE
        );
        if (!win) {
            fprintf(stderr, "[SDL] Window failed: %s\n", SDL_GetError());
            vflash_destroy(vf); SDL_Quit(); return 1;
        }

        ren = SDL_CreateRenderer(win, -1,
            SDL_RENDERER_ACCELERATED);
        if (!ren) ren = SDL_CreateRenderer(win, -1, 0);

        tex = SDL_CreateTexture(ren,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            VFLASH_SCREEN_W, VFLASH_SCREEN_H);

        SDL_RenderSetLogicalSize(ren, VFLASH_SCREEN_W, VFLASH_SCREEN_H);
        printf("[SDL] Window: %dx%d (scale %d)\n",
               VFLASH_SCREEN_W * scale, VFLASH_SCREEN_H * scale, scale);
    }

    /* Redirect stdout to log file for capture when running in background.
     * Also keeps line-buffered so messages appear in real time. */
    {
        const char *logfile = getenv("VFLASH_LOG");
        if (logfile) {
            FILE *lf = freopen(logfile, "w", stdout);
            if (lf) line_buffered(lf);
        } else {
            line_buffered(stdout);
        }
    }

    /* Main loop */
    int       running   = 1;
    SDL_Event ev;
    uint32_t  fps_timer = SDL_GetTicks();
    int       fps_count = 0;
    char      title[64];
    Audio *audio = vflash_get_audio(vf);
    FramePacer pacer;
    frame_pacer_reset(&pacer);

    while (running) {
        uint32_t buttons = 0;
        int accelerated = !dbg_mode && vflash_fast_booting(vf);
        audio_set_discard(audio, accelerated);

        /* Events */
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT && !headless) {
                printf("[Main] SDL_QUIT received, exiting\n");
                running = 0; break;
            }
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE: printf("[Main] ESC pressed\n"); running = 0; break;
                    case SDLK_F2:
                        if (dbg_mode) {
                            if (dbg_is_running()) dbg_pause(vf);
                            else                  dbg_resume();
                        }
                        break;
                    case SDLK_F5: {
                        /* Save screenshot as BMP */
                        uint32_t *fb = vflash_get_framebuffer(vf);
                        int sw, sh;
                        vflash_get_screen_size(vf, &sw, &sh);
                        SDL_Surface *surf = SDL_CreateRGBSurfaceFrom(
                            fb, sw, sh, 32, sw*4,
                            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
                        if (surf) {
                            SDL_SaveBMP(surf, "screenshot.bmp");
                            SDL_FreeSurface(surf);
                            printf("[Main] Screenshot saved: screenshot.bmp\n");
                        }
                        break;
                    }
                    case SDLK_F11:
                        if (!headless) {
                            fullscreen = !fullscreen;
                            SDL_SetWindowFullscreen(win,
                                fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        }
                        break;
                }
            }
        }

        /* Input */
        if (!headless) {
            const uint8_t *keys = SDL_GetKeyboardState(NULL);
            if (keys[SDL_SCANCODE_UP])     buttons |= VFLASH_BTN_UP;
            if (keys[SDL_SCANCODE_DOWN])   buttons |= VFLASH_BTN_DOWN;
            if (keys[SDL_SCANCODE_LEFT])   buttons |= VFLASH_BTN_LEFT;
            if (keys[SDL_SCANCODE_RIGHT])  buttons |= VFLASH_BTN_RIGHT;
            if (keys[SDL_SCANCODE_Z])      buttons |= VFLASH_BTN_RED;
            if (keys[SDL_SCANCODE_X])      buttons |= VFLASH_BTN_YELLOW;
            if (keys[SDL_SCANCODE_C])      buttons |= VFLASH_BTN_GREEN;
            if (keys[SDL_SCANCODE_V])      buttons |= VFLASH_BTN_BLUE;
            if (keys[SDL_SCANCODE_RETURN]) buttons |= VFLASH_BTN_ENTER;
        }
        vflash_set_input(vf, buttons);

        /* Debugger frame tick (handles commands, stepping, BP check) */
        if (dbg_mode) {
            int dbg_ret = dbg_frame(vf);
            if (dbg_ret == DBG_QUIT) { running = 0; break; }
            /* If paused: don't run emulator, just render last frame */
            if (dbg_ret == DBG_PAUSE) goto render;
        }

        /* Run one emulator frame */
        vflash_run_frame(vf);

        /* Breakpoint check after frame */
        if (dbg_mode && vflash_bp_hit(vf)) {
            fprintf(stderr, "[DBG] *** Breakpoint hit at 0x%08X ***\n",
                    vflash_get_pc(vf));
        }

    render:
        /* Auto-screenshot in headless mode after game starts */
        if (headless) {
            /* fps_count resets every second, so it cannot say how far the
             * game has got; count frames since start instead. */
            /* VFLASH_SHOT_FRAME may list several frames ("600,1500,3000");
             * a %d in VFLASH_SHOT is replaced by the frame number. */
            static int total_frames = 0, nshots = -1, next_shot = 0;
            static int shot_frames[256];
            if (nshots < 0) {
                const char *sf = getenv("VFLASH_SHOT_FRAME");
                nshots = 0;
                if (!sf) shot_frames[nshots++] = 50;
                while (sf && *sf && nshots < 256) {
                    shot_frames[nshots++] = atoi(sf);
                    sf = strchr(sf, ',');
                    if (sf) sf++;
                }
            }
            total_frames++;
            if (next_shot < nshots && total_frames >= shot_frames[next_shot]) {
                uint32_t *fb = vflash_get_framebuffer(vf);
                int sw, sh;
                vflash_get_screen_size(vf, &sw, &sh);
                const char *spat = getenv("VFLASH_SHOT") ? getenv("VFLASH_SHOT") : "/tmp/vflash_screen.ppm";
                char sp[512];
                snprintf(sp, sizeof sp, spat, total_frames);
                FILE *pf = fopen(sp, "wb");
                if (pf) {
                    fprintf(pf, "P6\n%d %d\n255\n", sw, sh);
                    for (int i = 0; i < sw*sh; i++) {
                        uint32_t p = fb[i];
                        uint8_t rgb[3] = {(p>>16)&0xFF, (p>>8)&0xFF, p&0xFF};
                        fwrite(rgb, 1, 3, pf);
                    }
                    fclose(pf);
                    printf("[SCREENSHOT] Saved %s (frame %d)\n", sp, total_frames);
                }
                next_shot++;
            }
        }
        /* Render */
        /* Reduce rendering overhead during accelerated boot. */
        static unsigned boot_frames;
        if (!headless && win && !(vflash_fast_booting(vf) && (boot_frames++ & 7))) {
            /* The picture size follows the machine (320x240, or 320x288 on
             * a PAL system); the texture is remade when it changes. */
            static int tex_w = VFLASH_SCREEN_W, tex_h = VFLASH_SCREEN_H;
            int sw, sh;
            vflash_get_screen_size(vf, &sw, &sh);
            if (sw != tex_w || sh != tex_h) {
                SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, sw, sh);
                SDL_RenderSetLogicalSize(ren, sw * 3 / 4 * 4 / 3, sw * 3 / 4);
                tex_w = sw; tex_h = sh;
            }
            SDL_UpdateTexture(tex, NULL,
                vflash_get_framebuffer(vf),
                sw * sizeof(uint32_t));
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }

        /* FPS */
        fps_count++;
        uint32_t now = SDL_GetTicks();
        if (now - fps_timer >= 1000) {
            const char *dbg_tag = dbg_mode
                ? (dbg_is_running() ? " [DBG:run]" : " [DBG:paused]") : "";
            if (!headless && win) {
                snprintf(title, sizeof(title),
                         "V.Flash Emulator — %d FPS%s%s",
                         fps_count, debug ? " [TRACE]" : "", dbg_tag);
                SDL_SetWindowTitle(win, title);
            } else {
                printf("[Main] %d FPS%s\n", fps_count, dbg_tag);
            }
            fps_count = 0;
            fps_timer = now;
        }

        /* Fractional deadlines avoid the 62.5 Hz drift of a 16 ms delay.
         * Rendering is not vsynced: display refresh must not clock audio. */
        if (accelerated) {
            frame_pacer_reset(&pacer);
        } else if (!dbg_mode || dbg_is_running()) {
            frame_pacer_wait(&pacer, audio);
        } else {
            audio_clear(audio);
            SDL_Delay(16);  /* Paused: still yield CPU */
            frame_pacer_reset(&pacer);
        }
    }

    /* Cleanup */
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    if (memcard && !vflash_memcard_save(vf, memcard))
        fprintf(stderr, "[Main] could not write memory card %s\n", memcard);
    vflash_destroy(vf);
    SDL_Quit();
    printf("[Main] Exited cleanly\n");
    return 0;
}
