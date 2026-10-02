---
platform: Vector-06C
topic: io
status: verified
source: multiple
---

# Vector-06C I/O Port Map

## Overview

The Vector-06C uses IN/OUT instructions to access I/O ports (8-bit port space, 0x00–0xFF).
Ports are grouped by function: PIA1, PPI2, Timer, Palette, Kvas (paging), AY, FDC.

## PIA1 — Parallel Interface Adapter (ports 0x00–0x03)

| Port | Direction | Function |
|------|-----------|----------|
| 0x00 | OUT | Control Word / Port C bit-set-reset |
| 0x01 | IN/OUT | Port C (tape, shift keys, keyboard status) |
| 0x02 | IN/OUT | Port B (video mode, border, keyboard read) |
| 0x03 | IN/OUT | Port A (vertical scroll value, keyboard row mask) |

### Control Word (port 0x00)

Written byte configures PIA1 direction and mode:

| Bit | Function |
|-----|----------|
| 7 | Mode set (1 = mode set active, 0 = port C bit operation) |
| 6 | Port A direction (1 = input, 0 = output) |
| 5 | Port C upper direction (1 = input, 0 = output) |
| 4 | Port B mode (1 = input, 0 = output) |
| 3 | Port B direction (1 = input, 0 = output) |
| 2 | Port C lower direction (1 = input, 0 = output) |
| 1 | Port A mode (1 = input, 0 = output) |
| 0 | Port C bit operation: 1 = set, 0 = reset (when bit 7 = 0) |

Default CW after reset: 0x08.

### Port C (0x01)

| Bit | Function |
|-----|----------|
| 0 | Tape output |
| 3 | Rus/Lat indicator (0 = Rus, 1 = Lat) |
| 4–7 | Keyboard shift status / tape input (when configured as input) |

Bit 4–7 (input mode):
- Bit 4: tape input (from tape player sample)
- Bit 5: Shift (SS) status (0 = pressed)
- Bit 6: Ctrl (US) status (0 = pressed)
- Bit 7: Rus (0 = pressed)

### Port B (0x02)

| Bit | Function |
|-----|----------|
| 0–3 | Border color index (0–15) — separate register, not palette cell |
| 4 | Video mode: 0 = 256-column, 1 = 512-column |
| 5–7 | Keyboard scan return (when CW bit 1 = 1) |

**Important:** Port B does NOT set the palette index. The palette index comes from
pixel data (bit planes). See `video.md` for palette mechanism details.

**CONFLICT with TIMSoft:** TIMSoft vector_techinfo.md §6 claims port 02h carries
"математический код цвета" (mathematical color code). Both emulators show port 02h
controls border and 512px mode only. See `verification.md` §7.3.2.

### Port A (0x03)

Vertical scroll value (0–255). Also used as keyboard row mask during keyboard scanning.
Stored as PA register.

## PPI2 — Second Parallel Interface (ports 0x04–0x07)

| Port | Function |
|------|----------|
| 0x04 | Control Word 2 |
| 0x05 | Port C2 |
| 0x06 | Port B2 |
| 0x07 | Port A2 (Covox DAC output) |

PPI2 is used for secondary I/O. Port A2 (0x07) serves as a Covox (8-bit DAC) output.

**Note:** Covox is an expansion, not part of the base Vector-06C configuration.
Confirmed in VSDL: `Covox() { return this->PA2; }` (port 0x07 = PA2).

## Timer — KR580VI53 (ports 0x08–0x0B)

| Port | Timer Address | Function |
|------|--------------|----------|
| 0x08 | 3 | Control word |
| 0x09 | 2 | Counter 2 data |
| 0x0A | 1 | Counter 1 data |
| 0x0B | 0 | Counter 0 data |

Port-to-timer-address mapping (VSDL: `timer.write(~port & 3, w8)`):
- port 0x08 → addr 3 (control word)
- port 0x09 → addr 2 (counter 2)
- port 0x0A → addr 1 (counter 1)
- port 0x0B → addr 0 (counter 0)

Used for system timing and sound generation (3 square-wave tone channels).
Timer clock: ~1.5 MHz (pixel clock / 8).

**Note:** The VI53 has NO hardware noise generator. Noise effects must be created
programmatically. See `sound.md`.

Channel initialization (VSDL `vio.h`):
- Channel 0: port 0x0B, CW = 36h
- Channel 1: port 0x0A, CW = 76h
- Channel 2: port 0x09, CW = B6h

## Palette (ports 0x0C–0x0F)

| Port | Direction | Function |
|------|-----------|----------|
| 0x0C | W | Write physical color to palette register |
| 0x0D | W | Mirror of 0x0C |
| 0x0E | W/R | Mirror of 0x0C / Joystick C |
| 0x0F | W/R | Mirror of 0x0C / Joystick C |

All four ports write the same palette register. The palette index (cell 0–15) is
determined by the 4-bit pixel color code from the bit planes, NOT by any port write.

Palette byte format: `RRR GGG BB`
- Bits 0–2: Red (0–7)
- Bits 3–5: Green (0–7)
- Bits 6–7: Blue (0–3)

See `video.md` for full palette mechanism details.

## Kvas — Memory Paging Controller (port 0x10)

| Bits | Function |
|------|----------|
| 0–1 | Map page select |
| 2–3 | Stack page select |
| 4 | Stack paging mode enable |
| 5 | Map paging mode enable |

See `memory.md` for full details on address translation.

## AY-3-8912 — Sound Generator (ports 0x14–0x15)

| Port | Function |
|------|----------|
| 0x14 | Data read/write (register addressed by current index) |
| 0x15 | Address (register index, 0x00–0x0F) |

Write sequence: OUT 0x15 with register number, then OUT 0x14 with value.
Read sequence: OUT 0x15 with register number, then IN 0x14.

**Note:** AY-3-8912 is an **expansion**, not part of the base Vector-06C configuration.
Base sound is provided by the VI53 timer (3 tone channels, no noise).

See `sound.md` for AY register details.

## FD1793 — Floppy Disk Controller (ports 0x18–0x1C)

| Port | Read | Write |
|------|------|-------|
| 0x18 | Data register | Data register |
| 0x19 | Sector register | Sector register |
| 0x1A | Track register | Track register |
| 0x1B | Status register | Command register |
| 0x1C | — (not implemented) | Control register |

## Unmapped Ports

Reading unmapped ports returns 0xFF.
Writing to unmapped ports is silently ignored.

## Port Summary Table

| Range | Device |
|-------|--------|
| 0x00–0x03 | PIA1 (keyboard, video, scroll, tape) |
| 0x04–0x07 | PPI2 (Covox, secondary I/O) |
| 0x08–0x0B | Timer (KR580VI53) |
| 0x0C–0x0F | Palette |
| 0x10 | Kvas (memory paging) |
| 0x14–0x15 | AY-3-8912 (expansion) |
| 0x18–0x1C | FD1793 (FDC) |
| Other | Unmapped (read = 0xFF, write = noop) |

## Hardware vs Emulator Behavior

- **Port write timing (VSDL)**: Port writes are deferred to specific scanline moments
  (`commit_time`), not applied instantly. This is an emulator model of beam-racing behavior.
- **Covox (VSDL)**: Covox on PA2 (port 0x07) is an expansion. The VSDL code confirms
  `Covox() { return this->PA2; }`.
- **Joystick (ports 0x0E/0x0F)**: Joystick support on palette mirror ports is implemented
  in emulators; hardware implementation details are UNVERIFIED.

See `verification.md` for detailed evidence and source attribution.
