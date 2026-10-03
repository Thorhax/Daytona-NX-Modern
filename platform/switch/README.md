# Daytona USA Recomp - Nintendo Switch Port

Native Nintendo Switch standalone homebrew port (`.nro`) using devkitPro/libnx and SDL2.

## Features
- **Ahead-Of-Time Recompilation**: Uses native C++ code statically recompiled from the original Sega Model 2 i960 CPU, Fujitsu MB86234 TGP DSP, and 68000 sound CPU programs.
- **Full Speed Emulation**: Locked 60 FPS / 57.5 Hz arcade timing on Tegra X1 Cortex-A57.
- **Audio**: Full stereo sound emulation (YM3438 FM synth + 2x MultiPCM sample playback via `ymfm` and SDL2 audio).
- **Controls**:
  - **Steering**: Left Analog Stick (or D-Pad Left/Right)
  - **Accelerate**: Right Trigger (`ZR`) or Right Stick Up
  - **Brake**: Left Trigger (`ZL`) or Right Stick Down
  - **Gear Shift**: Right Bumper (`R`) to shift up, Left Bumper (`L`) to shift down, D-Pad Up (Gear 4) / Down (Gear 1)
  - **VR Views**: Face buttons (`B`: Bumper view, `A`: Chase view, `Y`: Far chase, `X`: Cockpit view)
  - **Arcade Switches**: `Plus` (`+`) for Start, `Minus` (`-`) for Coin
  - **Pause / In-Game Menu**: Press `Plus` + `Minus` simultaneously

## File Placement on SD Card
Place the files on your Nintendo Switch SD card as follows:
```text
sdmc:/
  └── switch/
      └── daytona/
          ├── daytona.nro
          └── daytona.zip
```
*(Optionally, `daytona93.zip` is also supported if compiled for Deluxe '93)*

NVRAM saves (`ioboard_eeprom.bin`, `backup_ram.bin`) are automatically stored in `sdmc:/switch/daytona/`.
