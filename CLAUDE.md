# V.Flash Emulator — Current Task

## State (2026-09-26)
The emulator is hardware-only: src/hw.c is the machine (device models behind a physical
memory map), src/vflash.c the frontend API around it. The real ROM boots µMORE; with a
disc image loaded it now **mounts the disc, reads \0SYSTEM\BOOT.BIN over the emulated CD
hardware, and starts the game's own µMORE kernel** (it prints its banner on UART0).
The game then builds command lists for the 0xA8000000 engine (src/ge.c), which draws
into a 512x240 framebuffer shown by the video engine. **As of 2026-09-26 Dingo Rallye
reaches its main menu, rendered correctly, and the joystick navigates it** (up/down,
OK opens sub-menus and Options). Since 2026-09-27 the race itself renders (3D world,
kart, HUD), and 10 other discs load and reach their menus (tools/audit.sh). The
audit's open items (transparency, Cars' white screens, Bratz desync, race ground) are fixed.

## Boot flow
- GPIO B0 holds the wake sources (read at 0x100113FC): bit 0 power key -> system menu,
  bit 2 disc wake -> disc mount (task 9, 0x100115F0 -> 0x1001428C) and BOOT.BIN. Both are
  momentary; with a disc loaded hw.c raises bit 2 for 120 frames. The ROM only jumps into
  a loaded BOOT.BIN (0x100126B8) after bit 2 drops. `VFLASH_GPIOB=1` forces the menu.
- BOOT.BIN header: "BOOT", size, load 0x10C00000, entry via `ldr pc` at +0x10.
- Lid switch = GPIO A1 bit 6 (high = open, `VFLASH_LID=a-b`); A1 bit 2 a second switch.

## CD drive (src/cdsp.c + decoder in hw.c) - works end to end
- Servo DSP: Sony CXD2545-family command set (datasheet text in WSL ~/ds/cxd2545.txt).
  SENS mux by the last command byte ($30 SSTP, $38 AGOK, $40 XBUSY, $50 FOK, $A0 GFS).
  $7 track count is bits 19-4; the ROM writes half the distance for a 2N jump. $39xx
  readback: $391F RFDC must read 0x9E-0xA2 (gain calibration window).
- Disc model: CLV spiral (1.3 m/s, 1.6 µm), program area from 25 mm, lead-in from
  23 mm, innermost switch at 22.5 mm; TOC and subcode Q from the cue sheet.
- Decoder (0xC4000000, IRQ 14): subcode Q +0x54/58/5C (int bit 5); header MSF +0x4C;
  target MSF +0x1C (bit 24 any, advances with each sector); length +0x20; +0x24
  header dest / live DMA address; data ring +0x28/+0x2C; 4 x 1 KB slot descriptors at
  +0x3C (status, data ptr, header at +0x14, raw EDC/ECC bytes 0x810-0x92F at +0x18 for
  the kernel's software ECC, C2 flags at +0x138); +0x40 bit 2 run, bits 3-4 mode;
  +0x70/+0x74 slot count/release; int bit 2 target found, bit 4 transfer ended (the
  driver relies on it after a CPU stop), bit 12 slot filled, bit 16 DMA watermark +0x80;
  +0x84 bit 0 ends data DMA at the watermark with the normal transfer-end status.
  Cars' mode-2 streaming driver uses this guard one sector behind its read pointer;
  ignoring it overwrites unread compressed movie sectors. With bit 0 clear, the
  watermark only notifies. `tools/test_cd_ring_guard.c` covers guard/wrap/restart.
  +0x60 timers (10 ms units: bits 23-20 lost-signal watchdog int 14, 31-24 timeout
  int 15).
- Traverse counter (COMP, $B0 SENS) counts tracks crossed by plain sled moves too: 2006
  game kernels seek by kicking the sled ($22) and polling COMP, not by auto sequences.
  Without it they ran to the disc edge and stalled at "Loading" (7 of 10 audited games).
- DMA controller 0xBC000000 (IRQ 12): memory-to-memory copies out of the CD ring.

## Graphics engine (src/ge.c) - Dingo Rallye's menus render correctly
- 0xA8000000: +0x28 list address, +0x00 = 1 starts, IRQ 8 with +0x14 = 0xFD08 at the
  end. `VFLASH_LCDLOG=1` dumps lists, `VFLASH_GELOG=1` reports unknown opcodes,
  `VFLASH_GETRACE=<frame>` traces commands, `VFLASH_SURFLOG=1` logs surface changes,
  `VFLASH_GEWATCH=x,y` reports every write to one surface pixel (fill/sprite/triangle).
- Surface: 1024 px wide, 16 bpp BGR555, 8x8 tiles of 128 bytes (16 KB per tile row).
  A8 +0x98 is the address of row (+0x90 >> 16), which is also the surface's top edge
  (kernel: row 240 = 0x10048000; Dingo Rallye: row 128 = 0x10010000 - the same layout).
  GE.surface = where row 0 would be. Textures/palettes live below y = 720 (to ~1820).
- Fills are clipped by 0x82/0x83. (The (0,0) 512x240 fill under a clip that excludes it
  is the depth clear, see below; unclipped it overwrote the ROM kernel's RAM.)
- Texels: transparent = bit 15 set or 0x0000 (not "palette index 0").
- Sprites 0xC8, 3D (0x09 calls, 0x0A/0x40/0x44/0x67/0x6F vertex records) as in ge.c:
  0x40 strip, 0x44 triangle list, 0x6F / 0x67 skinning stores (below). Unhandled, 0x67
  sent the GE on through vertex data at 103Cxxxx - Multisports' ~60 "unknown ops".
- Numbers in vertices/registers are the engine's 16-bit float (s:1 e:6 bias 31 m:9,
  ge_f16_to), NOT IEEE half and NOT 4.12: the game kernel decodes it in software at
  0x10A6E658 (Disney Princess). 1.0 = 0x3E00. UVs are 0..1 over the whole texture.
- Prim flags byte bit 6 (0x40780005 vs 0x40380005): record holds 16-bit indices into
  the 0x0A buffer instead of inline vertices - for 0x44 too (Spider-Man's 0x4438 is
  inline; treating every 0x44 as indexed ran into its vertices: ops 2C/3D).
  0x0C000000 a: vertex buffer for the next model call / skinning store (below).
- 0x67 records and flags bit 7 (every 0x6F seen is 0x6FB8) = SKINNING (Multisports stores
  with 0x67380006, 6 vertices a bone, 0x0C destinations 0x78 apart; drawn as triangles
  they were the "black spike" and a missing player figure): the record is not drawn; each vertex goes
  through the model stage (scale 3C-3E, matrix 40-48, translation 4C-4E; normal by the
  matrix) and is stored as a 5-word 0x38 vertex at the 0x0C buffer (i-th at +20 i). The
  game does one 0x0C + store per bone into 0x10D44060.., then draws the body with indexed
  0x4078 records over that buffer and identity model registers. Nothing else writes the
  buffer. This is what drew Spider-Man, Mr. Incredible, the Bratz fairies and the Dingo
  driver (and the old "-0/-2 stripes" in Bratz were the unwritten buffer).
- Vertex layout follows the prim flags byte, in this order: x|y, z|nx, then ny|nz
  (bit 5 = normal), u|v (bit 4), colour (bit 3). 0x38 = 5 words, Bratz's 0x10 = 3,
  Spider-Man's water 0x18 = uv + colour, 0x28 = normal + colour. (Until 2026-09-27 bits
  3/5 were swapped, which only 0x38/0x10 hid: Spider-Man's sea read colour 0x6F7B as a
  UV and came out as cyan speckle.)
- Texture format (0x86) bits 0-2/3-5 log2 w/h - 3, bits 9-11 depth (0 = 16, 1 = 8,
  2 = 4 bpp), bit 8 = 4444 colours (alpha in the top nibble, red in the low nibble)
  instead of BGR555 + bit-15 transparency: Princess's text, Dingo's blob shadow.
  0x88 bit 6 is NOT an alpha switch: 0x40 is simply the normal state on Multisports /
  Bratz (0x42 = blending on; Princess / Dingo use 0x02). Reading the nibble as
  transparency under it (tried 2026-09-27) fixed Multisports' menu but made Bratz's
  parchment menu, clothes display and figures see-through - reverted.
- SPRITE DEPTH: a sprite's attribute word (f16, after the header) is its depth as 1/z
  in the same buffer as the 3D. Every sprite pixel writes z = 1/attr; with 0x1E bit 15
  on it is only drawn if not farther. This (not alpha) is why Multisports' glass bars
  (attr 170.5) stay behind the labels drawn before them (512 / 170.5); it also keeps
  Cars' timer (25.1, drawn with depth off) in front of the checkered banner at z 0.98,
  un-dims the Cars help text and shows Bratz's "Confirm" button - all three as in the
  real-hardware videos (Desktop/vflashy/refvideos). VFLASH_SPRD=0 disables.
- The depth buffer starts far (1e30): zero-initialised, it hid the Bratz room's girl and
  clothes display until the first depth clear (replays / first frames).
- DESTINATION ALPHA: every sprite/triangle pixel leaves in abuf (ge.c, beside zbuf) the
  alpha it drew with if the texture was 4444, else 0; fills clear it. Under 0x88 = 0x42
  (blending + bit 6) a sprite with a non-4444 texture takes its alpha from abuf, not the
  texel. Bratz's menu: white 4444 swirls, then a grey gradient "glint" sprite over the
  same 128x128 - on hardware the glint shows only inside the swirls (video: coloured
  swirl interiors cycling gold -> purple). Not in the surface's bit 15: that is the video
  engine's transparency. VFLASH_DSTA=0 disables. Only Bratz changes in any capture.
- SPRITE COLOUR WORD (flag bit 2, after the uv word): 00BBGGRR tint on the texels.
  Normally 00FFFFFF; Bratz's glint animates it (0036B9FA gold ... 005F9DFA) - the swirl
  colours. The top byte (FF on Bratz's swirl sprites) is not understood.
- D1000002 y|x h|w: read a surface rectangle back into RAM at the 0x0C address (2 pixels
  a word, rows) - the inverse of D0/0E. Bratz renders a 128x128 sprite off-screen, D1s it
  to 0x10E39A2C, then D0/0E uploads it from there: render-to-texture through RAM; 0x89
  alone follows (texture-cache sync, no effect).
- D2000003 dst y|x, h|w, src y|x: copy a surface rectangle (destination FIRST). D6 has the
  same words and keeps the destination where the source is transparent. Bratz's try-on
  room builds a top's texture: D2 (0,2272) <- (0,1504) (the chosen colour swatch), then
  D6 (192,2272) <- (576,2400) 128x64 (skin). Read source-first, D2 put the "Dummy
  texture" placeholder over the swatch: every top was mint (#7). VFLASH_D2=0 = old order.
- 0x10000000 a: two words, once a frame after the depth clear, a = RAM address never read
  by the CPU (readback/fence). Was executed as a command ("unknown op 10" at 10Bxxxxx).
- 0x1C render mode (default 006760): bits 15-23 a per-object counter on world models
  (with 1E bit 2), bit 3 with it, bit 6 clear on SpongeBob's blender glass, bits 6+9 clear
  on Dingo's title sky. Not culling (VFLASH_CULL=2 tried: deletes skies drawn under 6760);
  stored, no effect. Strips now pass alternate triangles in consistent winding.
- A list whose PC leaves RAM ends (Bratz's particle lists end 0x08 a 0).
- Blending: 0x88 m = 2 turns it on, 0x8D kkkkkk = factor per channel (Cars fades its logos
  with a white full-screen sprite under 8D000000..FFFFFF); texture alpha applies on top.
- Triangles are bilinear filtered (VFLASH_FILTER=0 = point sampling); real hardware
  unconfirmed - format bit 17 is set on most textures, clear on Dingo's sky.
- Depth: 0x1E bit 15 = depth test/write on (sky dome drawn with it off). 0x85 ... 0x96
  around a 0xD3 fill = depth clear over the viewport (fill rect relative to clip origin).
  The SDK also submits viewport-local depth rectangles without 0x85. A rectangle
  fitting the local viewport but disjoint from its absolute colour clip updates
  depth, using the fill's f16 inverse-depth value instead of always clearing far.
  SpongeBob uses depth 256 to keep its movie's transparent opening in front of
  a background sprite at about 205. See tools/test_ge_movie_depth.c and
  ../dsp-research/OTHER-MOVIES.md. The physical depth-buffer layout remains inferred.
- Near plane = register 0x74 (0x75 far). Triangles crossing it are CLIPPED (default since
  2026-09-27; VFLASH_CLIP=0 rejects them, VFLASH_CLIPLOG=1 logs). Rejecting lost
  SpongeBob's loading-screen counter top and Cars' last road rows. It used to be the
  default because clipping drew junk (Bratz stripes, a Dingo menu wedge, Shrek title
  particles) - those came from the since-fixed skinning / vertex-layout / 0x67 bugs and
  do not reproduce. The "oversized / clips a lot" 3D was the float format.
- Renderer state (Dingo kernel setters 0x10A7A6xx-0x10A7AB6C, emitters 0x10A34C58-
  0x10A34E58): 0x84 flags (default 0x17: bits 0|1 lighting, bit 3 fog on, bit 5 fog mode,
  bits 2/4 unknown two-bool setter), 0x8A fog colour, 0x8B/0x8C fog start/end (floats),
  regs 0x60/0x64/0x68 light 0-2 direction, 0x6C-0x6E light colours, 0x70 ambient.
  Lighting is per-vertex from normals (vertex flag bit 5), VFLASH_NOLIGHT=1 disables.
  No captured frame enables fog yet (untested). 0x95 = sync after each sprite/upload.
  D0000002 y|x h|w + 0E00nnnn src = upload n words of pixels into that rectangle
  (0x10A333B4); not seen in the audited frames.
- Open: 0x1E/0x1C other bits, 0x84 bits 2/4, regs 0x71/0x72, 0x88 bits other than 1/6,
  ops 0x12 (opens strips of Spider-Man's sea / Dingo's track, no visible effect) and the 0x1C flag bits: depth-write gating by bits 6 / 9 changes nothing in any capture, bit 8 345 px in Bratz - no hardware defect to decode them against yet.
- `VFLASH_GETRACE` prints, after each 0x09 call, its triangles, how many were behind /
  crossing the near plane and their screen box - the quickest way to find a missing model.
  `VFLASH_WATCH` also reports DMAC copies into the range; unknown-op logs name the list
  and the previous command.

### Fast GE iteration (no boot needed)
- Capture once (about 2 min): `VFLASH_GECAP=<file>,<frame>[,<frames>]` records the GE
  state + RAM, then the pages the CPU changes before each list (not what the GE drew).
- `make gereplay`; `./gereplay cap out.ppm [x y w h]` reruns the lists through the
  current ge.c in ~0.05 s. `GEREPLAY_LISTS=a-b`, `GEREPLAY_RAM=<file>` (RAM at the end),
  plus VFLASH_GETRACE/GELOG/GEWATCH/NO3D/NODEPTH. `tools/tex.py <ram> out.png tx ty w
  h [px py bpp]` decodes a texture. A ge.h struct change invalidates captures.
- Menu capture: Dingo Rallye, `VFLASH_INPUT="100@3100-3110"`, frames 3640-3652.

## Controller (hw.c pad_report6)
- The game reads the 6-byte report (port 0 decoder 0x10A6B694; consumer 0x10B49150):
  two signed 8-bit axes centred on 0 - field 1 (struct +6) vertical, up positive;
  field 2 (+5) horizontal (sign a guess). Button bit 2 = OK. The colour buttons are
  in this report too: bit 11 yellow, bit 12 green (Multisports' event briefing takes
  "Start" = green / "Anleitung" = yellow only from here - found by trying each bit;
  without them it sat on the briefing forever); red 10 / blue 13 assumed from the pad's
  button order. They are also sent as n = 2 events (codes 1-4). `VFLASH_STICK=xx,yy@a-b` forces raw axes,
  `VFLASH_PADBITS` raw buttons. The 4-byte report (12-bit axes with min/max
  calibration, ISR at 0x10C95B4C) is not what the menus use.
- `VFLASH_RWATCH=addr,len[,from]` lists the PCs that read a range. TRACEPC/RWATCH
  PCs are instruction address + 8.

## Also open
- 0xA4000000 math unit (game kernels): write an operand, read the result back.
  +0x00 IEEE float -> f16, +0x04 f16 -> float, +0x18 20.12 -> f16 (0x10A6E6B0),
  +0x1C f16 -> 20.12. +0x08/0x28/0x2C (32->16) and +0x0C/0x20/0x24 (16->32) are wrappers
  at 0x10A081F0 in Princess, formats unknown (echo). `VFLASH_HALF=1` = the old half/echo
  model. The echo turned 1.0 into 0 and Princess/Incredibles hit ASSERT fix.inl(78) on 1/0.
- 0xA0000000: optional external/SPI flash window (magic "New_Flash_Driver",
  "VFLASHQA" test image); retail units have none - modelled as erased (0xFFFFFFFF).
- 0xB0000000 is the separate MIDI sound core, now in src/midi.c. BIOS UI PCM,
  CDDA planar PCM, observed game PCM music modes0/0x10 (pitched sustain loops), and Apple IMA4 modes
  0x16 (movie ring streaming) / 0x1E6 (one-shot effects) are implemented.
  Mode0x02 adds pitched IMA4 soundfont playback (final-word endpoint);
  mode0x1EA adds11025Hz IMA4 effects (final-byte endpoint), both verified against
  independent FFmpeg asset decodes. The uncapped multi-game stress audit also
  found1F6/1FA fixed-rate IMA4 loops; both are supported, as are mode16
  final-word movie-ring endpoints used by Wacky Race and SpongeBob.
  Shrek mode12 fixed-tuple IMA4 movie streaming runs at44100Hz; this does
  not enable mode12 with the still-unverified soundfont-envelope tuple.
  IMA4 uses 34-byte blocks / 64 samples at 22050 Hz; original movie/effect
  samples match independent FFmpeg decoding exactly. Compressed cursor +0x28
  advances through headers/data and wraps at the streaming ring boundary.
  Master target1188/current1248 apply stereo volume forFFFF ramp parameters
  at1184. Linear/32767 gain before saturation is approximate; finite ramps
  remain unsupported. Original BIOS volume and immediate fade routines pass.
  Sound1004 low-six pending bits use W1C acknowledgment. Hardware envelopes,
  precise gain/interpolation and sound IRQ9 event generation remain unresolved.
- 0xC0000000 (38 of 50 games, driver 0x109FE608 in Multisports): a descriptor-chained DMA.
  Descriptors of 0x80 bytes (w0 0x00400000, w1 0x1F004800, source, destination
  0xDC000000/0xDC020000 + 2n - the far side's own address space, next, count, ...);
  +0x88 = list, +0x18 = 1, +0x58 = 1, +0x80 = 2 start it; polls +0x00 bit 0 (done) or
  +0x40 != 0 (error) up to 0x100000 times, then +0x10 |= 1 and waits for +0x84 bit 4.
  Now implemented by src/zevio_dsp.c, including channel1 at +0xC0. The real ARM
  loader and ZSP400 firmware boot pass tools/test_dsp_loader.c. Native firmware
  decodes 60 consecutive Multisports frames, every Y/U/V sample within one level
  of libjpeg; see ../dsp-research/IMPLEMENTATION.md and tools/test_zsp_movie.c.
  A fresh Multisports frontend boot now delivers60 native frames and bit-exact
  movie audio across an audio-ring wrap. Other games remain unverified; DSP
  timing is approximate.
- Multisports' event intros use native DSP decoding and MIDI IMA4 audio. The
  original ARM driver109F6114 supplies planar Y/U/V to B8000164/168/16C;
  colour layer1 is now scanned out as320x160 planar4:2:0, not packedRGB555.
  The captured movie window is(96,40)..(415,199). Its luma matches the native
  reference exactly; RGB agrees with Pillow within one level. Earlier noise
  was missing DSP execution plus incorrect scanout interpretation. See
  tools/capture_movie_frontend.c and tools/test_ve_movie.c.
- VE colour order is movie layer 1 behind RGB layer 0, then tile layers.
  RGB bit-15 transparency opens the movie window. Spider-Man leaves both
  colour layers enabled after DSP stop and paints opaque menus/loading/gameplay
  over the old movie planes; drawing video last caused its stale/noisy box.
  Hardware reference footage and captured RGB pixels confirm the order. See
  ../dsp-research/VE-WINDOW-REFERENCES.md and tools/test_ve_compositing.c.
- Key register 0x900A0018: bit 25 is power; the other bits are guessed.
- RTC (0x90090000 +0x00) counts EMULATED seconds from power-on (VFLASH_RTC=<unix time>
  fixes the start for reproducible runs; default host time at start). It used host
  time() until 2026-09-27, which made runs depend on emulation speed: a faster
  rasteriser hung SpongeBob at frame ~6760 (it times its loading screen by the RTC).
- FMV: Cars USA's in-game TV movie (/A_MJPEG/OP.MJP), Disney Princess's adventure intro
  (/MAIN/MAIN01.MJP) and Multisports' event intros all go through the 0xC0000000 DMA /
  DSP. Earlier builds never decoded and Cars stayed on "Loading" to frame16000.
  Retest with the native core. Cars' main menu displays a GREEN confirmation
  prompt; Enter-only scripts stayed in that menu. Mapping its confirmation
  through the current controller reports still needs validation.
- UARTs send FF 01 02 40 74 40 46 44 40 repeatedly (controllers?) and get nothing back.
- Video engine: layer order, tile-entry upper bits and windows 0-3 are not worked out.
- Speed (2026-09-26 boot audit): Dingo Rallye power-on -> title screen (frame ~2760) takes
  ~50 s wall, frame 3650 ~70 s (was 138 s). What made it faster: a soft TLB in hw.c (4 KB pages
  with host pointers; flushed via cp15.tlb_gen and on CPU writes to page-table pages), a
  CPU fetch-page cache (arm9.h mem_page), table-driven cond_ok, idle-task skip (µMORE idle
  loops recognised by their words; `VFLASH_NOIDLE=1` disables; the report prints "idle: N%"),
  and fast boot (unthrottled until the game kernel has run 16 GE lists; `VFLASH_FASTBOOT=0`).
  Rasteriser (2026-09-27): per-row spans, per-triangle 1/z attributes, a per-triangle
  texture context (tc: palette decoded once, 2x2 quad fetch for bilinear, no libm floorf).
  SpongeBob's loading screen (~770k textured px/frame, the heaviest seen) 17 -> 22 fps;
  with 3D off it runs ~41, so the GE is still about half its frame time there.
  Game phase runs ~47 fps: exec_arm ~35%, GE rasteriser (run/px/texel) ~13%. Not done:
  a faster CD - scaling the servo speed breaks the ROM's disc mount, and loading is CPU-bound
  (software ECC 0x10A1C914 + loading animation). The menu's 0x10B416E4 is a timer-poll delay.
  `tools/boottime.sh <dir>` times a headless boot, `tools/boottl.py <log>` prints the
  per-second timeline, `VFLASH_PROFILE=a,b` + `VFLASH_PROFILE_N=4096` samples guest PCs.

## Running
- Build in WSL/Linux: `make flashem`, `make -f Makefile.libretro`. The Makefiles have no
  header dependencies - after changing a struct in a header, delete src/*.o first.
- Windows (cross-compiled from WSL with MinGW-w64, or natively in an MSYS2 MinGW64 shell):
  `make -f Makefile.libretro platform=win` -> flashem_libretro.dll (imports only
  KERNEL32/msvcrt); `bash tools/get_sdl2_mingw.sh` once, then `make platform=win` ->
  flashem.exe + SDL2.dll + the tools as .exe (objects *.win.o, so both builds share the
  tree; SDL2_PREFIX overrides the SDL2 path). Checked 2026-09-27: flashem.exe on Windows
  renders frames identical to the Linux build (0 of 122880 pixels differ at Dingo frame
  3650); `tools/retro_smoke.c` (a minimal libretro host: core, cue, system dir, frames,
  out.ppm) runs the DLL natively to Dingo's title with audio. Windows paths with '\' work.
- `FLASHEM_BIOS=70004.bin SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./flashem --headless game.cue`
- Diagnostics are env vars (README.md): VFLASH_RAMDUMP(+_FRAME), VFLASH_TRACEPC=a:b:c,n
  (+VFLASH_TRACEMEM=<reg> dumps 8 words at that register, VFLASH_TRACEFROM=<frame>),
  VFLASH_IOHIST=from,to[,page], VFLASH_KEYS, VFLASH_SHOT_FRAME, VFLASH_LOG,
  VFLASH_CDLOG (servo/decoder), VFLASH_CDSTACK (call chain per servo auto-sequence),
  VFLASH_LCDLOG, VFLASH_PUTS=<addr>, VFLASH_GPIOA/B, VFLASH_LID, VFLASH_PWRHOLD.
- The per-60-frame report includes IRQs taken per line.
