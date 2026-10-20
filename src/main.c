#include "vflash.h"
#include "debugger.h"
#include "audio.h"
#include "frame_pacer.h"
#include "input_config.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <commdlg.h>
#define dup _dup
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

static void print_usage(const char *prog) {
    fprintf(stderr,
        "FlashEm - V.Flash emulator\n"
        "Usage: %s [options] [<disc.cue>]\n"
        "       %s [options] --load <program> [<disc.cue>]\n"
        "With no disc a file picker asks for one; cancelled, the console starts\n"
        "into its menu with the drive empty.\n\n"
        "Options:\n"
        "  --dbg        Start interactive debugger (paused at boot)\n"
        "  --dbg-run    Start interactive debugger (running)\n"
        "  --headless   Run without display\n"
        "  --scale N    Window scale factor (default: 2)\n"
        "  --language L The console's language: bios, en-us, en-uk, fr, es, de, nl\n"
        "               (default: flashem.ini's [console] language, else bios)\n"
        "  --load F[@A] Load a program and start it instead of the boot ROM: an ELF,\n"
        "               a BOOT.BIN, or a raw binary at address A (hex)\n"
        "  --entry A    Start the loaded program at address A (hex) instead\n"
        "  --memcard F  Insert the memory card image F (8 MB, blank if F does not\n"
        "               exist); written back on exit when the card was written to\n"
        "  --quiet      Show only what the program writes to its UARTs, not the\n"
        "               emulator's log\n"
        "  --help       Show this help\n\n"
        "Controls (keyboard; rebind them, and the gamepad's, in flashem.ini beside\n"
        "the program, written with these defaults on the first start):\n"
        "  Arrow keys   Stick\n"
        "  Z / X / C / V  Red/Yellow/Green/Blue (right/up/left/down)\n"
        "  Enter        OK\n"
        "  Backspace    Exit\n"
        "  Q / W        Left/Right shoulder\n"
        "  Space        Stick button\n"
        "  H / B        Question/Book\n"
        "  1 - 4        A-D\n"
        "  F2           Pause/resume debugger\n"
        "  F5           Save screenshot.bmp\n"
        "  O            Open another disc (the console restarts with it)\n"
        "  Tab          Turbo while held (unthrottled, no sound)\n"
        "  F11          Toggle fullscreen\n"
        "  A disc image dropped on the window is opened the same way.\n"
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

/* Ask for a disc image with the system's own file picker: the Windows common
 * dialog, AppleScript's on macOS, zenity or kdialog elsewhere. 1 with the
 * chosen path in out, 0 when cancelled or when there is no picker. */
static int choose_disc(char *out, size_t size) {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "V.Flash disc (*.cue;*.bin;*.iso)\0*.cue;*.bin;*.iso\0All files\0*.*\0";
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = (DWORD)size;
    ofn.lpstrTitle  = "FlashEm - open a disc";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
#else
    static const char *const pickers[] = {
#ifdef __APPLE__
        "osascript -e 'POSIX path of (choose file with prompt \"FlashEm - open a disc\" "
        "of type {\"cue\", \"bin\", \"iso\"})' 2>/dev/null",
#endif
        "zenity --file-selection --title='FlashEm - open a disc' "
        "--file-filter='V.Flash disc | *.cue *.CUE *.bin *.BIN *.iso *.ISO' 2>/dev/null",
        "kdialog --title 'FlashEm - open a disc' --getopenfilename . "
        "'*.cue *.CUE *.bin *.BIN *.iso *.ISO|V.Flash disc' 2>/dev/null",
    };
    for (size_t i = 0; i < sizeof(pickers) / sizeof(pickers[0]); i++) {
        FILE *p = popen(pickers[i], "r");
        if (!p) continue;
        out[0] = '\0';
        int got = fgets(out, (int)size, p) != NULL;
        int status = pclose(p);
        if (got) {
            out[strcspn(out, "\r\n")] = '\0';
            if (out[0]) return 1;
        }
        /* 1 is the picker's own "cancelled"; anything else (127: not
         * installed) means try the next one */
        if (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 1) return 0;
    }
    fprintf(stderr, "[Main] no file picker (zenity or kdialog) found; give the disc on the command line\n");
    return 0;
#endif
}

/* The console restarted with another disc in the drive, as when a disc is
 * changed with the power off: the memory card is written back and inserted
 * again. 0 when the disc cannot be opened; the running console then stays. */
/* --language, else flashem.ini's: kept for every console started */
static const char *s_language;

static int switch_disc(VFlash **vf, Audio **audio, const char *disc_path, const char *memcard) {
    if (memcard && !vflash_memcard_save(*vf, memcard))
        fprintf(stderr, "[Main] could not write memory card %s\n", memcard);
    VFlash *next = vflash_create(disc_path);
    if (!next) {
        fprintf(stderr, "[Main] could not open %s\n", disc_path);
        return 0;
    }
    vflash_destroy(*vf);
    *vf = next;
    vflash_set_language(next, s_language);
    if (memcard && !vflash_memcard_load(next, memcard))
        fprintf(stderr, "[Main] could not read memory card %s\n", memcard);
    vflash_init_audio(next);
    *audio = vflash_get_audio(next);
    printf("[Main] Disc: %s\n", disc_path);
    return 1;
}

int main(int argc, char **argv) {
    const char *disc_path = NULL;
    int debug    = 0;
    int headless = 0;
    int scale    = 2;
    int dbg_mode = 0;  /* 0=off, 1=paused at boot, 2=running with debugger */
    const char *program = NULL;
    const char *memcard = NULL;
    int quiet = 0;
    uint32_t load_addr = VFLASH_ADDR_NONE, entry = VFLASH_ADDR_NONE;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0)        debug    = 1;
        else if (strcmp(argv[i], "--headless") == 0) headless = 1;
        else if (strcmp(argv[i], "--language") == 0 && i+1 < argc) s_language = argv[++i];
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
        else if (strcmp(argv[i], "--quiet") == 0)   quiet = 1;
        else if (argv[i][0] != '-') disc_path = argv[i];
    }

    /* Only what the program writes to its UARTs on the console: the
     * emulator's own log, on stdout and stderr, goes to the null device. */
    if (quiet) {
#ifdef _WIN32
        const char *null_device = "NUL";
#else
        const char *null_device = "/dev/null";
#endif
        FILE *con = fdopen(dup(fileno(stdout)), "w");
        if (con && freopen(null_device, "w", stdout) && freopen(null_device, "w", stderr))
            vflash_set_uart_output(con);
    }

    /* The boot ROM beside the program too (70004.bin or flashem/70004.bin),
     * for a run started from a file manager in another working directory */
    char *base_dir = SDL_GetBasePath();
    if (base_dir)
        vflash_set_bios_dir(base_dir);

    /* No disc given: ask for one, unless the run is headless, under the
     * debugger or starts a program of its own */
    static char chosen[1024];
    if (!disc_path && !program && !headless && !dbg_mode && choose_disc(chosen, sizeof(chosen)))
        disc_path = chosen;

    /* Create emulator */
    VFlash *vf = vflash_create(disc_path);
    if (!vf) {
        if (!headless)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FlashEm",
                disc_path ? "The boot ROM 70004.bin was not found beside FlashEm, or the disc "
                            "image could not be opened."
                          : "The boot ROM 70004.bin was not found. Put it beside FlashEm.",
                NULL);
        return 1;
    }
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
    /* Gamepads are optional: without the subsystem the keyboard still works */
    if (!headless && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0)
        fprintf(stderr, "[SDL] No gamepad support: %s\n", SDL_GetError());
    if (!headless)
        input_config_load(base_dir);
    if (!s_language) s_language = input_config_language();
    if (!vflash_set_language(vf, s_language))
        fprintf(stderr, "[Main] unknown language \"%s\"\n", s_language);

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

    uint32_t hotkeys = 0, hotkeys_before = 0;
    while (running) {
        uint32_t buttons = 0;
        int turbo = !dbg_mode && (hotkeys & HOTKEY_TURBO);
        int accelerated = !dbg_mode && (vflash_fast_booting(vf) || turbo);
        audio_set_discard(audio, accelerated);

        /* Events */
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT && !headless) {
                printf("[Main] SDL_QUIT received, exiting\n");
                running = 0; break;
            }
            if (ev.type == SDL_DROPFILE) {
                if (!dbg_mode && !program && switch_disc(&vf, &audio, ev.drop.file, memcard))
                    frame_pacer_reset(&pacer);
                SDL_free(ev.drop.file);
            }
            input_handle_event(&ev);
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_F2:
                        if (dbg_mode) {
                            if (dbg_is_running()) dbg_pause(vf);
                            else                  dbg_resume();
                        }
                        break;
                }
            }
        }

        /* Input: the bindings of flashem.ini, keyboard and gamepads */
        hotkeys_before = hotkeys;
        hotkeys = 0;
        if (!headless)
            buttons = input_poll(SDL_GetKeyboardState(NULL), &hotkeys);
        {
            /* Hotkeys act when pressed, not while held (turbo aside) */
            const uint32_t pressed = hotkeys & ~hotkeys_before;
            if (pressed & HOTKEY_QUIT) {
                printf("[Main] Quit key pressed\n");
                running = 0;
            }
            if (pressed & HOTKEY_SCREENSHOT) {
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
            }
            if ((pressed & HOTKEY_OPEN_DISC) && !dbg_mode && !program &&
                choose_disc(chosen, sizeof(chosen)) &&
                switch_disc(&vf, &audio, chosen, memcard)) {
                frame_pacer_reset(&pacer);
                hotkeys = 0; /* the picker had the keyboard; nothing is held now */
            }
            if (pressed & HOTKEY_FULLSCREEN) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
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

        /* The console's LEDs, when they change */
        {
            static unsigned last_leds = ~0u;
            unsigned l = vflash_leds(vf);
            if (l != last_leds) {
                fprintf(stderr, "[LED] power %s, play %s\n",
                        (l & VFLASH_LED_POWER) ? "on" : "off", (l & VFLASH_LED_PLAY) ? "on" : "off");
                last_leds = l;
            }
        }

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
        /* Reduce rendering overhead during accelerated boot and turbo. */
        static unsigned boot_frames;
        if (!headless && win && !(accelerated && (boot_frames++ & 7))) {
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
    input_shutdown();
    if (base_dir) SDL_free(base_dir);
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
