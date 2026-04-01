# MD-SDL: SDL 1.2 Video Offload Microfirmware for SidecarTridge Multi-device

## Project Goal

Create a dedicated microfirmware that acts as a **graphics coprocessor** for SDL 1.2 applications on the Atari ST (Doom, Hexen, Heretic, and other SDL 1.2 ports). The RP2040 will handle rendering, compositing, and C2P conversion; the ST will only send draw commands/surfaces and copy the returned planar buffer to screen memory.

This builds directly on the official **md-sprites-demo**, which already returns ready-to-display frames at good speed on a stock ST.

## Key Requirements

- Detect MD presence from ST-side SDL driver and fall back to software rendering if absent.
- Support common SDL 1.2 video operations: init, set palette, blit, fillrect, update rect, flip.
- Perform final C2P on the RP2040 so the returned buffer is in Atari ST planar format (low-res 16-color).
- Keep bus responsive at all times (core 0 + PIO).

## Starting Points & References

- **Base**: `md-microfirmware-template`[](https://github.com/sidecartridge/md-microfirmware-template)
- **Strong reference**: `md-sprites-demo`[](https://github.com/sidecartridge/md-sprites-demo) – study how it composites sprites/tiles and returns the buffer.
- Official programming guide: https://docs.sidecartridge.com/sidecartridge-multidevice/programming/
- Existing Drives Emulator and ROM Emulator for protocol patterns.

## Architecture Overview

- **RP2040 side**:
  - Core 0/PIO: Cartridge bus handling via `tprotocol`.
  - Core 1: SDL command handler + internal chunky surfaces + compositing + fast native C2P.
- **ST side**: Patch SDL 1.2 video backend to route calls to MD commands when detected.
- Communication: Binary payloads for speed (raw surface data, command structs). Avoid heavy JSON for hot path.

## Immediate Next Steps

1. Clone template + sprites-demo and build the demo to understand buffer handoff.
2. Implement minimal full-frame upload → render/C2P → return planar buffer test.
3. Add detection routine (magic signature + ping) in ST code.
4. Extend with individual SDL commands incrementally.
5. Test ST-side changes first in Hatari with dummy detection, then on real hardware.

## Risks / Notes

- Memory is limited (264 KB SRAM) – be careful with multiple surfaces.
- Performance target: noticeable improvement on stock 8 MHz ST for 2D SDL games.
- Future: Could add sprite/tile acceleration layers if useful.

## Related Conversation Context

User already has SDL Doom/Hexen running (slow on ST, good on TT). Goal is reusable acceleration for multiple titles.
