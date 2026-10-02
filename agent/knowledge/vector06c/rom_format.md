---
platform: Vector-06C
topic: rom_format
status: verified
source: multiple
---

# Vector-06C ROM Formats

## File Extensions

| Extension | Load Address | Description |
|-----------|-------------|-------------|
| .rom | 0x0100 | Standard ROM — loaded after interrupt vector table |
| .r0m | 0x0000 | Raw memory image — loaded at address 0 |
| .bin | varies | Generic binary data |

Status: VERIFIED_BY_PRIMARY_SOURCE (TIMSoft, emulator conventions)

## ROM Loading Conventions

### .rom files

The standard Vector-06C ROM format. Loaded at address 0x0100 because:
- Addresses 0x0000–0x00FF are reserved for the interrupt vector table
- The vector table contains 64 entries × 4 bytes = 256 bytes
- ROM code starts at 0x0100

This is a heritage from CP/M/MicroDOS, where .COM format programs were loaded at 0x0100.

### .r0m files

Raw memory images loaded at address 0x0000. These contain a complete memory snapshot
including the vector table.

### MAP files

MAP files use the Z88DK linker output format. See `debugger/docs/z88dk_map.md` for the complete format specification.

Brief summary:
```
symbol_name           = $ADDRESS ; type, visibility, def, module, section, source_location
```

- `$ADDRESS` — hex address with `$` prefix (4 uppercase digits, e.g. `$0100`)
- `type` — `addr` (code/data label) or `const` (compile-time constant, NOT a memory address)
- `visibility` — `public` (exported) or `local` (internal)

A MAP file is typically paired with a ROM:
```
program.rom
program.map
```

MAP files are stored in the ROM Library alongside their ROMs.

**Important:** `const` entries are link-time constants, not memory addresses. Loading them as addresses would be incorrect. For example, `__head = $0100 ; const` does not mean there is code at 0x0100 — it means the program starts at 0x0100.

## Boot Sequence

1. On reset, 2 KB boot ROM is mapped at 0x0000–0x07FF
2. Boot ROM initializes hardware and loads the main program
3. Boot ROM detaches, revealing RAM at 0x0000
4. Standard initial SP after boot: 0xC300
5. Program entry typically at 0x0100

Status: VERIFIED_BY_CODE (VSDL `board.cpp` BLKVVOD/BLKSBR modes, EMU80 `m_romEnabled`)

## Interrupt Vector Table

```
Address 0x0000–0x00FF:
  Entry 0:  RST 0 handler  (address 0x0000)
  Entry 1:  RST 1 handler  (address 0x0008)
  ...
  Entry 7:  RST 7 handler  (address 0x0038) — used for INTR interrupt (VBlank)
  ...
  Entry 63: last vector    (address 0x00FC)
```

Each entry is a 4-byte jump instruction (JP addr).

## ROM Size

Typical ROM sizes:
- 4 KB – small utilities
- 8–16 KB — standard programs
- 32–48 KB — large applications (approaching available user space)

Maximum user program space: 0x0100–0xBFFF (~48 KB), limited by screen RAM at 0x8000
in 256-column mode, or 0x0100–0x7FFF (32 KB) in 512-column mode.

## Hardware vs Emulator Behavior

- **Boot ROM mechanism**: Both emulators implement boot ROM overlay at reset and
  detachment after boot. The actual hardware boot ROM size and behavior are confirmed.
- **.rom load address (0x0100)**: This is a convention shared by both emulators and
  confirmed by TIMSoft as primary source.
- **MAP files**: Z88DK convention; not hardware-related.

See `verification.md` for detailed evidence and source attribution.
