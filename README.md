# Daytona USA for Nintendo Switch (Daytona-NX-Modern)

A native Nintendo Switch port of the Daytona USA static recompilation below.
No game data is in this repository or in the release: put your own MAME
`daytona.zip` (Revision A) at `sdmc:/switch/daytona/daytona.zip` next to
`daytona.nro`.

Switch build (devkitPro, or Docker with `devkitpro-mesa-rust:latest`):

    python3 scripts/recompile.py --set daytona --build-dir build-daytona   # needs roms/daytona.zip
    bash scripts/build_switch.sh

Controls: left stick steer, ZR gas, ZL brake (or right stick up/down),
X shift up, B shift down (D-pad Up/Down: 4th/1st), views L bumper, A chase,
Y far, R cockpit; Plus start, Minus coin, Plus+Minus menu, L3 test,
R3 service. In test mode R/L move the cursor.

Widescreen: 16:9 by default (menu SCREEN 16:9 / 4:3). More of the road at
the sides with the HUD centred; the race sky is stretched across the width
and 2D screens get black side bars. With 16:9 the game draws every scenery
block around the car (draw distance +1) so stands and trees reach the edges.

The same tree also builds the Virtua Fighter 2 port
([VF2-NX-Modern](https://github.com/Thorhax/VF2-NX-Modern)); notes on both
ports are in `PROJECT_STATUS.md`.

# Daytona USA static recompilation

Daytona USA (Sega Model 2) rebuilt as native code: the game's i960 program
and the TGP program it uploads are statically recompiled to portable C++,
and the fixed-function hardware (geometrizer, rasterizer, tilemaps) is native
C++. No interpreter, no emulation core. MAME is used only as a test oracle.
See `docs/daytona-usa-recomp-design.md` and `HANDOFF.md`.

No game data is in this repository. You need your own ROM set (a
MAME-format `.zip` or `.7z`): `daytona93` (Daytona USA Deluxe '93) or
`daytona` (Revision A, 1994), or both.

## Setup

**New here? Follow [docs/getting-started.md](docs/getting-started.md)**: step
by step for Windows, macOS and Linux, with fixes for the usual problems.

In short:

1. Copy your ROM set into `roms/` in the project folder, named exactly after
   the set: `roms/daytona93.zip` (Daytona USA Deluxe '93) and/or
   `roms/daytona.zip` (Revision A, 1994), or `.7z`. Each is built as its own
   game; other Daytona sets are rejected.
2. Run setup. Linux or macOS:

       ./setup.sh

   Windows (PowerShell):

       powershell -ExecutionPolicy Bypass -File setup.ps1

   On Windows the game is built with Clang from the Visual Studio Build
   Tools. Setup installs them, or adds the Clang tools to the Visual Studio
   you have; Windows asks for permission for that. `setup.ps1 --msvc` uses
   Microsoft's compiler instead.
3. Run the command setup prints at the end (`build/daytona`, or
   `build\Release\daytona.exe` on Windows).

**Updating:** `git pull`, then run setup again (it recompiles the game from
your ROM set, since updates can change the generated code).

**Clean rebuild** (after a failed build, or to start over): delete the
`build` folder and run setup again. Your ROM set in `roms/` and the
downloaded libraries in `extern/` are kept.

If setup fails, it says whether your ROM set was rejected (the line above
names the file) or the build failed (the errors are just above). The
[guide's troubleshooting](docs/getting-started.md#troubleshooting) covers
the usual ones. Every change is built and tested by GitHub Actions on
Windows (Clang and MSVC), macOS and Linux (GCC and Clang), without a ROM set.

Setup scripts install the toolchain (C++20 compiler, CMake, Ninja, Python 3, Git;
Visual Studio 2022 Build Tools on Windows, Homebrew packages on macOS, your
distribution's packages on Linux), fetch the pinned dependencies into
`extern/`, build, and run the tests. Put your ROM set at
`roms/daytona93.zip` (or `.7z`) first and the game code is recompiled as well (into
`build/`, never committed); Revision A at `roms/daytona.zip` is recompiled
into `build-daytona/`. Already have a toolchain? Run
`python3 scripts/setup.py` directly.

Options: `--msvc` (setup.ps1 only: Microsoft's compiler), `--test-extras`
(optional test dependencies), `--with-mame` (MAME source for the oracle
test), `--build-mame` (the patched MAME that records validation traces;
Linux and macOS).

After changing the recompiler or the seeds: `python3 scripts/recompile.py`
(and `python3 scripts/recompile.py --set daytona --build-dir build-daytona`
for Revision A). Revision A is `build-daytona/daytona`, with its own
settings and saves. Its factory settings are a linked twin cabinet, which
waits for a second cabinet: set a single cabinet once in test mode (F2).

## Link play (the 1994 set)

Revision A (`daytona`) can link two or more cabinets over a network, Wi-Fi
or wired, as linked arcade machines race each other (experimental). On each
computer, in the launcher's Game tab, tick **Link to other cabinets**, give
this cabinet's port and the next cabinet's `host:port` (the cabinets form a
ring; with two, each one's next is the other), then in test mode (F2) >
GAME SYSTEM set **LINK ID** (one MASTER, the others SLAVE) and a different
**CAR NUMBER** on each. Two on one computer: start the second with
`--profile 2` (its own settings and saves) and give the two different ports,
e.g. 15112 and 15113, each the other's as next.

## Playing

    build/daytona

(`build/Release/daytona.exe` with the Visual Studio generator.) The launcher
opens first:

- **Game**: choose your `daytona93` ROM set, `.zip` or `.7z` (Browse, or type the path); every file
  is checked against the ROM set this build was recompiled from. Graphics API
  (automatic, Vulkan, Direct3D 12, Metal), Renderer (software, the exact
  CPU renderer; or hardware, on the GPU, experimental), Super sampling
  (hardware renderer: off, or the 3D drawn at 2x to 4x the original resolution), fullscreen, Draw mode (double buffered,
  as the game; single buffered or every third frame draw less often, for slower
  machines; the game itself runs at full speed), and Skip launcher (start
  the game straight away next time; Esc still opens the launcher). Enhancements
  (off by default): widescreen 16:10, 16:9 or 21:9, which shows more of the
  scene at the sides with the HUD kept 4:3 in the centre, or with "HUD at
  the screen edges" (experimental) the lap times, position, condition panel and course map
  moved out to the sides (in a race the sky at the sides is plain blue, or
  with "Stretch tile background" the game's sky picture stretched across);
  and
  a draw distance slider for the scenery (default
  is the game's own; shorter runs faster). Start.
- **Controls**: bind every arcade control to a key, a gamepad button or
  axis, and a wheel or joystick input (experimental; click, then press). Triggers, sticks,
  wheels and pedals are analogue. Wheels, pedals and shifters work as
  separate devices too; binding a wheel or pedal axis also sets its range
  (turn or press as far as full lock or full travel should be, and let go).
  Force feedback (experimental, untested on a real wheel so far): the arcade wheel's motor (centring, friction, rumble, the
  wheel pulling) plays on the device steering is bound to (a force feedback
  wheel, or a gamepad's rumble), with a strength slider. Live meters, dead
  zones, invert steering.

In the game, Esc brings the launcher back (Resume, Reset, Quit). Settings
are saved as they change, with the settings EEPROM and backup RAM, in your
user data folder (`launcher.ini`). Options: `--rom FILE.zip --autostart
--gpu vulkan|direct3d12|metal --fullscreen`.

Default controls:

| Control | Keyboard | Gamepad |
| --- | --- | --- |
| Steer | Left / Right | Left stick (analogue) |
| Accelerate / brake | Up / Down | Right / left trigger (analogue) |
| Gears 1-4 | 1 2 3 4 | |
| View buttons VR1-VR4 | A S D F | Face buttons |
| Shift up / down | W / Q | Right / left shoulder |
| Coin / start | 5 / Enter | Back / Start |
| Test / service | F2 / F3 | |
| Fullscreen / launcher | F11 / Esc | |

Sound: the sound board's 68000 program is statically recompiled like the
i960 code and runs on the native board with the YM3438 (ymfm) and both
MultiPCMs; output goes through SDL audio. Volume and mute are in the
launcher.
