# Virtua Fighter 2 (Sega Model 2A) — Nintendo Switch Port
**Project Status & Technical Notes**
*Updated: October 9, 2026 (round 4)*

---

## 0. Daytona USA on the shared tree (2026-10-10, testing)

Working copy for Daytona: `Model2/daytona-nx` (clone of vf2-nx; origin is
Daytona-NX-Modern). Switch build: `bash scripts/build_switch.sh` ->
`Model2/daytona.nro` (no RomFS: the user's daytona.zip stays on the SD card).
- Two VF2 changes broke Daytona (it hung at boot: no interrupts, no TGP):
  the board and m2recomp took a program image over 1 MB to mean Model 2A,
  but Daytona's image is 2 MB too. Model 2A is now `M2_ROMSET_VF2` in the
  board, and m2recomp skips the 0x220000 ROM mirror only with
  `--no-rom-mirror` (recompile.py passes it for vf2; VF2 output unchanged).
- Checked against the Daytona 1.0.0 release commit (2fd6cce, worktree
  `Model2/daytona-v100`): race_basic with the Switch app's NVRAM (single
  cabinet), 6,000 frames, every dump identical, same i960/TGP/interrupt counts.
- Widescreen 16:9 by default (menu SCREEN), HUD centred (HUD-at-edges is
  off, so the video stays threaded); the margins use Daytona's existing
  backdrop rules (plain sky in races).
- Threaded video on Daytona too (+1 frame latency): threaded output at frame
  N equals unthreaded N-1 over 6,000 frames (5,337 distinct pictures); the
  Switch render path equals the standard path over 6,000 frames at 16:9.
- Host raster (single thread) in the race: 24.4 ms mean at 4:3, 29.9 at 16:9
  (+23%). The 1.0.0 Switch log (unthreaded) peaked at 19.9 ms raster at 4:3
  and was under 57 fps in half its windows, so speed is the thing to check.
- Hardware test 1 (16:9): 62 of 62 gameplay windows at 57.52 fps, raster
  peak 7.2 ms. Smearing at the sides: GENTLEMEN START YOUR ENGINES (a 2D
  screen: row edge colours carried out) and the race start, where the clouds
  ended at the 4:3 edge over plain sky. Now the Switch sets
  `set_stretch_backdrop` (the race sky stretched across 16:9; wrapping it
  shows a seam, see fill_margins) and `set_pillarbox_2d` (2D screens: black
  side bars). The stretch is integer now (weights per column, at most 1 off
  the old float result in 2,047 bytes over 60 frames). Threaded Switch path =
  standard path over 6,000 frames with both options.
- Hardware test 2: 38 of 38 windows at 57.52 fps. Still some clipping at
  the sides: scenery (grandstands, trees) cut off in the margins. The game
  picks scenery by course cell for its 4:3 view (visibility masks over the
  5x5 around the car, HANDOFF "Draw distance"). At 16:9 the Switch now sets
  draw distance +1 (all 5x5 cells, polygon budget 10000; game logic lists
  untouched). Host, race_basic 16:9 every 50th frame: 13 of 120 frames gain
  margin pixels (stands and trees past the 4:3 edge); raster mean 13.6 ->
  13.7 ms.
- Note: `build-daytona` made by recompile.py has no build type (-O0); the
  game's geometrizer self-test then takes minutes. Configure it with
  `-DCMAKE_BUILD_TYPE=RelWithDebInfo`.

## 1. Working directory

As of 2026-10-09 the project lives in **`/root/tmp/switch-upd/Model2/vf2-nx`**
(a copy of `daytonausa/daytona-arcade-recomp` with its git history and the
uncommitted VF2 work). The old tree under `daytonausa/` is no longer the
place to make VF2 changes.

| Path | Purpose |
| :--- | :--- |
| `Model2/vf2-nx` | Source (CMake, recompilers, runtime, Switch platform code) |
| `Model2/vf2-nx/build-vf2` | Host build: ROM cache, `m2recomp`, `m2run`, generated C++ (`gen/`) |
| `Model2/vf2-nx/build-switch-vf2` | Switch build (devkitA64 via Docker image `devkitpro-mesa-rust:latest`) |
| `Model2/vf2.nro` | Deployable Switch binary (RomFS bundled) |
| `Model2/vf2-round4.nro` (and earlier rounds), `vf2-previous.nro` | Rollback: earlier rounds, the pre-fix build |
| `Model2/mame-ref` | Host MAME (Model 2 only) used as the reference |
| `Model2/VF2/vf2.zip`, `vf2-nx/roms/vf2.zip` | MAME `vf2` ROM set |
| `Model2/vf2-logs/` | Logs and captures from Switch test runs |

### Build

```sh
cd /root/tmp/switch-upd/Model2/vf2-nx
python3 scripts/recompile.py --set vf2 --build-dir build-vf2   # host tools + generated C++
bash scripts/build_switch_vf2.sh                                # -> ../vf2.nro (Model2/vf2.nro)
```

The two failing host targets (`m2sndcheck`, `fp_vs_mame`/`test_fp`) were
already broken before this round; `m2run` and the recompilers build.

### Headless test

```sh
./build-vf2/m2run build-vf2/rom_cache/vf2 3000 \
    --inputs scripts/inputs/vf2_fight.txt --dump /tmp/out --every 100 --wav /tmp/out/a.wav
```

`scripts/inputs/vf2_fight.txt`: two coins, start, pick Akira, walk right
and alternate punch/kick. Input scripts gained `p1=MASK` / `p2=MASK`
(IN1/IN2 bits: 0x01 punch, 0x02 kick, 0x04 guard, 0x10 down, 0x20 up,
0x40 right, 0x80 left).

---

## 2a. Widescreen (2026-10-10)

Ported from the Virtual-On port (`Model2/virtualon/von-nx`, same renderer):
16:9 by default, menu SCREEN 16:9 / 4:3. VF2 also uses the wrapped back
tilemaps (`Video::set_wrap_backdrop`) and black side bars on menu screens
(`Video::set_pillarbox_2d`: margins black when no 3D reaches them; VF2's
fights are not "scenes" to `Video::scene()`, so that test is not used).
Host: identical i960/TGP instruction counts at 4:3 and 16:9; the Switch
render path (M2_VITA_RENDER_OPT, host build `build-vf2-vopt`) matches the
standard path over 3,999 frames. Raster cost on the host: +24% at 16:9.
Hardware (16:9): 307 of 309 gameplay windows at 57.52 fps; raster peaks
~10.5 ms, tile drawing ~3.5 ms, compose ~2 ms. The two slow windows are a
46 fps blip and one 11 fps window where geo and raster both read ~75 ms (a
pause outside the game, e.g. the clip capture).
Known limit: the back tilemaps are 512 px wide and the game shows 496, so
the margins show the 16 hidden columns (which the game rewrites as it
scrolls, often stale) and then the tilemap again from the other side. On
some camera angles that gives a visible seam in the sky ~13 px past the 4:3
edge, or horizon art missing in a margin. No wider source data exists.

## 2. Fixes in this round (2026-10-09)

### A. Board timers never interrupted (slow loading, missing braid)
`0x00f00000` timers: VF2 enables interrupt bit 5 (timer 3) and runs timer 3
as a 250 Hz tick (re-armed to ~100,000 ticks = 4 ms in its handler). The
port never raised timer interrupts, so the game's clock never ran:
- the title/logo and other timer-paced waits stretched out;
- **Pai's braid physics is driven by it**: with the earlier build the braid
  vanished (seen on the Switch capture and reproduced on the host build of
  the old code); with the fix it is drawn and swings.

Now as MAME `timers_r/timers_w/model2_timer_cb` (Model 2A only, Daytona
unchanged): count down at 25 MHz, at zero read 0xfffff and raise request
bit 2+n if enabled.

Timer time is "board time" (`M2Board::vtime()`): executed i960 instructions
plus the time skipped when the game idles in its wait-for-vblank loop. When
it idles with a timer due before the frame ends, the game loop skips ahead
to the timer instead of ending the frame, so the interrupt lands on time.

### B. CPU budget per frame too small for VF2's loaders
CPU-bound frames were capped at 110,000 i960 instructions (Daytona's MAME
figure: FP- and TGP-heavy). VF2's loading frames run a Huffman-style
decompressor (`0x4c6e0-0x4c930`): ALU ops 1 cycle, loads 4, stores 2 in
MAME — about 1.5 cycles each, ~290,000 instructions per frame at 25 MHz.
`M2Board::kFrameInstructions` is now 290,000 for VF2 (110,000 for Daytona).
Measured: warning-screen load ends at frame 205 (was 480), title load at
frame 739 (was ~880); gameplay frames are unaffected (~37k, idle-ended).

### C. Sound effects garbled (stream buffers refilled in the wrong half)
VF2 plays voices and SFX by streaming: each slot plays an 8 KB looped
buffer in sound RAM (`SA = 0x10000 + slot*0x2000`, 8-bit, loop 0-0x1fff)
and the 68000 refills the half not being played from the sample ROMs. It
writes MSLC (0x408), waits ten instructions, and reads CA bit 0 at 0x409.
The SCSP was rendered lazily (end of each frame slice or on a register
write) and its monitor latch only updated while rendering, so the 68000 read
another slot's position or a stale one. Fixed: the SCSP renders up to the
68000's current time before every register read, and the monitor reads the
selected slot's live state. Measured over the fight script: writes into the
512-byte block being played: 585,000 of 11.7 M (old) → 0 of 4.6 M; the
68000 also does 2.6× less copying.

### D. SCSP interrupts as MAME
Per-line assert/clear (the 68000 sees the highest asserted line), one source
asserted per check in MAME's priority, cleared by SCIRE or an empty MIDI
FIFO, the "SCIRE on an expired timer re-pops it" behaviour, SCIPD stored as
written, no DMA/CPU interrupts to the 68000 (VF2 enables only MIDI L3,
timer A L2, timer B L1), DMA restoring its registers, a timer write of 255
leaving a running timer alone.

### E. Audio latency on the Switch
The SCSP output queue was only capped at 2 s, so drift between the
emulation and the audio device could delay hit sounds by up to 2 s. Now
held near 70 ms: small drift corrected by dropping/repeating ~0.4% of
frames spread over a push, large excess trimmed at once.

### F. ARM64-safe float→int conversions
`cvtri`/`cvtzri` in recompiled i960 code (`gen::d2i`) and the geometrizer's
z-clip and luminance casts (`s32_x86`) now give MAME's x86 result
(0x80000000 for NaN/out of range) on every host. ARM64 saturates instead.
Not hit in the test runs; kept for safety.

---

## 2b. Round 2 (2026-10-09, after the first hardware test)

Hardware report: Pai's braid and the graphics fixed; second coin sound cut
off; title music a few seconds before the 3D scene; some slowdown; attacks
and voices far too quiet.

### G. SCSP DSP: 0x7c0-0x7ff is not a MADRS mirror (quiet voices/hits)
VF2's sound program writes the DSP block 0x700-0x7ff in one pass: COEF,
MADRS (0x780-0x7bf), then zeros at 0x7c0-0x7ff. MAME (and the port, copied
from it) mirror MADRS at 0x7c0, so the zeros wiped every delay-line address:
the reverb's taps all fell on one address and the effect bus (music sends,
EFSDL 0 dB) became a loud smear about 7 dB above the dry mix, where the
voices and hits play (they are dry: IMXL 0). Now 0x7c0-0x7ff is separate
storage. Fight mix: dry RMS 1031, reverb 247 (was reverb ~2300 over dry
~1000); overall level 7.5 dB lower, so the Switch output gains 2.5x
(`kScspGain` in platform/switch/audio.h; peak 6607/32767 before gain).
The port matched MAME exactly before this fix (same RMS 2479 over the fight).

### H. Second coin sound cut off = audio underrun on the Switch
The capture shows the second coin sound stop for ~60 ms and resume where it
left off: the output queue ran dry during slow frames. The SCSP side matches
MAME (same coin sound levels and timing). Fixed by the pacing and threading
below, plus a 90 ms queue target (was 70).

### I. Slowdown: threaded video and deadline pacing
- `Video::set_threaded(true)` (Switch, VF2): screen_update decodes tilemaps
  and snapshots tile RAM, pens, polygons, palette/colour/luma RAM on the
  emulation thread, then a worker (cores 1-2) draws 2D layers and the 3D
  while the next frame emulates. One frame of extra display latency. Texture
  RAM is read live. Verified on the host: 2400-frame fight, 1472 distinct
  pictures, identical to the unthreaded output one frame later
  (`m2run ... --threaded-video --hashes FILE`).
- Native 57.52 Hz pacing now runs against a deadline: an overrunning frame is
  made up by the next ones (resync if more than 3 frames behind). Before, any
  frame over 17.38 ms slowed the game and starved the audio.
- Fight frames before: total ~15-17 ms on one thread (core 5-7, video ~9).

### J. Title music before the 3D scene: matches the arcade (MAME)
MAME plays the title music on the SEGA logo screen about 4 s before the
attract fight appears; the port does the same (music ~45 frames after the
logo, 3D ~230 frames later in both). Not a loading delay.

### MAME reference (oracle)
`Model2/mame-ref`: MAME (Switch fork's source, `git archive`) built for the
host with only `sega/model2.cpp`: binary `mame-ref/vf2`. `oracle/drive.lua`
plays an m2run input script and takes snapshots; `oracle/roms/segabill` holds
a zero-filled billboard ROM (not in the set; MAME refuses to start without it).

```sh
cd Model2/mame-ref/oracle
M2_SCRIPT=../../vf2-nx/scripts/inputs/vf2_fight.txt M2_FRAMES=2400 M2_SNAP_EVERY=100 \
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ../vf2 vf2 -rompath "../../VF2;roms" \
  -video none -sound none -nothrottle -skip_gameinfo -seconds_to_run 600 \
  -autoboot_script drive.lua -wavwrite out.wav -snapshot_directory snap \
  -nvram_directory nv -cfg_directory cfg
```
Host deps installed for it: libsdl2-dev, libsdl2-ttf-dev; fontconfig headers
unpacked locally (libfontconfig-dev conflicts with the installed library).
Note MAME has the MADRS-mirror bug (G).

---

## 2c. Round 3 (2026-10-09, third hardware test)

Hardware report: big performance win, no more audio dropouts; voices and
attacks still a bit quiet; significant slowdown on Sarah's stage.

### K. Slowdown: core time grows over a session on the Switch only
Switch perf log: `core_ms` (i960 + TGP wall time on the emulation thread)
rises steadily within each round (Sarah: 5.7 -> 12.5 ms in one round, 15-19
ms in the next), as do raster (worker) and texture upload, while geo_ms
stays ~1.0. Host (optimized build, 20,000-frame scripted fight through
Sarah's stage, and a random-input run): core flat at ~1.0-1.2 ms per frame,
gameplay frames ~40k i960 instructions, no capped frames. Not reproduced, so
this build adds diagnostics (perf log `diag:` line every 2 s): i960
instructions per frame, frames at the instruction cap, worst core time,
sound worker ms, main-thread wait for the sound worker, audio queue ms,
pending scheduler events, and a fixed ALU loop and 4 MB memcpy timed (if
those slow down too, the console is slowing: clocks/heat).
Changes that may help:
- Lockstep reused no callback slots: `calls_` grew by every event ever
  scheduled (~75 per frame, std::function each). Slots are now reused
  (~4,400 max).
- Threads: emulation thread on core 0 only; the higher-priority sound worker
  and SDL audio callback on cores 1-2 (they could preempt the emulation on
  core 0 before); drawing threads (lower priority) may use core 0 when idle.
- SCSP DSP program decoded once per program change, not per sample
  (bit-identical audio; ~3% of sound time on x86, more on ARM).

### L. Attack sounds panned hard right: "arcade mono"
Attack sounds are RAM-resident samples keyed in threes (pitch variants),
always DIPAN 0x08 (left -24 dB) whichever side the fighter is on; nothing is
panned left; music and voices are centred; the DSP returns stereo reverb
(EFREG0/2 left, 1/3 right). Yabause and MAME agree on the pan direction. So
the mix targets a mono cabinet speaker on the right channel. New menu item
SOUND MIX: ARCADE MONO (default: both speakers play the right channel) /
STEREO (as emulated: hits on the right speaker only, ~3 dB weaker).
Yabause also confirms G: its MADRS is 0x780-0x7bf only.

### Host tooling
- build-vf2 is now RelWithDebInfo (was unoptimized: ~37 ms/frame -> ~10).
- `m2run --hashes FILE` also logs per frame: elapsed ms, core/geo/sound us,
  i960 instructions. `perf` is installed for profiling.

---

## 2d. Round 4 (2026-10-09)

- Round 3's log: the core-time growth is gone. In fights, per-frame core
  time peaks around 2.5 ms (was a 15-19 ms average), i960 instructions
  ~40k per frame, and the ALU and memcpy benchmarks stay flat. The likely
  fix was the thread pinning and/or the scheduler slot reuse (K).
- Effects/voices +3 dB over the music: `Scsp::set_effects_gain` (off by
  default; Switch sets 1.41). Applies to streamed slots at TL <= 0x0c
  (voices, coin and other effects; music never goes below 0x0d) and to the
  RAM-resident attack sounds (SA >= 0x50000, DIPAN 0x08). Measured: music-only
  select screen RMS 1222 -> 1226; fight effects component up 3 dB.
  `m2run --fx-gain X`.
- Controls (both players): B punch, A kick, Y guard, X guard+kick;
  ZL/ZR/L/R still guard.
- Cheat: ZL+ZR+D-pad Up toggles P1 infinite health (on-screen banner).
  Health is u16 at 0x00510b2c (176 = full; mirror at 0x00515b40), held at
  the most it has been since enabled, written at each frame end
  (`GameLoop::set_p1_infinite_health`, `m2run --p1-inf-hp`). Verified: idle
  P1 keeps a full bar through two rounds; ring-outs still lose rounds. While
  ZL+ZR are held, D-pad Up is not passed to the game.
- `m2run --ram-dump DIR --every N`: main RAM then work RAM, raw.
- Round 4 hardware log: 165 of 166 gameplay windows at 57.52 fps. The one
  slow window had normal frame work (3.4 ms), so the pause was outside the
  game, probably the screenshot. Core time peaks at ~2.5 ms in fights and
  ~15.7 ms on loading frames.
- Icon: `platform/switch/icon_vf2.jpg` made from `Model2/vf2.png` (500x500
  RGBA, composited on white, 256x256 JPEG).

---

## 3. Status

| Component | Status |
| :--- | :--- |
| Boot, RomFS, controls | Working |
| Attract, coin, select, fight, rounds | Working (host-verified to round 2) |
| Timers / loading | Fixed (A, B) — verify load feel on hardware |
| Pai's braid | Fixed via timers (A) — verify on hardware |
| SFX / voices | Fixed (C, D, E, G) — verify balance by ear on hardware |
| Coin sound dropout | Fixed (H, I) — verify on hardware |
| Slowdown | Fixed (I, K): fight core ~2.5 ms on the Switch |
| Music before 3D scene | Arcade behaviour (J) |
| Music | Working |

### Next checks on hardware
- Sound effects, hit sounds and announcer: clean, in sync with the picture.
- Stage load: scene appears with its music, no long black/static wait.
- Pai's braid present and swinging; other characters' hair/cloth.
- Perf overlay: core ms during loads (up to ~290k instructions per frame).

### Open
- No MAME oracle for VF2 on this machine (host MAME build blocked by
  package conflicts for SDL2/fontconfig dev files); checks so far are by
  analysis and instrumentation.
- `M2Board::in_idle_loop` addresses for Model 2A came from earlier work and
  have not been re-derived.
