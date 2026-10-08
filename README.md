# FlashEm

FlashEm is an experimental emulator for the VTech V.Flash, also sold as the
V.Smile Pro, educational console (2006), as a libretro core. The repository also
has an SDL2 standalone frontend, kept as a development tool (debugger, headless
runs, screenshots); releases contain only the core.

The emulator runs the console's original boot ROM and each disc's own ARM code.
The ROM boots µMORE, mounts the disc through the emulated CD hardware, and loads
the game's `0SYSTEM/BOOT.BIN`. Graphics, audio, and movies come from the emulated
devices; the original movie-decoder firmware runs on a ZSP400 interpreter.
There is no high-level replacement for the operating system or games.

## Current status

Games now boot, menus respond to input, and 3D gameplay renders. The project is
still under active reverse engineering; these results do not establish complete
playability across every title, revision, or game mode.

- **Boot and disc access:** original ROM startup, game-kernel boot, CD servo and
  decoder, seeking, sector DMA, and streaming-ring protection are implemented.
- **Graphics:** tile and colour layers, textured sprites and triangles, skeletal
  animation, lighting, depth, blending, near-plane clipping, and surface copies
  are implemented in a software renderer. Dingo Rallye reaches its menus and
  renders the race, kart, and HUD. Other tested titles include Cars, Bratz,
  Spider-Man, The Incredibles, Disney Princess, Scooby-Doo, SpongeBob, Shrek,
  Wacky Race, and Multisports.
- **Movies:** native DSP firmware decodes MJP video into planar YUV, which the
  video engine composites with game graphics. Validation includes Multisports
  playback and movie samples from Cars, Spider-Man, SpongeBob, Wacky Race,
  Shrek, and Scooby-Doo, plus complete-movie and streaming-ring tests.
- **Audio:** BIOS sounds, PCM music, pitched/looped sample playback, Apple IMA4
  effects and movie streams, and the CDDA DMA path are implemented. Both
  frontends output 44.1 kHz stereo audio.
- **Frontends:** the libretro core has been run on Linux (x86_64 and aarch64)
  and Windows. Releases also carry a macOS arm64 build, and
  [.gitlab-ci.yml](.gitlab-ci.yml) builds the core with libretro's buildbot
  templates for Windows, Linux, macOS, Android and iOS; those builds compile,
  which does not imply equivalent runtime validation.

### Known limitations

- Compatibility testing consists of captured scenes and scripted routes, not
  complete playthroughs. Rendering and controller-register details remain
  partially inferred.
- CPU/device timing, DSP scheduling, and some CD behavior are approximate.
  Performance varies by scene and host; full speed is not guaranteed.
- Audio envelopes, finite-duration master-volume ramps, precise gain and
  interpolation, and sound interrupt generation remain incomplete.
- There are no save states or serialization-based features such as rewind,
  run-ahead, or netplay. The libretro core also has no reset implementation,
  disk swapping, cheats, core options, or persistent save-memory interface.
  Reload the content to restart it.
- The libretro core currently advertises 60 Hz/NTSC timing even though framebuffer
  dimensions follow the video-engine registers, including PAL-sized rasters.

## Download

The [latest release](https://github.com/WizzardSK/flashem-libretro/releases/latest)
has the libretro core for Linux (x86_64, aarch64), macOS (arm64) and Windows
(x86_64), and `flashem_libretro.info`. Put the core in your frontend's `cores`
directory and the info file in its `info` directory, then follow
[Required files](#required-files).

## Required files

Supply your own **`70004.bin` boot ROM (2 MiB)** and disc image. ROMs, games, and
extracted DSP firmware are not supplied. Normal gameplay loads the DSP firmware
from the game through the emulated hardware; no separate DSP file is needed.

The boot ROM is searched for in this order:

1. `<system>/flashem/70004.bin`, then `<system>/70004.bin`, when a libretro
   frontend supplies its system directory.
2. The path in the `FLASHEM_BIOS` environment variable.
3. `70004.bin` in the current working directory.

Use a **raw BIN image with its CUE sheet** and open the `.cue`. Keep the referenced
BIN beside the CUE. The loader reads tracks from a single-file CUE; multi-file
CUE layouts are not implemented. Extract ZIP archives before loading content.

The loader also accepts `.bin` and `.iso` paths, but a 2048-byte-sector ISO lacks
the raw EDC/ECC bytes used by the guest's software checks, and the emulator does
not reconstruct them. An ISO opening successfully does not mean it will boot.
Opening a BIN directly also loses the CUE's track metadata.

## Build

Run these commands from this repository's directory. The core needs a C
compiler with C11 atomics, GNU Make, and the math library, nothing else. Only the
standalone development frontend also needs SDL2 development files; normal
emulation does not require a host JPEG library.

The core alone, on any platform with GCC or Clang:

```sh
make -f Makefile.libretro -j
```

The sections below also build the standalone frontend and the tools.

### Linux / WSL

On Debian/Ubuntu:

```sh
sudo apt install build-essential libsdl2-dev
make -j
make -f Makefile.libretro platform=unix -j
```

`make` builds `flashem` and the command-line tools. The second command builds
`flashem_libretro.so`. To build only the standalone emulator, use `make flashem`.

### Windows with MSYS2 MinGW64

In an MSYS2 **MinGW64** shell:

```sh
pacman -S --needed make mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2
make platform=win SDL2_PREFIX= -j
make -f Makefile.libretro platform=win -j
```

The outputs are `flashem.exe`, the tools as `.exe` files, and
`flashem_libretro.dll`. For standalone use outside MSYS2, keep the matching
`SDL2.dll` beside `flashem.exe` (available in MSYS2's `mingw64/bin` directory).
The libretro core does not use that DLL.

### Cross-compile Windows builds from Linux / WSL

```sh
sudo apt install build-essential gcc-mingw-w64-x86-64 curl
bash tools/get_sdl2_mingw.sh
make platform=win -j
make -f Makefile.libretro platform=win -j
```

The SDL helper downloads the MinGW development package to `~/vfg/deps`, matching
the standalone Makefile's default. Set `SDL2_PREFIX` to the package's
`x86_64-w64-mingw32` directory to use a different location. This build copies
`SDL2.dll` alongside the standalone executable.

Standalone Linux and Windows objects use separate suffixes. Libretro objects
all use `.lr.o`, so run `make -f Makefile.libretro platform=<target> clean`
before switching core target platforms. Header dependencies are only partially
listed; use a clean rebuild after changing shared structures in headers.

## Standalone frontend (development)

The standalone frontend is for development - the debugger, headless runs and
scripted screenshots below - and is not published in releases. With `70004.bin`
in the current directory:

```sh
./flashem "path/to/game.cue"
./flashem --scale 3 "path/to/game.cue"
./flashem --dbg "path/to/game.cue"       # debugger paused at boot
./flashem --dbg-run "path/to/game.cue"   # debugger running
```

Windows PowerShell, with an explicit ROM path:

```powershell
$env:FLASHEM_BIOS = 'C:\path\to\70004.bin'
.\flashem.exe 'C:\path\to\game.cue'
```

`--scale` accepts 1–4 (default 2). `--help` lists the command-line options.
Accelerated boot is enabled by default and discards audio while active. Set
`VFLASH_FASTBOOT=0` to run startup at normal pacing. The debugger disables the
standalone frontend's accelerated pacing.

### Controls

| Keyboard | V.Flash | RetroPad (port 1) |
|----------|---------|------------------|
| Arrow keys | Joystick directions | D-pad |
| Z | Red | A |
| X | Yellow | B |
| C | Green | X |
| V | Blue | Y |
| Enter | Enter / OK | Start |
| Backspace | Exit | Select |
| Q / W | Left / right shoulder | L / R |
| Space | Stick button | L3 |
| H / B | Question / book | L2 / R2 |
| 1-4 | A-D | keyboard 1-4 |
| 5 / 6 / 7 | Console: play / stop / forward | keyboard 5 / 6 / 7 |
| 8 / 9 | Console: volume down / up | keyboard 8 / 9 |
| Home / End | Console: power on / off | keyboard Home / End |

The console's power and play LEDs go to RetroArch's LED driver (Settings >
LEDs) as LEDs 0 and 1 and to the log; the standalone frontend prints them when
they change.

With RetroArch's default keyboard mapping of the RetroPad, that is: arrow keys,
X for red, Z for yellow, S for green, A for blue, and Enter.

In the standalone frontend, F5 saves `screenshot.bmp`, F11 toggles fullscreen,
and Esc quits. F2 pauses/resumes when started with `--dbg` or `--dbg-run`.
Use a libretro frontend for host gamepad mapping. Some game prompts require a
colour button rather than Enter.

## libretro core

Load the built core in a libretro frontend, put `70004.bin` in its system
directory, and load the game's `.cue`. The accompanying
`flashem_libretro.info` provides frontend metadata.

The core opens content by filesystem path (`need_fullpath`) and requires a disc
path. It supplies software-rendered XRGB8888 video at a 4:3 display aspect and
44.1 kHz stereo audio. Framebuffer dimensions follow the guest's video settings,
up to 512×288; games commonly use 512×240. Input currently comes from the first
RetroPad's D-pad and buttons, as mapped above.

## Hardware and source layout

The V.Flash uses an LSI Logic ZEVIO 1020 with an ARM926EJ-S CPU at 150 MHz,
16 MiB SDRAM, and CD-ROM media. Its boot ROM and games run the µMORE RTOS.

| Source | Responsibility |
|--------|----------------|
| [src/arm9.c](src/arm9.c), [src/cp15.c](src/cp15.c) | ARM/Thumb interpreter and MMU |
| [src/hw.c](src/hw.c) | Memory map, timers, interrupts, GPIO, UART/controller reports, CD decoder, and video scanout |
| [src/cdrom.c](src/cdrom.c), [src/cdsp.c](src/cdsp.c) | Disc images, track/subcode data, and CD servo model |
| [src/ge.c](src/ge.c) | Command-list graphics engine and software rasterizer |
| [src/zevio_dsp.c](src/zevio_dsp.c), [src/zsp400.c](src/zsp400.c) | DSP DMA/mailboxes and ZSP400 instruction execution |
| [src/midi.c](src/midi.c), [src/cdda_dma.c](src/cdda_dma.c) | Sound voices, PCM/IMA4 playback, and CDDA transfers |
| [src/audio.c](src/audio.c) | Audio buffering and frontend output |
| [src/vflash.c](src/vflash.c) | Frontend-facing machine API and ROM lookup |
| [src/main.c](src/main.c), [src/flashem_libretro.c](src/flashem_libretro.c) | SDL2 and libretro frontends |

The graphics command engine is at `0xA8000000`; display composition is handled
by the video engine at `0xB8000000`. Other major devices include sound at
`0xB0000000`, memory DMA at `0xBC000000`, DSP DMA at `0xC0000000`, and the CD
decoder/servo interface at `0xC4000000`.

Reverse-engineering evidence and unresolved register behavior are documented
next to the device models. [CLAUDE.md](CLAUDE.md) contains more detailed working
notes, including historical findings and references to local research fixtures.

## Diagnostics and development

For a bounded headless run on Linux/WSL:

```sh
FLASHEM_BIOS=/path/to/70004.bin \
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
VFLASH_EXIT=3601 VFLASH_SHOT_FRAME=1800,3600 \
VFLASH_SHOT='frame-%d.ppm' \
./flashem --headless /path/to/game.cue > run.log 2>&1
```

`--headless` skips the window but still initializes SDL video/audio; the dummy
drivers allow runs without display or audio devices. Choose a writable screenshot
path explicitly, especially on Windows. Headless runs continue until an exit
condition is supplied.

| Variable | Effect |
|----------|--------|
| `VFLASH_LOG=<file>` | Redirect standalone stdout after initialization; shell redirection also captures startup output and stderr |
| `VFLASH_EXIT=N` | End the process at the specified emulated frame |
| `VFLASH_SHOT_FRAME=N[,N...]` | Headless screenshot frames in ascending order; default 50 |
| `VFLASH_SHOT=<path>` | Headless PPM path; `%d` inserts the frame number; default `/tmp/vflash_screen.ppm` |
| `VFLASH_FASTBOOT=0` | Disable accelerated boot |
| `VFLASH_RTC=<unix-seconds>` | Fix the RTC's starting time for reproducible runs |
| `VFLASH_INPUT=<hex>@<from>-<to>[;...]` | Script frontend button masks; e.g. `100@3100-3110` presses Enter |
| `VFLASH_KEYS=<hex>@<from>-<to>` | Force raw key-register bits, distinct from controller reports |
| `VFLASH_RAMDUMP=<file>` | Dump SDRAM and palette RAM (`<file>.sram`) at `VFLASH_RAMDUMP_FRAME` (default 600) |
| `VFLASH_TRACEPC=<hex>[:<hex>...][,N]` | Trace registers at selected instruction addresses, with an optional hit limit |
| `VFLASH_IOHIST=<from>,<to>[,page]` | Collect MMIO access counts by address and PC |
| `VFLASH_CDLOG=1` | Trace CD servo/decoder activity |
| `VFLASH_GELOG=1` | Report unhandled graphics commands |
| `VFLASH_GECAP=<file>,<frame>[,<frames>]` | Capture graphics state, RAM, and command-list inputs for replay |

`VFLASH_INPUT` uses the button masks in [src/vflash.h](src/vflash.h): directions
`01/02/04/08`, red/yellow/green/blue `10/20/40/80`, and Enter `100` (hex).
Other device-specific tracing switches are documented in the source.

### Debugger

Start with `--dbg` or `--dbg-run`, then enter commands in the terminal:

```text
s [N]        step N instructions     c            continue
n            step over              b <addr>     set breakpoint
bc <a>|all   clear breakpoint(s)     bl           list breakpoints
r            registers              pc           current instruction
d <a> [N]    disassemble             m <a> [N]    memory dump
bt           stack dump             setreg r0=1  write register
q            quit
```

### Tools and tests

The default standalone build includes disc/asset utilities and a graphics replay
tool:

```sh
./disc_analyze game.iso
./disc_compare first.iso second.iso
./mjp_extract game.iso frames/
./ptx_extract game.iso images/
./gereplay capture.bin output.ppm
```

The disc/asset utilities above operate on ISO payload images; they do not share
the emulator's full CUE/BIN loader. MJP extraction is a research utility, separate
from native DSP playback. `testrom_gen` is also built; `make testrom.bin` generates
its synthetic test ROM.

Focused C tests live in [tools/](tools/), covering DSP instructions and DMA,
firmware integration, video composition, graphics depth, CD streaming guards,
PCM/IMA4 audio, volume, and frontend buffering/pacing. There is no unified
`make test` target. Some tests need user-supplied firmware, movie files, captured
RAM, or independent decoded references; read each test's source for its inputs.

For a small test without proprietary fixtures:

```sh
cc -std=c11 -O2 tools/test_zsp400.c src/zsp400.c -o /tmp/test_zsp400
/tmp/test_zsp400
```

To check a Linux libretro build without a full frontend:

```sh
cc tools/retro_smoke.c -Isrc -o /tmp/retro_smoke -ldl
/tmp/retro_smoke ./flashem_libretro.so /path/to/game.cue /path/to/system 600 /tmp/retro.ppm
```

`retro_smoke` also supports Windows DLLs. The audit/capture shell scripts in
`tools/` are research helpers with local paths and fixture assumptions; adapt
them before use. Graphics captures depend on the current `ge.h` layout and must
be recreated when that layout changes.

## License

MIT — see [LICENSE](LICENSE). `src/libretro.h` comes from the libretro project
and retains its own MIT notice.
