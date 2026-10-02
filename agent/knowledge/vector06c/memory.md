---
platform: Vector-06C
topic: memory
status: verified
source: multiple
---

# Vector-06C Memory System

## Physical Memory

Total addressable RAM: 64 KB (0000h–FFFFh).

The screen RAM (32 KB, 8000h–FFFFh) is the **upper half** of the main 64 KB RAM.
It is NOT additional memory beyond the 64 KB. The CPU accesses it as ordinary memory;
the video controller reads it as 4 bit planes.

TIMSoft confirms: "В БПЭВМ используется общая оперативная память для микропроцессора
и контроллера графического дисплея объёмом 64 Кбайта".

### Expansion Memory

Bigram (Квазиск) controller adds up to 256 KB of expansion RAM, managed through
banking at I/O port 0x10. This is separate from the base 64 KB.

```c
// VSDL memory.h
#define TOTAL_MEMORY (64 * 1024 + 256 * 1024)  // 327680 bytes = 64 KB base + 256 KB expansion
```

## Memory Map

```
0000h–00FFh   Interrupt vector table (256 bytes, 64 entries × 4 bytes)
0100h–7FFFh   Lower RAM — user program space (32 KB)
8000h–9FFFh   Screen bit plane 3 (8 KB, bit 3 of color)
A000h–BFFFh   Screen bit plane 2 (8 KB, bit 2 of color)
C000h–DFFFh   Screen bit plane 1 (8 KB, bit 1 of color)
E000h–FFFFh   Screen bit plane 0 (8 KB, bit 0 of color)
```

The upper 32 KB (8000h–FFFFh) serves dual purpose:
- **CPU perspective**: Ordinary RAM, readable and writable
- **Video controller perspective**: 4 bit planes for pixel color generation

There is no hardware protection preventing CPU from writing to screen RAM.

## Screen RAM Plane Organization

Each pixel color (0–15) is formed from 4 bits, one from each plane:

| Plane | Address Range | Color Bit | Weight |
|-------|--------------|-----------|--------|
| 0 | E000h–FFFFh | bit 0 | 1 |
| 1 | C000h–DFFFh | bit 1 | 2 |
| 2 | A000h–BFFFh | bit 2 | 4 |
| 3 | 8000h–9FFFh | bit 3 | 8 |

Status: VERIFIED_BY_CODE (both VSDL `filler.cpp` and EMU80 `Vector.cpp renderLine()`)

### Plane Numbering Note

Two numbering conventions exist in documentation:
- **Documentary** (TIMSoft): E000h=plane 0, C000h=plane 1, A000h=plane 2, 8000h=plane 3
- **Physical memory order**: 8000h is lowest address, E000h is highest

Both emulators (VSDL and EMU80) use the same assignment regardless of naming convention.

## Address Translation (Bigram Paging)

The Vector-06C supports memory paging through the "Bigram" (Кваз) controller at I/O port 0x10.

### Control Register (port 0x10)

| Bits | Function |
|------|----------|
| 0–1 | Map page select: page_map = ((bits[1:0] + 1) << 16) |
| 2–3 | Stack page select: page_stack = ((bits[3:2] + 1) << 16) |
| 4 | Stack paging mode enable |
| 5 | Map paging mode enable |

### Address Mapping

Without paging (default):
- Physical address = logical address (1:1 mapping for 0x0000–0xFFFF)

With stack paging (mode_stack = 1, stack access):
- Physical = logical + page_stack
- Applies only to stack operations (PUSH, POP, CALL, RET, XTHL, etc.)

With map paging (mode_map = 1, address 0xA000–0xDFFF):
- Physical = logical + page_map
- Applies only to accesses in range 0xA000–0xDFFF

### Physical Address Calculation

```
bigram_addr = bigram_select(logical_addr, is_stack_access)
physical_addr = tobank(bigram_addr)
```

The `tobank` function interleaves address bits for DRAM banking:
```
physical = (addr & 0x78000) | ((addr << 2) & 0x7FFC) | ((addr >> 13) & 3)
```

Status: VERIFIED_BY_CODE (VSDL `memory.cpp control_write()`, EMU80 `VectorAddrSpace`)

## Boot ROM

On reset, a 2 KB boot ROM is mapped at 0x0000–0x07FF:
- Boot ROM overlays the first bytes of RAM
- After the boot sequence completes, boot ROM detaches
- Standard SP after boot: 0xC300

Status: VERIFIED_BY_CODE (VSDL `board.cpp` BLKVVOD/BLKSBR modes, EMU80 `m_romEnabled`)

## ROM Loading Conventions

| Extension | Load Address | Description |
|-----------|-------------|-------------|
| .rom | 0x0100 | After interrupt vector table (0x0000–0x00FF) |
| .r0m | 0x0000 | Raw memory image |
| .bin | varies | Binary data |

## Hardware vs Emulator Behavior

- **Interleaved memory (VSDL)**: VSDL stores pixels in interleaved format (`tobank()`)
  for rendering optimization. The CPU sees standard linear addresses.
- **ERAM (EMU80)**: Extended RAM system in EMU80 may be an emulator expansion, not
  original hardware. Status: UNVERIFIED for original hardware.
- **Bigram paging**: Both emulators implement it. The `tobank()` interleaving is an
  emulator optimization for DRAM access patterns.

See `verification.md` for detailed evidence and source attribution.
