---
platform: Vector-06C
topic: architecture
status: partially_verified
source: multiple
---

# Vector-06C Architecture

## Overview

Vector-06C (Вектор-06Ц) is a Soviet home computer based on the KR580VM80A (Intel 8080) CPU.

## Key Components

| Component | Chip | Role |
|-----------|------|------|
| CPU | KR580VM80A (i8080) | Central processor, clock UNVERIFIED (commonly cited: 3 MHz) |
| PIA1 | KR580VV55A (i8255) | System peripheral interface |
| PPI2 | KR580VV55A (i8255) | Auxiliary peripheral interface |
| Timer | KR580WI53 (i8253) | Programmable interval timer |
| Memory | RAM 64 KB + expansion | Main memory; screen RAM (32 KB) is upper half of 64 KB |
| Screen RAM | 32 KB (8000h–FFFFh) | 4 bit planes within upper 32 KB of main RAM |
| AY-3-8912 | Programmable sound generator | 3-channel sound (expansion, not base config) |
| FD1793 | Floppy disk controller | Disk drive interface |

## Bus Architecture

CPU communicates with peripherals through I/O port addressing (IN/OUT instructions).
Memory is accessed through a 16-bit address bus with optional paging (Bigram mode).

## Reset Behavior

On power-on reset (BLKVVOD mode):
- PC = 0x0000
- SP is preserved by boot ROM (typically 0xC300)
- Boot ROM is mapped at 0x0000–0x07FF (2 KB)
- Boot ROM detaches after first jump to user code

On BLKSBR reset:
- Similar to power-on but preserves more state

## Interrupt Architecture

- Single interrupt line (INTR)
- Interrupt enable flag (INTE/IFF)
- RST 0x38 (restart vector at 0x0038) is the standard interrupt handler entry
- Timer generates frame interrupts (50 Hz)

## Timing

- CPU clock: UNVERIFIED — commonly cited as 3 MHz, but emulators do not model absolute frequency; CPU runs in pixel-clock units
- Pixel clock: 12 MHz (VERIFIED_BY_CODE: 50×768×312 = 11,980,800 ≈ 12 MHz)
- Timer clock: pixel clock / 8 ≈ 1.5 MHz (VERIFIED_BY_CODE)
- Frame rate: ~50.08 Hz (VERIFIED_BY_CODE)
- Raster: 768 columns × 312 lines (in pixel clock units)

## Address Space

```
0x0000–0x00FF  Interrupt vector table
0x0100–0x7FFF  User RAM (lower 32 KB)
0x8000–0x9FFF  Screen bit plane 3 (8 KB, bit 3 of color)
0xA000–0xBFFF  Screen bit plane 2 (8 KB, bit 2 of color)
0xC000–0xDFFF  Screen bit plane 1 (8 KB, bit 1 of color)
0xE000–0xFFFF  Screen bit plane 0 (8 KB, bit 0 of color)
```

The upper 32 KB (8000h–FFFFh) is the screen-related memory, but it is NOT separate
from main RAM — it is the upper half of the 64 KB addressable RAM. The CPU accesses
it as ordinary memory. The video controller reads it as 4 bit planes.

Note: Actual VRAM layout depends on video mode (256 or 512 column) and Bigram paging.

## Data Flow

```
CPU ←→ Memory (64 KB address space; screen RAM 8000h–FFFFh is upper half)
CPU ←→ I/O ports (256 ports, IN/OUT instructions)
PIA1 ←→ Keyboard, cassette, system control
PPI2 ←→ Covox, auxiliary I/O
Timer ←→ Frame timing, sound
AY-3-8912 ←→ Sound synthesis (expansion)
FD1793 ←→ Floppy disk
```

## Hardware vs Emulator Behavior

The following facts are confirmed by emulator code (VSDL, EMU80) and may differ from
original hardware:

- **Wait states**: EMU80 uses an explicit wait-state table (`VectorCpuWaits`); VSDL
  uses pixel-clock timing (`filler.fill(instr_time << 2)`). Neither may exactly match
  real hardware arbitration.
- **Bigram paging**: Implemented in both emulators. The `tobank()` interleaved address
  transformation is an emulator optimization; the CPU sees standard addresses.
- **CPU frequency**: Emulators run in pixel-clock units, not absolute MHz. The commonly
  cited 3 MHz figure is UNVERIFIED for original hardware.
- **AY-3-8912**: Confirmed in both emulators as an expansion. Not part of base Vector-06C.
- **ERAM (EMU80)**: Extended RAM in EMU80 may be an emulator expansion, not original hardware.

See `verification.md` for detailed evidence and source attribution.
