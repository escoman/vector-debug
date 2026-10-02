---
platform: Vector-06C
topic: keyboard
status: verified
source: multiple
---

# Vector-06C Keyboard

## Overview

The Vector-06C keyboard is an 8×8 matrix scanned through PIA1 ports.
The matrix encodes approximately 64 keys plus 3 shift keys.

## Matrix Layout

```
  │ 7   6   5   4   3   2   1   0
──┼───────────────────────────────
7 │SPC  ^   ]   \   [   Z   Y   X
6 │ W   V   U   T   S   R   Q   P
5 │ O   N   M   L   K   J   I   H
4 │ G   F   E   D   C   B   A   @
3 │ /   .   =   ,   ;   :   9   8
2 │ 7   6   5   4   3   2   1   0
1 │F5  F4  F3  F2  F1  AP2 CTP ^\ -
0 │DN  RT  UP  LT  ЗАБ ВК  ПС  TAB
```

Column names: Russian labels (ПС = Enter, ЗАБ = Backspace, ВК = input, AP2 = Esc, CTP = F7/Screen).

## Scanning Mechanism

1. CPU writes row select mask to PIA1 Port A (port 0x03).
2. CPU reads Port B (port 0x02) to get column data.
3. Each bit in the returned byte corresponds to a column (0 = pressed, active low).

**Note:** Port A (port 0x03) is shared with the vertical scroll register. During keyboard
scanning, the row mask temporarily replaces the scroll value. The keyboard read function
in the emulator reads `PA` directly:

```c
int read(int rowbit) {
    int result = 0;
    for (int i = 0; i < 8; ++i) {
        if ((rowbit & 1) != 0)
            result |= matrix[i];
        rowbit >>= 1;
    }
    return (~result) & 0xFF;
}
```

Status: VERIFIED_BY_CODE (VSDL `keyboard.h`, EMU80 `Vector.h m_keyMatrix[8][8]`)

## Shift Keys

Shift keys are not part of the matrix. They are read from Port C (port 0x01):

| Key | Signal | Port C bit | Active |
|-----|--------|-----------|--------|
| Shift (SS) | Left/Right Shift | Bit 5 | 0 = pressed |
| Ctrl (US) | Control | Bit 6 | 0 = pressed |
| Rus | Rus/Lat toggle | Bit 7 | 0 = pressed |

These bits are read when PIA1 CW bit 3 = 1 (Port C upper configured as input).

Status: VERIFIED_BY_CODE (VSDL `vio.h input(0x01)`, EMU80 `Vector.h`)

## Key Encoding

Each key is encoded as a (column, bit) pair:
- Column: 0–7 (selects which matrix row)
- Bit: one of {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80}

Example: Space = column 7, bit 0x80 → encoded as 0x780.

## Special Keys

| Key | Function |
|-----|----------|
| F11 | Hard reset |
| F12 | Soft reset |
| Pause/Break | Terminate (set terminate flag) |

## Key Injection via MCP Debugger

The debugger exposes the virtual keyboard to the analysis agent through four
MCP tools (this is a **debugger/emulator** capability, not hardware):

| Tool | Purpose |
|------|---------|
| `debug_list_keys` | Authoritative table of accepted key names (case-insensitive) with SDL scancode + legend |
| `debug_press_key` | Press and **hold** a key (matrix bit or modifier latch stays set) |
| `debug_release_key` | Release a previously held key |
| `debug_type_key` | Full tap: press → hold ~120 ms → release |

Key names follow the matrix above: letters `A`–`Z`, digits `0`–`9`,
`SPACE`, `TAB`, `ENTER` (ВК), `BACKSPACE` (ЗАБ), `ESCAPE` (АР2), arrows
`UP`/`DOWN`/`LEFT`/`RIGHT`, function keys `F1`–`F8`, punctuation
(`MINUS`, `EQUALS`, `LBRACKET`, `RBRACKET`, `BACKSLASH`, `SEMICOLON`,
`APOSTROPHE`, `COMMA`, `PERIOD`, `SLASH`, `GRAVE`), and modifiers `SS`
(shift), `US` (ctrl), `RUS` (RU/LAT — an F6 pulse toggle). `SHIFT`/`CTRL`
are aliases for `SS`/`US`. Call `debug_list_keys` for the exact accepted set.

**Timing constraint (important):** the ROM samples the matrix only while the
CPU is running and polls the keyboard port (Port A mask → Port B read). A
`debug_type_key`/`debug_press_key` while **paused** latches the key but the ROM
will not observe it until execution resumes — call `debug_run` first. For a
key+modifier combination (e.g. shifted symbol) use the primitives: hold `SS`
via `debug_press_key`, tap the base key with `debug_type_key`, then release
`SS` via `debug_release_key`.

Status: EMULATOR_FEATURE (VSDL debugger `IDebugBackend::pressKey/releaseKey` →
`Keyboard::apply_key`)

## Tape I/O

- Tape output: PIA1 Port C bit 0
- Tape input: PIA1 Port C bit 4 (read when CW bit 3 = 1)

Status: VERIFIED_BY_CODE (VSDL `vio.h TapeOut(): PC & 1`, `tape.sample() << 4`)

## Hardware vs Emulator Behavior

- **Keyboard matrix**: Both VSDL (`keyboard.h`) and EMU80 (`Vector.h m_keyMatrix[8][8]`)
  implement identical 8×8 matrices. Status: VERIFIED_BY_CODE.
- **Shift keys**: VSDL reads them from Port C bits 5-7. EMU80 places Shift/Ctrl/Lang
  as separate entries in the matrix. Both approaches produce the same scan results.
- **Port A sharing**: Port A (0x03) carries both scroll value and keyboard row mask.
  The emulator models this as a shared register.

See `verification.md` for detailed evidence and source attribution.
