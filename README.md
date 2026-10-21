# FlashEm

FlashEm is an experimental emulator for the VTech V.Flash, also sold as the
V.Smile Pro, educational console (2006), as a libretro core and as a standalone
program (SDL2), which also serves as the development tool (debugger, headless
runs, screenshots).

The emulator runs the console's original boot ROM and each disc's own ARM code.
The ROM boots µMORE, mounts the disc through the emulated CD hardware, and loads
the game's `0SYSTEM/BOOT.BIN`. Graphics, audio, and movies come from the emulated
devices; the original movie-decoder firmware runs on a ZSP400 interpreter.
On 64-bit hosts (the libretro cores for Linux, Windows x64, macOS arm64 and
Android arm64-v8a/x86_64, and the standalone emulator) the ARM9 runs on the
dynarmic JIT recompiler, with the interpreter for what it gives back (mode
changes, CP15, exceptions); the core option CPU Emulation, or `--cpu` and
`flashem.ini`'s `cpu` in the standalone, switches back to the interpreter.
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
  disk swapping, cheats, or persistent save-memory interface; its core
  options are Boot ROM, Console Language and CPU Emulation (JIT or Interpreter).
  Reload the content to restart it.
- The libretro core currently advertises 60 Hz/NTSC timing even though framebuffer
  dimensions follow the video-engine registers, including PAL-sized rasters.

## Download

The [latest release](https://github.com/WizzardSK/flashem-libretro/releases/latest)
has the libretro core for Linux (x86_64, aarch64), macOS (arm64), Windows
(x86_64) and Android (arm64-v8a, armeabi-v7a, x86_64, x86; name the one for
your device `flashem_libretro_android.so`), and `flashem_libretro.info`. Put the core in your frontend's `cores`
directory and the info file in its `info` directory, then follow
[Required files](#required-files).

It also has the [standalone emulator](#standalone-emulator) for Linux (x86_64,
aarch64, `.tar.gz`), macOS (arm64, `.tar.gz`, with SDL2) and Windows (x86_64,
`.zip`, with SDL2). On Linux it uses the system's SDL2 (`libsdl2-2.0-0` on
Debian and Ubuntu).

## Required files

Supply your own **boot ROM (2 MiB)** and disc image. Two are known:
`70004.bin`, the German console's (md5 `f19e090723accb3353a7c4bef342af71`, menu
in German), and `vflash_us.bin`, the US console's (md5
`60b1ef08a7f3a795d66f2cf41140901b`, menu in English; a dump of the console's
A29L160ATV NOR flash - rename it). ROMs, games, and
extracted DSP firmware are not supplied. Normal gameplay loads the DSP firmware
from the game through the emulated hardware; no separate DSP file is needed.

The boot ROM is searched for in this order, `70004.bin` first and then
`vflash_us.bin` at each step, or only the one chosen with the core option Boot
ROM (`--boot-rom` or `flashem.ini`'s `boot_rom` in the standalone):

1. `<system>/flashem/<name>`, then `<system>/<name>`, when a libretro
   frontend supplies its system directory; for the standalone emulator, the
   directory the program is in.
2. The path in the `FLASHEM_BIOS` environment variable.
3. `<name>` in the current working directory.

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

## Standalone emulator

Put the boot ROM (`70004.bin` or `vflash_us.bin`) beside `flashem` (`flashem.exe`) and start it. With no disc on
the command line it asks for one with the system's file picker (on Linux zenity
or kdialog; without either, give the disc on the command line); cancelled, the
console starts into its menu with the drive empty. A disc image dropped on the
window, or one chosen with O, restarts the console with that disc, as when a
disc is changed with the power off.

The same program is the development frontend - the debugger, headless runs and
scripted screenshots below:

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

In the standalone emulator, Tab runs it unthrottled while held (turbo, without
sound), O opens another disc, F5 saves `screenshot.bmp`, F11 toggles fullscreen,
and Esc quits. F2 pauses/resumes when started with `--dbg` or `--dbg-run`.
Some game prompts require a colour button rather than Enter.

The standalone emulator also takes gamepads (SDL GameController), laid out as the
RetroPad column above, with the left stick as the V.Flash stick too and R3 as
turbo. Keys and gamepad buttons, hotkeys included, are rebound in `flashem.ini`
beside the program, which it writes with the defaults on its first start; delete
it to get them back. Its `[console]` section sets the console's `language`, the
`boot_rom` and the `cpu` (`jit` or `interpreter`), as `--language`, `--boot-rom`
and `--cpu` do for one run. The debugger always runs the interpreter.

## libretro core

Load the built core in a libretro frontend, put the boot ROM (`70004.bin` or
`vflash_us.bin`) in its system directory, and load the game's `.cue`. The accompanying
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
| [src/jit.cpp](src/jit.cpp), [tools/dynarmic.patch](tools/dynarmic.patch) | The ARM9 on dynarmic (`external/dynarmic`, built by `tools/build_dynarmic.sh`, `make -f Makefile.libretro JIT=1`) |
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
| `VFLASH_JIT=0` | Run the ARM9 on the interpreter (as the core option or `--cpu interpreter`) |
| `VFLASH_RTC=<unix-seconds>` | Fix the RTC's starting time for reproducible runs |
| `VFLASH_INPUT=<hex>@<from>-<to>[;...]` | Script frontend button masks; e.g. `100@3100-3110` presses Enter |
| `VFLASH_KEYS=<hex>@<from>-<to>` | Force raw key-register bits, distinct from controller reports |
| `VFLASH_RAMDUMP=<file>` | Dump SDRAM and palette RAM (`<file>.sram`) at `VFLASH_RAMDUMP_FRAME` (default 600) |
| `VFLASH_TRACEPC=<hex>[:<hex>...][,N]` | Trace registers at selected instruction addresses, with an optional hit limit |
| `VFLASH_IOHIST=<from>,<to>[,page]` | Collect MMIO access counts by address and PC |
| `VFLASH_CDLOG=1` | Trace CD servo/decoder activity |
| `VFLASH_UART0_LINK=<path>` / `VFLASH_UART1_LINK=<path>` | Tie that UART to a pseudo-terminal on the host, raw both ways, and make a link to it at `<path>` (`1`: no link, the name is only printed). Not on Windows |
| `VFLASH_GELOG=1` | Report unhandled graphics commands |
| `VFLASH_GECAP=<file>,<frame>[,<frames>]` | Capture graphics state, RAM, and command-list inputs for replay |

`VFLASH_INPUT` uses the button masks in [src/vflash.h](src/vflash.h): directions
`01/02/04/08`, red/yellow/green/blue `10/20/40/80`, and Enter `100` (hex).
Other device-specific tracing switches are documented in the source.

### Serial link

A program that talks to a host over the console's UART - a debug stub, a monitor - can be reached from the host as if over a serial cable. With `VFLASH_UART0_LINK=/tmp/vflash-uart0` (or `VFLASH_UART1_LINK` for the second UART) the emulator makes a pseudo-terminal, ties the UART to it and links it at that path; the terminal's own name (`/dev/pts/N`, different every run) is printed as `[UART0] link: ...`. Point the host tool at the path where it would take `/dev/ttyUSB0`:

```sh
VFLASH_UART0_LINK=/tmp/vflash-uart0 ./flashem --load program.elf &
picocom /tmp/vflash-uart0
```

The link is raw: every byte the program sends goes out, every byte that comes in is received, and the baud rate set on either side is ignored. Bytes from the host wait in the terminal until the UART has room, so a program that polls loses none. The controller, which normally sits on UART0, is disconnected from a linked UART. The lines the program prints are still logged as without the link. Linux and macOS only.

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
