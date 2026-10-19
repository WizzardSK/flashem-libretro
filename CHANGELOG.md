# Changelog

What changed in FlashEm, newest first. Releases are builds of the libretro core and, from v0.3, of the standalone emulator too.

## Unreleased

- The console's language can be chosen: English (US), English (UK), French, Spanish, German or Dutch, kept in the console's system flash as on the real hardware. Core option Console Language, `[console] language` in `flashem.ini`, or `--language` (SatellaGuyAlt, #5).
- Standalone: turbo (Tab held, R3 on a gamepad) runs the emulator unthrottled, without sound (SatellaGuyAlt, #5).
- Standalone: gamepads, laid out as the libretro core's RetroPad, and every key and gamepad binding, hotkeys included, configurable in `flashem.ini` beside the program, written with the defaults on the first start (#5).

## v0.3 - 2026-10-18

- The standalone emulator is in the releases, for Linux (x86_64, aarch64), macOS (arm64) and Windows (x86_64), with SDL2 beside it on macOS and Windows (SatellaGuyAlt, #4).
- Standalone: with no disc on the command line it asks for one with the system's file picker; a disc dropped on the window, or chosen with O, restarts the console with that disc. The boot ROM is also looked for beside the program, and a missing one is reported in a message box rather than only on the console (#4).
- The standalone builds again with GCC 14 on Linux: the serial link's pseudo-terminal functions were used without their declarations being asked for, which older compilers only warned about (mrdudz, #3).

## v0.2 - 2026-10-11

Mostly what running homebrew needed, worked out with mrdudz (#3): programs built with vtech-lib, DooM and Noiz2sa among them, now start, show their picture, take the controller and play sound.

### The machine (core and standalone)

- Sound: the sound core's timer raises its interrupt (IRQ 9), and plain PCM plays at any rate and pitch. One timer tick is one sample at 11025 Hz, measured on DooM: at the earlier 1 ms a tick its sound ran dry 473 times in 1400 frames, now never.
- Video: colour layer 1 is shown as a 16-bit RGB surface when the program selects that, besides the planar movie format the games use.
- Controller: every button of the pad is reported, the colour buttons also as its d-pad, and the controller answers vtech-lib's driver whether it uses interrupts or polls.
- The console's own buttons and LEDs are modelled.
- Memory card: the AT45DB642 DataFlash on the SPI bus, and its mirror at 0xA0000000.
- PMU: the clock-change done bit rises a little after the write, as programs expect.
- No host pointer is kept for the page at 0xFFFFF000.

### Standalone frontend

- `--load` starts an ELF, a BOOT.BIN or a raw binary straight from RAM, instead of the boot ROM; `--entry` picks the start address.
- `--memcard` inserts a memory card image, written back on exit when the card was written to.
- Without a disc the console starts into its own menu.
- `--quiet` shows only what the program writes to its UARTs.
- Serial link: `VFLASH_UART0_LINK` / `VFLASH_UART1_LINK` tie a UART to a pseudo-terminal on the host, for a debugger or monitor that would use a serial cable. Linux and macOS. New and not yet tried with a real tool.

### Builds

- Android builds of the core in the release: arm64-v8a, armeabi-v7a, x86_64 and x86. Built, not yet run on a device.
- The core info file is in libretro's full layout, with the controls and the disc image advice in its notes.
- The release's notes carry the newest section of this changelog.
- The Makefiles generate header dependencies, so a changed header rebuilds what includes it.

## v0.1 - 2026-10-03

The first release. An emulator of the VTech V.Flash (V.Smile Pro) as a model of the real hardware - the device-model rewrite by deadman96385 (CD servo and decoder, the graphics command-list engine, the ZSP400 DSP running the discs' own movie firmware) in place of the earlier high-level frontend.

- The real boot ROM starts µMORE, mounts the disc over the emulated CD hardware and starts the game's own kernel.
- Games reach their menus and 3D gameplay renders, with movies and sound.
- The libretro core for Linux (x86_64, aarch64), macOS (Apple Silicon) and Windows, taking `.cue`, `.bin` and `.iso`.
- No save states: the emulator has no serialiser yet.

## Before the first release

- September 2026: the hardware-only rewrite; the first games boot from the real ROM, Dingo Rallye reaches its menu and then its race.
- March to May 2026: the first emulator, a high-level one, and the tools for the discs' formats.
