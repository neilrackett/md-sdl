# AGENTS.md — MD/SDL Playbook

Quick primer for any agent working in this repo.

## 1. What this project is

MD/SDL is a SidecarTridge Multi-device microfirmware that acts as a C2P and palette-reduction co-processor for SDL 1.2 applications on the Atari ST. The RP2040 receives chunky pixel data and a 256-entry palette over the cartridge bus, performs median-cut palette reduction (256→16 colours) and chunky-to-planar (C2P) conversion, then writes the resulting ST low-res planar frame to `$FA8000` where the ST reads it directly.

The project spans **two repositories**:
- `md-sdl` (this repo) — RP2040 firmware
- `atarist-sdl` — Atari ST SDL 1.2 library, driver in `src/video/xbios/SDL_xbios_md.c`

## 2. Environment setup

- **ARM GNU Toolchain** at `/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin`
  - Export `PICO_TOOLCHAIN_PATH` to this path before building
  - Version 14.x also works; adjust the path accordingly
- **atarist-toolkit-docker** — provides `m68k-atari-mint-gcc`, `vasm`, `vlink` via Docker
  - `stcmd` requires a PTY; run with `pty=true` if you see "not a TTY"
- **SDK environment variables** (set by `rp/build.sh` automatically, but useful for IDE):
  ```bash
  export PICO_SDK_PATH=$REPO_ROOT/pico-sdk
  export PICO_EXTRAS_PATH=$REPO_ROOT/pico-extras
  export FATFS_SDK_PATH=$REPO_ROOT/fatfs-sdk
  ```

## 3. Building

```bash
PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin \
  ./build.sh pico_w debug dbbd9f78-2b92-4209-b9a8-32c90b3bd2fd
```

Use `release` instead of `debug` to disable UART output. A successful build drops `dist/<UUID>-v<version>.uf2` and `dist/<UUID>.json`.

## 4. Key files

### RP2040 firmware (this repo)

| File | Purpose |
|---|---|
| `rp/src/emul.c` | Firmware entry point, command dispatcher, median cut, C2P |
| `rp/src/include/sdl_commands.h` | Command IDs, ROM-in-RAM offsets, surface size constants |
| `target/atarist/src/main.s` | 68k ROM cartridge header + minimal boot stub |
| `rp/src/CMakeLists.txt` | RP2040 build configuration |
| `desc/app.json` | App descriptor template (UUID, name, binary URL, MD5) |

### Atari ST SDL driver (`atarist-sdl` repo, `md-sdl` branch)

| File | Purpose |
|---|---|
| `src/video/xbios/SDL_xbios_md.c` | MD/SDL driver: detection, chunky upload, blitter/CPU planar copy, dirty-rect |
| `src/video/xbios/SDL_xbios.c` | Shared XBIOS lifecycle: calls `SDL_MegaSTE_EnableTurbo/RestoreTurbo` in `VideoInit`/`VideoQuit` |
| `src/video/ataricommon/SDL_megaste.c` | Mega STE 16 MHz + cache enable/restore (shared across all xbios drivers) |
| `src/video/ataricommon/SDL_megaste.h` | Header for `SDL_megaste.c`; exports `MCH_MEGA_STE_COOKIE` constant |

## 5. Command protocol summary

The ST sends commands by performing address-bus reads at `$FB8000 + value`. The RP2040 PIO monitors the bus and `tprotocol_parse()` decodes the stream.

Per-command sequence: `magic(0xABCD)` → `cmd_id` → `payload_size` → `token_lo/hi` → `d3_lo/hi` → `d4_lo/hi` → `d5_lo/hi` → `[inline buf words]` → `checksum`.

The RP2040 writes the echoed token back to `$FAF000` when done. The ST polls this address after every command.

## 6. ROM-in-RAM sub-addresses

| Offset from ROM4 base | ST address | RP2040 address | Content |
|---|---|---|---|
| `+0x8000` | `$FA8000` | `0x20028000` | Planar framebuffer (32 000 B) |
| `+0xF000` | `$FAF000` | `0x2002F000` | Random token (4 B) |
| `+0xF004` | `$FAF004` | `0x2002F004` | Token seed (4 B) |
| `+0xF400` | `$FAF400` | `0x2002F400` | HW palette return (16 × uint16_t) |

## 7. Build notes & gotchas

- The build produces no output on success from `git submodule update` lines — that's normal.
- `ADDRESS_HIGH_BIT` is **not** imported from `term.h` (which is not included); use `0x8000u` directly in `emul.c`.
- `PICO_TOOLCHAIN_PATH` must be exported **before** running `./build.sh` — the rp/build.sh does not set it.
- Expect harmless linker warnings (`ignoring duplicate libraries: 'errors/liberrors.a'`).
- The Atari ST build emits "not a TTY" and "Failed to resize the file" — these are from Docker/stcmd and are harmless.

### Shifter DMA contention (why the driver copies to screen RAM)

Pointing `Setscreen` at `$FA8000` (ROM4) causes the Shifter to steal ~800 000 ROM4 reads/second from the 68000 on every scanline, starving the BLIT_SURFACE commands. The driver copies the finished planar frame from `$FA8000` to a screen-RAM buffer each frame and points `Setscreen` at screen RAM. On STE the hardware blitter does the copy in ~1 ms; on plain ST the CPU loop takes ~20 ms but still beats continuous contention. **Never call `Setscreen` with a ROM4 address.**

### Mega STE 16 MHz + cache

`SDL_MegaSTE_EnableTurbo()` / `SDL_MegaSTE_RestoreTurbo()` live in `src/video/ataricommon/SDL_megaste.c` in the `atarist-sdl` repo. They are called from `XBIOS_VideoInit` / `XBIOS_VideoQuit` in `SDL_xbios.c` — **not** from any per-driver `saveMode`/`restoreMode`. This covers all xbios paths (ST, STE, Mega STE with NOVA card, MD, etc.) and is a no-op on non-Mega-STE hardware.

## 8. Troubleshooting

| Symptom | Fix |
|---|---|
| `arm-none-eabi-gcc not found` | Set `PICO_TOOLCHAIN_PATH` to the `bin/` directory of your ARM GNU Toolchain install |
| `ADDRESS_HIGH_BIT undeclared` | Replace with the literal `0x8000u` — `term.h` is not included in `emul.c` |
| UF2 not found after build | RP compile failed — scroll up to the first error before the copy step |
| ST hangs polling `$FAF000` | Bus sync failure — confirm UF2 is flashed; check UART log |

## 9. Editing guardrails

Agents must **not** modify code inside these directories:
- `pico-sdk/`
- `pico-extras/`
- `fatfs-sdk/`

Keep this file updated as the project evolves.
