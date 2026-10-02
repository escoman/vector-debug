---
platform: Vector-06C
topic: sound
status: verified
source: multiple
---

# Vector-06C Sound System

## Overview

The base Vector-06C sound system consists of:
1. KR580VI53 timer — 3 square-wave tone channels, **no hardware noise generator**
2. Tape I/O sound (input/output through PIA1 Port C)

Optional expansions:
3. AY-3-8912 programmable sound generator — 3 channels + noise + envelope (expansion)
4. Covox DAC — simple 8-bit DAC on PPI2 Port A2 (port 0x07, expansion)

## KR580VI53 (Base Sound)

### Access

| Port | Timer Address | Function |
|------|--------------|----------|
| 0x08 | 3 | Control word |
| 0x09 | 2 | Counter 2 data |
| 0x0A | 1 | Counter 1 data |
| 0x0B | 0 | Counter 0 data |

### Properties

- 3 counters, each producing a square wave (Mode 3)
- Timer clock: ~1.5 MHz (pixel clock / 8 = 1,497,600 Hz)
- **No hardware noise generator** — the VI53 is a timer, not a sound chip
- No amplitude control (on/off only)

### Frequency Formula

```
divider = 1,500,000 / desired_frequency_hz
```

Status: VERIFIED_BY_CODE (VSDL `8253.h`: 3 `CounterUnit`, no noise)

### Channel Initialization

| Channel | Port | Control Word |
|---------|------|-------------|
| 0 | 0x0B | 36h |
| 1 | 0x0A | 76h |
| 2 | 0x09 | B6h |

### Software Noise

Since the VI53 has no noise generator, noise effects are created programmatically:
- Rapidly changing VI53 channel frequency
- 1-bit biper through tape port (port 0x01, bit 0)

Status: VERIFIED_BY_PRIMARY_SOURCE (TIMSoft, "Секреты Вектора")

## AY-3-8912 (Expansion)

**Important:** AY-3-8912 is an **expansion**, not part of the base Vector-06C
configuration. It is supported by both VSDL and EMU80 emulators.

### Access

- Port 0x15: register address (0x00–0x0F)
- Port 0x14: register data

### Registers

| Reg | Bits | Function |
|-----|------|----------|
| 0x00 | 0xFF | Channel A tone period (low) |
| 0x01 | 0x0F | Channel A tone period (high) |
| 0x02 | 0xFF | Channel B tone period (low) |
| 0x03 | 0x0F | Channel B tone period (high) |
| 0x04 | 0xFF | Channel C tone period (low) |
| 0x05 | 0x0F | Channel C tone period (high) |
| 0x06 | 0x1F | Noise period |
| 0x07 | 0xFF | Mixer control: `xxNCNBNA TCTBTA` |
| 0x08 | 0x1F | Channel A amplitude (0x00–0x0F) or envelope mode (bit 4) |
| 0x09 | 0x1F | Channel B amplitude / envelope mode |
| 0x0A | 0x1F | Channel C amplitude / envelope mode |
| 0x0B | 0xFF | Envelope period (low) |
| 0x0C | 0xFF | Envelope period (high) |
| 0x0D | 0x0F | Envelope shape: CONT\|ATT\|ALT\|HOLD |

### Mixer Control (register 0x07)

| Bit | Function |
|-----|----------|
| 0 | Channel A tone enable (0 = enabled) |
| 1 | Channel B tone enable |
| 2 | Channel C tone enable |
| 3 | Channel A noise enable (0 = enabled) |
| 4 | Channel B noise enable |
| 5 | Channel C noise enable |
| 6–7 | Unused |

### Amplitude (registers 0x08–0x0A)

- Bits 0–3: fixed amplitude level (0–15)
- Bit 4: 1 = use envelope, 0 = use fixed amplitude

### Envelope Shapes (register 0x0D)

| CONT | ATT | ALT | HOLD | Behavior |
|------|-----|-----|------|----------|
| 0 | 0 | x | x | Sawtooth down, then hold at 0 |
| 0 | 1 | x | x | Sawtooth down, then hold at 0 |
| 1 | 0 | 0 | 0 | Single sawtooth up |
| 1 | 0 | 0 | 1 | Sawtooth up, hold at 15 |
| 1 | 0 | 1 | 0 | Sawtooth up, then down, repeat |
| 1 | 0 | 1 | 1 | Sawtooth up, then down, hold at 0 |
| 1 | 1 | 0 | 0 | Sawtooth down, then hold at 0 |
| 1 | 1 | 0 | 1 | Sawtooth down, hold at 0 |
| 1 | 1 | 1 | 0 | Sawtooth down, then up, repeat |
| 1 | 1 | 1 | 1 | Sawtooth down, then up, hold at 15 |

### 16-step Amplitude Table

```
Step:  0     1     2     3     4     5     6     7
Vol:  0.00  0.014 0.021 0.029 0.042 0.062 0.085 0.137

Step:  8     9    10    11    12    13    14    15
Vol:  0.169 0.265 0.353 0.450 0.570 0.687 0.848 1.000
```

### Noise Generator

17-bit LFSR with polynomial 0x24000.
Noise period register (0x06) sets clock divider.

## Covox DAC (Expansion)

Simple resistor-ladder DAC connected to PPI2 Port A2 (port 0x07).
8-bit unsigned samples, output value directly written via OUT 0x07.

**Note:** Covox is an expansion, not part of the base Vector-06C.
Confirmed in VSDL: `Covox() { return this->PA2; }` (PA2 = port 0x07).

## Sound Mixing

Output is mixed: 3 AY channels summed and scaled by 0.333, plus Covox and tape signals.

## Hardware vs Emulator Behavior

- **AY-3-8912 clock**: The AY emulator runs at 1.5 MHz. The actual AY clock on
  real hardware may differ. The `psgT` constant (110837.5) from Z88DK is UNVERIFIED.
- **TurboSound (EMU80)**: Two AY chips — expansion, not standard.
- **Stereo ABC mixing (EMU80)**: Emulator-specific AY stereo model.
- **Resampler (VSDL)**: Digital audio filtering — emulator-specific.
- **SDL audio buffer**: Audio buffering — emulator-specific, not hardware.

See `verification.md` for detailed evidence and source attribution.
