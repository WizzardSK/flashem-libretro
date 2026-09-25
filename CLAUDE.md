# V.Flash Emulator — Current Task

## State (2026-09-25)
Real-BIOS boot of Spider-Man (UK) reaches the µMORE RTOS, but the scheduler never ticks: task 1 sleeps for 10 ticks and never wakes, tasks 2–10 never run.

## Root cause (found from a RAM dump at frame 300)
- RTOS task table at 0x10B0D204 (10 × $TCB, stride 0xB0). Tasks 2–10 still have their initial frame (savedSP = top−0x40, state +0x88 = 0).
- Task 1 (entry 0x109D4D30, prio 4) starts tasks via 0x10A14A1C, inits hardware in 0x10A5B0B8, then sleeps 10 ticks (0x10A0E408 → 0x10A0E2E8 → dispatch 0x10A1BF30); it would resume at 0x10A108F4.
- The kernel's own IRQ handler is **0x10A1BCF0** (exception table 0x109D41A0–41BC, copied to 0x1000FFA0+). It reads DC000024/28, dispatches via 0x10A111B0, restores DC00002C.
- Our HLE stub overwrites [0x1000FFB8] with 0x1200 / 0x10FFF200 (install_rtos_irq_chain and the per-frame "Ensure [0xFFB8]" block), so the kernel ISR never runs and the tick is never counted.

## What the kernel expects from hardware (Nspire/Firebird classic style)
- Tick timer = pair at 0x900C0000: +00=0x72D (value), +04=9 (divider), +08=1 (control), +0C=0xFFFF, +10=0x23E2, +14=2, +18/+1C=0, +30=2. Control bit 0x10 = stopped; compl = control & 7 → reload when value == completion[compl−1]; bit 8 = count up. See Firebird core/misc.c.
- Timer interrupt mask/status at 0x900A0030/34/38 (kernel writes 0x30=0x3F, 0x38=3).
- IC at 0xDC000000 is Firebird classic: enable mask 0x08 = 0x365346, disable 0x0C, current IRQ number at 0x24, priority limit 0x28, restore 0x2C, priorities 0x300+. IRQ lines: pair0 → 17, pair1 → 18, pair2 → 19.
- The emulator currently maps 0x900C0000 to SP804 timer[0] and models 0xDC000000 as a 6-source ZEVIO VIC — both wrong for this kernel.

## Next steps
1. Let [0x1000FFB8] keep the kernel's handler (or chain the HLE stub into it).
2. Implement the Firebird timer pairs at 0x90010000/0x900C0000 + status/mask at 0x900A0030/38.
3. Make 0xDC000000 follow Firebird classic semantics so the ISR gets IRQ 18.
4. Rerun headless, dump RAM, check task 1 wakes and tasks 2–10 leave their initial frames.

## Running
- Build + run inside `org.kde.Sdk//6.10` flatpak: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy VFLASH_WILD=1 ./flashem --headless '<game>.cue'`.
- `VFLASH_RAMDUMP=<path>` (+ `VFLASH_RAMDUMP_FRAME=N`, default 600) dumps 16 MB RAM (base 0x10000000) + `.regs`.
- `VFLASH_WILD=1` also prints the MMIO page histogram and `[HWW]` timer/IC writes with PC.
- Disassembler: `~/.cache/vflash-tools/dis <ram.bin> <VA_hex> [count]` (built from dis.c + src/disasm.c).
- `make clean` removes the helper tools; rebuild with `make all`.
