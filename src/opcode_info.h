#pragma once

#include <cstdint>

// Instruction length table for Intel 8080 (KR580VM80A).
// Based on the opcode encoding: each entry gives the total instruction
// length in bytes (opcode + operands).
//
// This table is shared between the CPU core, debugger, and disassembler
// to avoid duplicating opcode information.

namespace opcode_info {

// Length in bytes for each of the 256 possible opcodes.
// 0 means undefined/undocumented opcode (treated as 1-byte NOP).
static const uint8_t opcode_length[256] = {
    // 0x00-0x0F
    1, // 00 NOP
    3, // 01 LXI B,word
    1, // 02 STAX B
    1, // 03 INX B
    1, // 04 INR B
    1, // 05 DCR B
    2, // 06 MVI B,byte
    1, // 07 RLC
    1, // 08 NOP (undocumented)
    1, // 09 DAD B
    1, // 0A LDAX B
    1, // 0B DCX B
    1, // 0C INR C
    1, // 0D DCR C
    2, // 0E MVI C,byte
    1, // 0F RRC

    // 0x10-0x1F
    1, // 10 NOP (undocumented)
    3, // 11 LXI D,word
    1, // 12 STAX D
    1, // 13 INX D
    1, // 14 INR D
    1, // 15 DCR D
    2, // 16 MVI D,byte
    1, // 17 RAL
    1, // 18 NOP (undocumented)
    1, // 19 DAD D
    1, // 1A LDAX D
    1, // 1B DCX D
    1, // 1C INR E
    1, // 1D DCR E
    2, // 1E MVI E,byte
    1, // 1F RAR

    // 0x20-0x2F
    1, // 20 NOP (undocumented)
    3, // 21 LXI H,word
    3, // 22 SHLD addr
    1, // 23 INX H
    1, // 24 INR H
    1, // 25 DCR H
    2, // 26 MVI H,byte
    1, // 27 DAA
    1, // 28 NOP (undocumented)
    1, // 29 DAD H
    3, // 2A LHLD addr
    1, // 2B DCX H
    1, // 2C INR L
    1, // 2D DCR L
    2, // 2E MVI L,byte
    1, // 2F CMA

    // 0x30-0x3F
    1, // 30 NOP (undocumented)
    3, // 31 LXI SP,word
    3, // 32 STA addr
    1, // 33 INX SP
    1, // 34 INR M
    1, // 35 DCR M
    2, // 36 MVI M,byte
    1, // 37 STC
    1, // 38 NOP (undocumented)
    1, // 39 DAD SP
    3, // 3A LDA addr
    1, // 3B DCX SP
    1, // 3C INR A
    1, // 3D DCR A
    2, // 3E MVI A,byte
    1, // 3F CMC

    // 0x40-0x4F  (MOV B,r)
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,

    // 0x50-0x5F  (MOV C,r)
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,

    // 0x60-0x6F  (MOV D,r)
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,

    // 0x70-0x7F  (MOV M,r / HLT)
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,

    // 0x80-0x8F  (ADD/ADC r)
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,

    // 0x90-0x9F  (SUB/SBB r)
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,

    // 0xA0-0xAF  (ANA/XRA/ORA r)
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,

    // 0xB0-0xBF  (CMP etc.)
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,

    // 0xC0-0xCF
    1, // C0 RNZ
    1, // C1 POP B
    3, // C2 JNZ addr
    3, // C3 JMP addr
    3, // C4 CNZ addr
    1, // C5 PUSH B
    2, // C6 ADI byte
    1, // C7 RST 0
    1, // C8 RZ
    1, // C9 RET
    3, // CA JZ addr
    1, // CB undefined (treat as 1)
    3, // CC CZ addr
    3, // CD CALL addr
    2, // CE ACI byte
    1, // CF RST 1

    // 0xD0-0xDF
    1, // D0 RNC
    1, // D1 POP D
    3, // D2 JNC addr
    2, // D3 OUT port
    3, // D4 CNC addr
    1, // D5 PUSH D
    2, // D6 SUI byte
    1, // D7 RST 2
    1, // D8 RC
    1, // D9 undefined (treat as 1)
    3, // DA JC addr
    2, // DB IN port
    3, // DC CC addr
    1, // DD undefined (treat as 1)
    2, // DE SBI byte
    1, // DF RST 3

    // 0xE0-0xEF
    1, // E0 RPO
    1, // E1 POP H
    3, // E2 JPO addr
    1, // E3 XTHL
    3, // E4 CPO addr
    1, // E5 PUSH H
    2, // E6 ANI byte
    1, // E7 RST 4
    1, // E8 RPE
    1, // E9 PCHL
    3, // EA JPE addr
    1, // EB XCHG
    3, // EC CPE addr
    1, // ED undefined (treat as 1)
    2, // EE XRI byte
    1, // EF RST 5

    // 0xF0-0xFF
    1, // F0 RP
    1, // F1 POP PSW
    3, // F2 JP addr
    1, // F3 DI
    3, // F4 CP addr
    1, // F5 PUSH PSW
    2, // F6 ORI byte
    1, // F7 RST 6
    1, // F8 RM
    1, // F9 SPHL
    3, // FA JM addr
    1, // FB EI
    3, // FC CM addr
    1, // FD undefined (treat as 1)
    2, // FE CPI byte
    1, // FF RST 7
};

inline uint8_t get_length(uint8_t opcode) {
    uint8_t len = opcode_length[opcode];
    return len ? len : 1; // undefined opcodes treated as 1-byte NOP
}

} // namespace opcode_info
