#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <functional>

// ---------------------------------------------------------------------------
// 8080 Disassembler — backend API only (no GUI).
//
// Covers all 256 opcodes.  Undefined/undocumented opcodes are decoded
// deterministically as "DB xx" (define byte).
// ---------------------------------------------------------------------------

struct DisassembledInstruction
{
    uint16_t address;

    uint8_t opcode;
    uint8_t length;

    std::array<uint8_t, 3> bytes;

    std::string mnemonic;
    std::string operands;
    std::string text;       // "MNEMONIC OPERANDS"

    // Branch target address (for CALL, JMP, RST)
    bool     hasTarget = false;
    uint16_t target    = 0;
};

// Read function type — the disassembler calls this to fetch bytes from memory.
using DisasmReadFn = std::function<uint8_t(uint16_t addr)>;

DisassembledInstruction disassemble(uint16_t address, DisasmReadFn readByte);
