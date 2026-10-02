---
platform: Vector-06C
topic: cpu
status: verified
source: multiple
---

# KR580VM80A (Intel 8080) CPU

## Overview

The Vector-06C uses the KR580VM80A, a Soviet clone of the Intel 8080 CPU.

**Clock frequency:** UNVERIFIED — commonly cited as 3 MHz, but emulators do not model
absolute CPU frequency. The CPU runs synchronised to the pixel clock (12 MHz / 4 = 3 MHz
in the emulator model). Actual hardware frequency may differ.

## Registers

| Register | Width | Description |
|----------|-------|-------------|
| A | 8-bit | Accumulator |
| B, C | 8-bit | General purpose (BC pair = 16-bit) |
| D, E | 8-bit | General purpose (DE pair = 16-bit) |
| H, L | 8-bit | General purpose (HL pair = 16-bit, often address pointer) |
| SP | 16-bit | Stack Pointer |
| PC | 16-bit | Program Counter |
| F | 8-bit | Flags register |

## Flag Register (F)

| Bit | Name | Description |
|-----|------|-------------|
| 7 | Sign (S) | Set if result is negative (bit 7 = 1) |
| 6 | Zero (Z) | Set if result is zero |
| 4 | Half-carry (AC/AUX) | Set if carry from bit 3 to bit 4 |
| 2 | Parity (P) | Set if result has even parity |
| 1 | Carry (CY) | Set if carry/borrow from bit 7 |
| 0,3,5 | — | Always 0 (unused) |

Flag order in F byte: SZ0A0P1C (bits 7–0, where 0 = always zero).

## Interrupt

- INTR pin: maskable interrupt
- INTE (Interrupt Enable Flag): controlled by EI/DI instructions
- On interrupt, CPU executes RST 7 (restart at 0x0038)
- IFF (inner flip-flop) tracks interrupt state; distinct from INTE pin
- After EI, interrupt is acknowledged only after the following instruction (standard 8080 behavior)

## Stack Operations

Instructions affecting SP:
- PUSH rp (BC, DE, HL, PSW) — SP -= 2, write word
- POP rp — read word, SP += 2
- CALL addr — SP -= 2, push PC, PC = addr
- RET — PC = pop(), SP += 2
- XTHL — swap HL with word at (SP)
- SPHL — SP = HL
- RST n — SP -= 2, push PC, PC = n*8
- DAD rp — does NOT affect SP

## Key Instruction Groups

| Group | Examples | Notes |
|-------|----------|-------|
| Data transfer | MOV, MVI, LDA, STA, LHLD, SHLD, LDAX, STAX | |
| Arithmetic | ADD, ADC, SUB, SBB, INR, DCR, DAD, INX, DCX | |
| Logic | ANA, XRA, ORA, CMP, RLC, RRC, RAL, RAR | |
| Branch | JMP, Jcc, CALL, Ccc, RET, Rcc, RST, PCHL | |
| Stack | PUSH, POP, XTHL, SPHL | |
| I/O | IN port, OUT port | Port is 8-bit (0–255) |
| Control | EI, DI, HLT, NOP | |

## Cycle Timing

### Standard 8080 Timing (without wait states)

Each instruction takes 4–18 T-states on a standard Intel 8080.

| Instruction | T-states | Status |
|-------------|----------|--------|
| MOV r,r | 5 | VERIFIED (standard 8080) |
| MVI r,D8 | 7 | VERIFIED |
| LDA addr | 13 | VERIFIED |
| CALL addr | 17 | VERIFIED |
| RET | 10 | VERIFIED |
| RST n | 11 | VERIFIED |
| XTHL | 18 | VERIFIED |
| JMP addr | 10 | VERIFIED |
| PUSH rp | 11 | VERIFIED |
| POP rp | 10 | VERIFIED |
| IN/OUT port | 10 | VERIFIED |

### Vector-06C Timing (with wait states)

The video controller adds wait states when CPU accesses shared memory. The exact
timing depends on the emulator model:

- **EMU80**: Uses an explicit wait-state table (`VectorCpuWaits::getCpuWaitStates()`).
  Examples: MOV r,r 5+3=8, CALL 17+4=21, RST 11+5=16.
- **VSDL**: Uses pixel-clock timing (`filler.fill(instr_time << 2)`). Does not have
  an explicit wait-state table.

**CONFLICT:** Some sources cite CALL=24, XTHL=24 for Vector-06C, but EMU80 wait table
gives CALL=21, XTHL=22. The true hardware values are unknown without cycle-exact
measurement. See `verification.md` §4.4.2, §17.4.

### Undocumented Opcodes

The existence and behavior of undocumented 8080 opcodes (08h, 10h, 18h, 20h, 28h, 30h,
38h as NOP; CBh as JMP; DDh/EDh/FDh as CALL; D9h as RET) on the specific KR580VM80A
used in Vector-06C is UNVERIFIED. Emulators generally do not model undocumented behavior.

## Hardware vs Emulator Behavior

- **CPU model**: Both VSDL (class `I8080`) and EMU80 (class `Cpu8080`) implement the
  complete 8080 instruction set. No Z80 extensions (IX, IY, EXX, DJNZ) are present.
  Status: VERIFIED_BY_CODE.
- **CPU frequency**: Emulators use pixel-clock timing, not absolute MHz. The commonly
  cited 3 MHz is UNVERIFIED for original hardware.
- **Wait states**: The two emulators use fundamentally different timing models. Neither
  may exactly match real hardware bus arbitration.
- **Undocumented opcodes**: Not modeled by either emulator. Real hardware behavior unknown.

See `verification.md` for detailed evidence and source attribution.
