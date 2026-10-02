---
platform: Vector-06C
topic: video
status: verified
source: multiple
---

# Vector-06C Video System

## Overview

The Vector-06C video system generates a 512×256 or 256×256 pixel display with a
16-color palette (from 256 possible hues).

## Video Modes

| Mode | Resolution | Colors | Pixels/Byte | VRAM Usage |
|------|-----------|--------|-------------|------------|
| 256-column | 256×256 | 16 | 8 | 32 KB (all 4 planes) |
| 512-column | 512×256 | 4 | 4 | 32 KB (all 4 planes) |

Mode selection: PIA1 port B bit 4 (port 0x02).
- 0 = 256-column mode
- 1 = 512-column mode

Status: VERIFIED_BY_CODE (both VSDL and EMU80)

## Screen Memory Layout

Screen memory occupies the upper 32 KB of the 64 KB main RAM (8000h–FFFFh), organized
as 4 bit planes:

```
8000h–9FFFh   Plane 3 (8 KB, bit 3 of color — MSB)
A000h–BFFFh   Plane 2 (8 KB, bit 2 of color)
C000h–DFFFh   Plane 1 (8 KB, bit 1 of color)
E000h–FFFFh   Plane 0 (8 KB, bit 0 of color — LSB)
```

The CPU accesses these as ordinary RAM. The video controller reads them as bit planes.

## Screen Layout

```
Total raster: 768 × 312 (pixel clock units)
Visible area: 512 × 256 pixels
Border: 32 pixels left/right, 16 pixels top/bottom
Frame buffer: 576 × 288 pixels (visible + border)
```

## VRAM Addressing

### 256-column mode (16 colors)

Each pixel is 1 bit from each of the 4 planes. One byte per plane = 8 horizontal pixels.

```
For pixel (X, Y), where X=0..255, Y=0..255 (Y=0 = top):

byte_offset = (X / 8) × 0100h + (255 - Y)
bit_number  = 7 - (X mod 8)
address     = base_plane + byte_offset
```

Where `base_plane` = 8000h / A000h / C000h / E000h for planes 3/2/1/0.

Bit 7 = leftmost pixel in each byte.

Status: VERIFIED_BY_CODE (both emulators)

### 512-column mode (4 colors)

In 512-column mode, planes are paired for even/odd X pixels:

- **Even X** (0, 2, 4, ...): color from planes 2+3 (A000h + 8000h), 2 bits per pixel
- **Odd X** (1, 3, 5, ...): color from planes 0+1 (E000h + C000h), 2 bits per pixel

```
byte_group  = X / 16
byte_offset = byte_group × 0100h + (255 - Y)
bit_number  = 7 - ((X / 2) & 7)

If X even: bit0 → A000h (plane 2), bit1 → 8000h (plane 3)
If X odd:  bit0 → E000h (plane 0), bit1 → C000h (plane 1)
```

Status: VERIFIED_BY_CODE (VSDL `filler.cpp fill3()`, EMU80 `renderLine()`)

### Plane Weight Table (512-column mode)

| Plane | Address | For even X | For odd X |
|-------|---------|-----------|-----------|
| 0 | E000h | — | bit 0 (weight 1) |
| 1 | C000h | — | bit 1 (weight 2) |
| 2 | A000h | bit 0 (weight 1) | — |
| 3 | 8000h | bit 1 (weight 2) | — |

### Plane Numbering Note

Two numbering conventions exist:
- **Documentary**: E000h=0, C000h=1, A000h=2, 8000h=3
- **Physical memory order**: 8000h=lowest, E000h=highest

Both emulators assign planes identically regardless of naming.

## Scrolling

Vertical scroll value: PIA1 port A (port 0x03), range 0–255.
```
vramRow = (scrollValue + visibleLine) & 0xFF
```

Status: VERIFIED_BY_CODE (VSDL `ScrollStart() → PA`, EMU80 `setPortA() → m_lineOffset`)

## Pixel Color Formation (256-column mode)

Color index (0–15) is formed from 4 bits, one from each plane:

```
color = ((plane3_byte >> bit_pos) & 1) << 3   // bit 3 (weight 8)
      | ((plane2_byte >> bit_pos) & 1) << 2   // bit 2 (weight 4)
      | ((plane1_byte >> bit_pos) & 1) << 1   // bit 1 (weight 2)
      | ((plane0_byte >> bit_pos) & 1)        // bit 0 (weight 1)
```

Status: VERIFIED_BY_CODE (EMU80 `renderLine()`:
`((byte0>>7)&1) | ((byte1>>6)&2) | ((byte2>>5)&4) | ((byte3>>4)&8)`)

## Palette

16 colors, each defined by a byte written to port 0x0C (mirrored at 0x0D–0x0F):

```
Palette byte: RRR GGG BB
  bits 0–2: Red (0–7)
  bits 3–5: Green (0–7)
  bits 6–7: Blue (0–3)
```

Total: 256 possible hues (8 × 8 × 4).

Status: VERIFIED_BY_CODE (VSDL `vio.h commit_palette()`, EMU80 `setPaletteColor()`,
TIMSoft `vector_techinfo.md`)

### Palette Write Mechanism

The palette (K155RU2) is a RAM of 16 cells × 8 bits. The **address** (cell index 0–15)
is determined by the 4-bit pixel color code from the bit planes. The **data** (physical
color) is written through port 0x0C.

Write process:
1. Pixel data (4 bits from bit planes) → palette address (0–15)
2. `OUT 0x0C` → write physical color into palette cell at current pixel-derived address

The palette is read synchronously with the beam: when the beam passes a pixel position,
the palette is read at the address from pixel data. If `OUT 0x0C` occurs at that moment,
the new value is written to that cell.

**Important:** Port B (0x02) does NOT set the palette index. The palette index comes
from pixel data. Port B controls border color (bits 0–3) and 512px mode (bit 4).

**CONFLICT with TIMSoft:** vector_techinfo.md §6 states "порт 02h — код математического
цвета" (port 02h = mathematical color code). Both emulators show the palette index comes
from pixel data, not port 02h. Requires hardware verification.

Status: VERIFIED_BY_CODE (mechanism confirmed by both emulators)

### Palette Ports

| Port | Direction | Function |
|------|-----------|----------|
| 0x0C | W | Write physical color to palette |
| 0x0D | W | Mirror of 0x0C |
| 0x0E | W/R | Mirror of 0x0C / Joystick C |
| 0x0F | W/R | Mirror of 0x0C / Joystick C |

All four ports write the same palette register.

### Boot Palette (Yellow/Blue "Yeblette")

```
Index with bit 1 set: Blue  (R=0, G=0, B=2)
Index with bit 1 clear: Yellow (R=5, G=5, B=0)
```

## Border Color

Border color index: PIA1 port B bits 0–3 (port 0x02, masked with 0x0F).

The border has its own 4-bit register — it is NOT tied to a specific palette cell.
The border color register is independent from the pixel palette.

Status: VERIFIED_BY_CODE (VSDL `BorderIndex() { return PB & 0x0f; }`,
EMU80 `setPortB() { m_borderColor = value & 0x0f; }`)

In 512-column mode, the border uses 2 bits from the 4-bit register:
- `borderColor & 0x03` — for one group of pixels
- `borderColor & 0x0c` — for another group

## Planes

The Vector-06C has 4 VRAM bit planes:
- Plane 3: base at 0x8000 (bit 3 of color, MSB)
- Plane 2: base at 0xA000 (bit 2 of color)
- Plane 1: base at 0xC000 (bit 1 of color)
- Plane 0: base at 0xE000 (bit 0 of color, LSB)

In 256-column mode, all 4 planes contribute to 16-color pixels.
In 512-column mode, planes are paired for even/odd X (see above).

## Timing

- Frame rate: ~50.08 Hz (VERIFIED_BY_CODE)
- Raster: 312 lines total
- Visible lines: 256 (lines 40–295 in raster coordinates)
- VSync: lines 0–21
- Upper border: lines 22–39
- Lower border: lines 296–311
- Pixels per line: 768

## Mid-Frame Palette Changes (Racing the Beam)

The palette can be changed during frame scanout, allowing more than 16 colors on screen.
TIMSoft describes up to 256 border colors using this technique.

Status: VERIFIED_BY_PRIMARY_SOURCE (TIMSoft, "Секреты Вектора"); confirmed by `clrs.asm`

> **Inspection of VRAM alone is insufficient for raster effects.** A static VRAM read
> cannot reveal mid-frame `OUT 0x0C–0x0F` palette writes. To analyse racing-the-beam
> code, use the runtime beam tools (Stage 6.27):
> `debug_get_beam_state` → `debug_get_raster_events` → `debug_get_screen_snapshot`.
> - `debug_get_beam_state` — beam position (`frame`, `raster_line`, `v_cycle_in_frame`/
>   `_in_line`, `rpixel`), the CPU `pc` beside the beam, and the palette entry being
>   shifted out (`palette_index`/`palette_value`) plus the hardware `border_index`.
> - `debug_get_raster_events` — ring of `OUT` events, each correlated to the exact beam
>   position and `pc`/`port`/`value` at execution time (recorded on the emulation thread).
> - `debug_get_screen_snapshot` — the composed TV framebuffer as PNG, revealing the
>   actual on-screen result rather than a VRAM reconstruction.
> Timing constants (`line_v_cycles=768`, `frame_lines=312`, `frame_v_cycles=239616`) come
> from the emulator's video model; MCP never computes them.

## Hardware vs Emulator Behavior

- **Deferred port commits (VSDL)**: Port writes are not applied instantly but deferred
  to specific scanline moments (`commit_time`).
- **Palette commit timing (VSDL)**: Palette updates when beam passes the pixel position.
- **Line offset latching (EMU80)**: Scroll register is latched at a specific frame moment.
- **512px mode latching (EMU80)**: 512px mode is latched at frame start.
- **BwMap (EMU80)**: B/W conversion table — emulator-specific feature.

See `verification.md` for detailed evidence and source attribution.
