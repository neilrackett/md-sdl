# MD/SDL: SDL Coprocessor for Atari ST

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://x.com/neilrackett)

## Introduction

Unless you're lucky enough to own a TT or Falcon, you'd struggle to achieve more than 1-2 FPS using the Atari ST version of Simple DirectMedia Layer (SDL). Until now.

Welcome to the SDL 1.2 video coprocessor for the Atari ST with SidecarTridge Multi-device (MD), or MD/SDL for short.

MD/SDL turns the RP2040 inside the MD into a graphics coprocessor for SDL 1.2 applications running on the Atari ST, including [Doom](https://github.com/neilrackett/atarist-doom) and [Rise of the Triad](https://github.com/neilrackett/atarist-rott).

## Goals & Progress

The aim is this project of to make it as easy to port SDL content to the Atari ST as it is for TT and Falcon by outsourcing as much of SDL's graphics processing functionality to the MD as possible, starting with the most CPU intensive and progressing from there.

### Make it work

- ✅ C2P processing: ~4ms for full-frame Bayer-mapped C2P
- ✅ Palette reduction: ~0.1ms to create 16 colour palette and Bayer mappings from 256 using median cut
- ✅ Pipelined parallel data processing: overlaps ST upload with RP2040 frame conversion
- ✅ Dirty rect handling: avoid full-frame C2P if not needed
- ✅ STE blitter path: ~1ms to copy planar data to screen
- ✅ Up to ~25fps?

### Make it better

- 🤔 Cartridge upload time: ~50ms to push full 64KB surface
- 🤔 Command overhead: 1-5ms for 34 BLIT_SURFACE commands plus FLIP
- 🤔 ST-side display copy: 20ms without blitter to copy planar data to screen

_If you have any ideas for boosting performance, please feel free to submit a a PR!_

## How it works

The ST maintains a simple 320×200 8bpp chunky pixel surface in RAM. When the application calls `SDL_Flip()`, the ST sends that surface to the RP2040 over the cartridge bus. The RP2040 performs 256→16 colour palette reduction using median cut, precomputes Bayer-dithered palette mappings, converts the result from chunky to ST planar format (C2P), and writes the finished frame into one of two ST-visible planar slots in the ROM4 window. The ST copies the ready slot back to screen RAM, so there is no software C2P on the 68000 at all.

```
Atari ST (68000)                          RP2040
────────────────                          ──────
SDL_SetVideoMode()  ──CMD 0x01──►  init chunky surface
SDL_SetColors()     ──CMD 0x03──►  median cut → Bayer maps → 16 colours → $FAF400
SDL_Flip()          ──CMD 0x04──►  blit chunky rows (×34 chunks)
                    ──CMD 0x06──►  queue async Bayer-dithered C2P on core 1
vsync / next flip   ◄──────────   mailbox says which planar slot is ready
ST copies ready slot◄──────────   blitter (STE) or CPU (ST) copies to screen RAM
Setscreen(screen RAM)              Shifter reads from ST RAM — no ROM4 contention
```

Pointing `Setscreen` directly at ROM4 causes the Shifter to steal ROM4 bus cycles from the 68000 on every scanline, starving the BLIT_SURFACE commands, so the SDL driver copies the finished planar frame to a screen-RAM buffer each frame and points `Setscreen` there instead. On STE the hardware blitter handles the copy in ~1 ms; on regular ST the CPU copy takes ~20 ms but still performs better than continuous Shifter contention.

The palette return area at `$FAFA80` contains 16 STE-format `uint16_t` values. A mailbox at `$FAFA20` reports `submit_seq`, `ready_seq`, `palette_seq`, `worker_busy`, and timing counters. The random token at `$FAFA00` is still polled after every command to synchronise the ST with the RP2040.

## Hardware requirements

- [SidecarTridge Multi-device](https://sidecartridge.com) (RP2040-based ROM cartridge emulator)
- Atari ST or STE (TT and Falcon continue to use their existing SDL video drivers)
- Raspberry Pi Debug Probe or Picoprobe for flashing/debugging (optional but recommended for development)

## Repository structure

```
rp/src/              RP2040 firmware (C, Pico SDK)
  emul.c             Firmware entry point, command dispatcher, C2P, median cut
  include/
    sdl_commands.h   Command IDs, address offsets, surface size constants

target/atarist/      Atari ST ROM stub (68000 assembly)
  src/main.s         ROM cartridge header + minimal pre-auto hardware handshake

pico-sdk/            Raspberry Pi Pico SDK v2.2.0 (git submodule)
pico-extras/         Pico Extras sdk-2.2.0 (git submodule)
fatfs-sdk/           FatFS SD/SDIO driver (git submodule)
```

The Atari ST SDL 1.2 driver lives in a separate repository:
[atarist-sdl](https://github.com/neilrackett/atarist-sdl) — see `src/video/xbios/SDL_xbios_md.c`.

## SDL driver API

MD/SDL integrates transparently into SDL 1.2. No application changes are required — just link against [the patched SDL 1.2 library](https://github.com/neilrackett/atarist-sdl.git). The driver is selected automatically when the SidecarTridge is detected.

```c
#include "SDL.h"

SDL_Init(SDL_INIT_VIDEO);
SDL_Surface *screen = SDL_SetVideoMode(320, 200, 8, SDL_HWPALETTE);

/* Render into screen->pixels as usual */

SDL_Flip(screen);   /* uploads chunky surface, triggers C2P on RP2040 */
```

`SDL_SetColors()` sends the full 256-entry palette to the RP2040 for median-cut reduction. The firmware precomputes 4×4 Bayer-phase palette maps so `SDL_Flip()` and `SDL_UpdateRects()` can ordered-dither indexed pixels during C2P. `SDL_Flip()` and `SDL_UpdateRects()` submit asynchronous conversion work; the ST presents the most recent ready planar slot on `Vsync()` or at the start of the next flip.

## Memory layout

| Region           | ST address | RP2040 offset | Size     | Content                                |
| ---------------- | ---------- | ------------- | -------- | -------------------------------------- |
| ROM4 window      | `$FA0000`  | `+0x0000`     | 128 KB   | ST-visible ROM-in-RAM                  |
| Planar slot 0    | `$FA0000`  | `+0x0000`     | 32 000 B | Ready/render target planar framebuffer |
| Planar slot 1    | `$FA7D00`  | `+0x7D00`     | 32 000 B | Ready/render target planar framebuffer |
| Random token     | `$FAFA00`  | `+0xFA00`     | 4 B      | Completion sync token                  |
| Token seed       | `$FAFA04`  | `+0xFA04`     | 4 B      | Seed for next token                    |
| Async mailbox    | `$FAFA20`  | `+0xFA20`     | 36 B     | Frame submit/ready status and timings  |
| Palette return   | `$FAFA80`  | `+0xFA80`     | 32 B     | 16 × STE `uint16_t` hardware colours   |
| Shared variables | `$FAFC00`  | `+0xFC00`     | 1 KB     | Boot-stub shared state                 |

ROM3 reads remain part of the command transport, so the second visible planar slot cannot live at `$FB0000`. Instead, ROM3 is used internally by the RP2040 as the second chunky staging buffer while both ST-visible planar slots stay in ROM4.

## Command reference

| ID   | Name                   | d3                    | d4             | d5               | Inline buf      | RP2040 action                                           |
| ---- | ---------------------- | --------------------- | -------------- | ---------------- | --------------- | ------------------------------------------------------- |
| 0x01 | `SDL_MD_INIT`          | `(width<<16)\|height` | `(bpp<<16)\|0` | 0                | —               | Clear buffers, reset palette, reset mailbox             |
| 0x02 | `SDL_MD_QUIT`          | 0                     | 0              | 0                | —               | Clear buffers, reset palette, reset mailbox             |
| 0x03 | `SDL_MD_SET_PALETTE`   | `(256<<16)\|0`        | 0              | 0                | 768 B (256×RGB) | Store palette for the next submitted frame              |
| 0x04 | `SDL_MD_BLIT_SURFACE`  | `(x<<16)\|y`          | `(w<<16)\|h`   | `(pitch<<16)\|0` | up to 1920 B    | Copy chunky rect into the current upload staging buffer |
| 0x05 | `SDL_MD_FILL_RECT`     | `(x<<16)\|y`          | `(w<<16)\|h`   | `(color<<16)\|0` | —               | Fill rect in the current upload staging buffer          |
| 0x06 | `SDL_MD_FLIP`          | `seq`                 | 0              | 0                | —               | Submit full-frame async conversion job                  |
| 0x07 | `SDL_MD_UPDATE_RECT`   | `(x<<16)\|y`          | `(w<<16)\|h`   | `seq`            | —               | Submit partial async conversion job                     |
| 0x08 | `SDL_MD_PING`          | `0x4D44534C`          | 0              | 0                | —               | Echo token (used for detection)                         |
| 0x09 | `SDL_MD_RELEASE_FRAME` | `seq`                 | 0              | 0                | —               | Mark a presented planar slot reusable                   |

A full 320×200 frame upload (`SDL_Flip`) uses 34 `SDL_MD_BLIT_SURFACE` commands (6 rows × 320 B = 1920 B each) followed by one `SDL_MD_FLIP(seq)`. A dirty-rect update (`SDL_UpdateRects`) sends only the changed rows via `SDL_MD_BLIT_SURFACE` then uses `SDL_MD_UPDATE_RECT(..., seq)` for the partial C2P (single rect) or `SDL_MD_FLIP(seq)` (multiple rects). The ST polls the mailbox on `Vsync()` or at the start of the next flip, presents any ready frame, then acknowledges it with `SDL_MD_RELEASE_FRAME(seq)`.

## Build prerequisites

### macOS

**1. ARM GNU Toolchain** (for RP2040 cross-compilation)

Download the macOS package from the [Arm Developer website](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads). Choose the `arm-none-eabi` variant for your host (Apple Silicon: `aarch64-apple-darwin`, Intel: `x86_64-apple-darwin`). Install the `.pkg` — it lands in `/Applications/ArmGNUToolchain/<version>/arm-none-eabi/`.

Tested with **15.2.rel1**. Version 14.x also works.

**2. atarist-toolkit-docker** (for Atari ST cross-compilation)

Install `stcmd` following the instructions at [github.com/sidecartridge/atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker). This provides `vasm`, `vlink`, and `m68k-atari-mint-gcc` via Docker.

**3. Other tools**

```bash
brew install cmake git python3
```

CMake 3.26 or later is required.

### Linux

Install `cmake`, `git`, `python3` from your package manager. Download the ARM GNU Toolchain `.tar.xz` and extract it to a permanent location, e.g. `/opt/arm-gnu-toolchain/`.

## Building

**1. Clone and initialise submodules**

```bash
git clone <repo-url> md-sdl
cd md-sdl
git submodule update --init --recursive
```

**2. Run the build script**

```bash
PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin \
  ./build.sh pico_w debug <your-uuid>
```

Replace `<your-uuid>` with the UUID from `desc/app.json` (or generate one with `uuidgen`). Use `release` instead of `debug` to disable UART output.

Adjust `PICO_TOOLCHAIN_PATH` to match your installed toolchain version and host platform.

**3. Output**

A successful build produces the following in `dist/`:

| File                    | Description                                   |
| ----------------------- | --------------------------------------------- |
| `<UUID>-v<version>.uf2` | RP2040 firmware to flash to the SidecarTridge |
| `<UUID>.json`           | App descriptor with version and MD5           |

## Flashing

With the SidecarTridge in BOOTSEL mode (hold BOOTSEL while plugging in USB), copy the UF2 to the mass-storage volume:

```bash
cp dist/<UUID>-v<version>.uf2 /Volumes/RPI-RP2/
```

Or use `picotool`:

```bash
picotool load dist/<UUID>-v<version>.uf2 --force
```

## Boot behaviour

MD/SDL does not render a boot banner into the ST planar framebuffer.
On boot, the RP2040 initializes the command/mailbox pipeline and then waits for
SDL commands.

## Verifying with UART (debug build)

Connect a debug probe to the SidecarTridge header (TX, RX, GND) and open a serial terminal at 115200 baud. On boot:

```
MD/SDL: ready, waiting for commands
```

Each command from the ST then appears as `SDL_MD_INIT: 320x200 bpp=8`, `SDL_MD_FLIP`, etc.

## Troubleshooting

| Symptom                        | Fix                                                                                                |
| ------------------------------ | -------------------------------------------------------------------------------------------------- |
| `arm-none-eabi-gcc not found`  | Set `PICO_TOOLCHAIN_PATH` to the `bin/` directory of your ARM GNU Toolchain install                |
| UF2 not found after build      | The RP build failed — scroll up for the first compiler or linker error                             |
| ST hangs on first `SDL_Flip()` | UART timeout — confirm the UF2 is flashed and check UART log for `ready, waiting for commands`     |
| Garbled colours                | Palette not sent before first flip — ensure `SDL_SetColors()` is called after `SDL_SetVideoMode()` |
| `stcmd` fails with "not a TTY" | Run with `pty=true` or use the `stcmd` wrapper script                                              |
| Screen stays black after flip  | Check `$FAFA00` — if it never matches `expected_token`, the bus sync is failing                    |

## License

Source code is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for the full text.
