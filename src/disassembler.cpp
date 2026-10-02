#include "disassembler.h"
#include "opcode_info.h"
#include <cstdio>

// ---------------------------------------------------------------------------
// Opcode classification
//
// Each of the 256 opcodes is assigned a format that tells the disassembler
// how to decode its operands.  Regular opcodes (MOV, ALU, etc.) are decoded
// algorithmically from the opcode bits; irregular ones use the table.
// ---------------------------------------------------------------------------

enum OpFormat {
    F_IMP,      // implicit (no operands)
    F_REG,      // single reg: r = bits 0-2  (INR, DCR, MOV src)
    F_RPAIR,    // reg pair: rp = bits 4-5   (INX, DCX, DAD, POP, PUSH)
    F_MVI,      // MVI r, byte
    F_ALU,      // ALU r  (ADD, ADC, SUB, SBB, ANA, XRA, ORA, CMP)
    F_MOV,      // MOV rd, rs
    F_IMM16,    // immediate 16-bit: ADI, ACI, SUI, SBI, ANI, XRI, ORI, CPI
    F_LXI,      // LXI rp, word
    F_JMP,      // JMP addr  (also Jcc)
    F_CALL,     // CALL addr (also Ccc)
    F_RET,      // RET       (also Rcc)
    F_RST,      // RST n
    F_STA,      // STA addr
    F_LDA,      // LDA addr
    F_SHLD,     // SHLD addr
    F_LHLD,     // LHLD addr
    F_IN,       // IN port
    F_OUT,      // OUT port
    F_PCHL,     // PCHL
    F_DB        // undefined opcode → DB xx
};

// ---------------------------------------------------------------------------
// Full 256-entry table
// ---------------------------------------------------------------------------

struct OpEntry { const char *mnemonic; OpFormat fmt; };

static const OpEntry optable[256] = {
    /* 00 */ { "NOP",  F_IMP  }, { "LXI",  F_LXI  }, { "STAX", F_RPAIR }, { "INX",  F_RPAIR },
    /* 04 */ { "INR",  F_REG  }, { "DCR",  F_REG  }, { "MVI",  F_MVI   }, { "RLC",  F_IMP   },
    /* 08 */ { "NOP",  F_IMP  }, { "DAD",  F_RPAIR }, { "LDAX", F_RPAIR }, { "DCX",  F_RPAIR },
    /* 0C */ { "INR",  F_REG  }, { "DCR",  F_REG  }, { "MVI",  F_MVI   }, { "RRC",  F_IMP   },

    /* 10 */ { "NOP",  F_IMP  }, { "LXI",  F_LXI  }, { "STAX", F_RPAIR }, { "INX",  F_RPAIR },
    /* 14 */ { "INR",  F_REG  }, { "DCR",  F_REG  }, { "MVI",  F_MVI   }, { "RAL",  F_IMP   },
    /* 18 */ { "NOP",  F_IMP  }, { "DAD",  F_RPAIR }, { "LDAX", F_RPAIR }, { "DCX",  F_RPAIR },
    /* 1C */ { "INR",  F_REG  }, { "DCR",  F_REG  }, { "MVI",  F_MVI   }, { "RAR",  F_IMP   },

    /* 20 */ { "NOP",  F_IMP  }, { "LXI",  F_LXI  }, { "SHLD", F_SHLD  }, { "INX",  F_RPAIR },
    /* 24 */ { "INR",  F_REG  }, { "DCR",  F_REG  }, { "MVI",  F_MVI   }, { "DAA",  F_IMP   },
    /* 28 */ { "NOP",  F_IMP  }, { "DAD",  F_RPAIR }, { "LHLD", F_LHLD  }, { "DCX",  F_RPAIR },
    /* 2C */ { "INR",  F_REG  }, { "DCR",  F_REG  }, { "MVI",  F_MVI   }, { "CMA",  F_IMP   },

    /* 30 */ { "NOP",  F_IMP  }, { "LXI",  F_LXI  }, { "STA",  F_STA   }, { "INX",  F_RPAIR },
    /* 34 */ { "INR",  F_REG  }, { "DCR",  F_REG  }, { "MVI",  F_MVI   }, { "STC",  F_IMP   },
    /* 38 */ { "NOP",  F_IMP  }, { "DAD",  F_RPAIR }, { "LDA",  F_LDA   }, { "DCX",  F_RPAIR },
    /* 3C */ { "INR",  F_REG  }, { "DCR",  F_REG  }, { "MVI",  F_MVI   }, { "CMC",  F_IMP   },

    /* 40-47 */ { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
                { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
    /* 48-4F */ { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
                { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
    /* 50-57 */ { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
                { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
    /* 58-5F */ { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
                { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
    /* 60-67 */ { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
                { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
    /* 68-6F */ { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
                { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
    /* 70-76 */ { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
                { "MOV", F_MOV }, { "MOV", F_MOV }, { "HLT", F_IMP   },
    /* 77 */    { "MOV", F_MOV },
    /* 78-7F */ { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },
                { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV }, { "MOV", F_MOV },

    /* 80-87 ADD r */ { "ADD", F_ALU }, { "ADD", F_ALU }, { "ADD", F_ALU }, { "ADD", F_ALU },
                       { "ADD", F_ALU }, { "ADD", F_ALU }, { "ADD", F_ALU }, { "ADD", F_ALU },
    /* 88-8F ADC r */ { "ADC", F_ALU }, { "ADC", F_ALU }, { "ADC", F_ALU }, { "ADC", F_ALU },
                       { "ADC", F_ALU }, { "ADC", F_ALU }, { "ADC", F_ALU }, { "ADC", F_ALU },

    /* 90-97 SUB r */ { "SUB", F_ALU }, { "SUB", F_ALU }, { "SUB", F_ALU }, { "SUB", F_ALU },
                       { "SUB", F_ALU }, { "SUB", F_ALU }, { "SUB", F_ALU }, { "SUB", F_ALU },
    /* 98-9F SBB r */ { "SBB", F_ALU }, { "SBB", F_ALU }, { "SBB", F_ALU }, { "SBB", F_ALU },
                       { "SBB", F_ALU }, { "SBB", F_ALU }, { "SBB", F_ALU }, { "SBB", F_ALU },

    /* A0-A7 ANA r */ { "ANA", F_ALU }, { "ANA", F_ALU }, { "ANA", F_ALU }, { "ANA", F_ALU },
                       { "ANA", F_ALU }, { "ANA", F_ALU }, { "ANA", F_ALU }, { "ANA", F_ALU },
    /* A8-AF XRA r */ { "XRA", F_ALU }, { "XRA", F_ALU }, { "XRA", F_ALU }, { "XRA", F_ALU },
                       { "XRA", F_ALU }, { "XRA", F_ALU }, { "XRA", F_ALU }, { "XRA", F_ALU },

    /* B0-B7 ORA r */ { "ORA", F_ALU }, { "ORA", F_ALU }, { "ORA", F_ALU }, { "ORA", F_ALU },
                       { "ORA", F_ALU }, { "ORA", F_ALU }, { "ORA", F_ALU }, { "ORA", F_ALU },
    /* B8-BF CMP r */ { "CMP", F_ALU }, { "CMP", F_ALU }, { "CMP", F_ALU }, { "CMP", F_ALU },
                       { "CMP", F_ALU }, { "CMP", F_ALU }, { "CMP", F_ALU }, { "CMP", F_ALU },

    /* C0 */ { "RNZ", F_RET  }, { "POP",  F_RPAIR }, { "JNZ", F_JMP  }, { "JMP", F_JMP  },
    /* C4 */ { "CNZ", F_CALL }, { "PUSH", F_RPAIR }, { "ADI", F_IMM16 }, { "RST", F_RST  },
    /* C8 */ { "RZ",  F_RET  }, { "RET",  F_RET   }, { "JZ",  F_JMP  }, { "DB",  F_DB   },
    /* CC */ { "CZ",  F_CALL }, { "CALL", F_CALL  }, { "ACI", F_IMM16 }, { "RST", F_RST  },

    /* D0 */ { "RNC", F_RET  }, { "POP",  F_RPAIR }, { "JNC", F_JMP  }, { "OUT", F_OUT  },
    /* D4 */ { "CNC", F_CALL }, { "PUSH", F_RPAIR }, { "SUI", F_IMM16 }, { "RST", F_RST  },
    /* D8 */ { "RC",  F_RET  }, { "DB",   F_DB    }, { "JC",  F_JMP  }, { "IN",   F_IN   },
    /* DC */ { "CC",  F_CALL }, { "DB",   F_DB    }, { "SBI", F_IMM16 }, { "RST", F_RST  },

    /* E0 */ { "RPO", F_RET  }, { "POP",  F_RPAIR }, { "JPO", F_JMP  }, { "XTHL",F_IMP  },
    /* E4 */ { "CPO", F_CALL }, { "PUSH", F_RPAIR }, { "ANI", F_IMM16 }, { "RST", F_RST  },
    /* E8 */ { "RPE", F_RET  }, { "PCHL", F_PCHL  }, { "JPE", F_JMP  }, { "XCHG",F_IMP  },
    /* EC */ { "CPE", F_CALL }, { "DB",   F_DB    }, { "XRI", F_IMM16 }, { "RST", F_RST  },

    /* F0 */ { "RP",  F_RET  }, { "POP",  F_RPAIR }, { "JP",  F_JMP  }, { "DI",  F_IMP  },
    /* F4 */ { "CP",  F_CALL }, { "PUSH", F_RPAIR }, { "ORI", F_IMM16 }, { "RST", F_RST  },
    /* F8 */ { "RM",  F_RET  }, { "SPHL", F_IMP   }, { "JM",  F_JMP  }, { "EI",  F_IMP  },
    /* FC */ { "CM",  F_CALL }, { "DB",   F_DB    }, { "CPI", F_IMM16 }, { "RST", F_RST  },
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char *reg_name(int r)
{
    static const char *names[] = {
        "B", "C", "D", "E", "H", "L", "M", "A"
    };
    return names[r & 7];
}

static const char *rp_name(int rp)
{
    static const char *names[] = { "B", "D", "H", "SP" };
    return names[rp & 3];
}

static const char *rp_push_name(int rp)
{
    static const char *names[] = { "B", "D", "H", "PSW" };
    return names[rp & 3];
}

static const char *cc_name(int cc)
{
    static const char *names[] = { "NZ", "Z", "NC", "C", "PO", "PE", "P", "M" };
    return names[cc & 7];
}

static void fmt_hex(char *buf, size_t sz, unsigned val, int digits)
{
    if (digits == 2)
        snprintf(buf, sz, "%02X", val);
    else
        snprintf(buf, sz, "%04X", val);
}

// ---------------------------------------------------------------------------
// Main disassembly function
// ---------------------------------------------------------------------------

DisassembledInstruction disassemble(uint16_t address, DisasmReadFn readByte)
{
    DisassembledInstruction di;
    di.address = address;
    di.bytes = {{0, 0, 0}};

    uint8_t op = readByte(address);
    di.opcode = op;
    di.length = opcode_info::get_length(op);
    di.bytes[0] = op;

    uint8_t b1 = 0, b2 = 0;
    if (di.length >= 2) { b1 = readByte(address + 1); di.bytes[1] = b1; }
    if (di.length >= 3) { b2 = readByte(address + 2); di.bytes[2] = b2; }

    uint16_t word = static_cast<uint16_t>(b1 | (b2 << 8));

    const OpEntry &e = optable[op];
    di.mnemonic = e.mnemonic;

    char hex2[8], hex4[16];
    char operand_buf[64];
    operand_buf[0] = '\0';

    switch (e.fmt) {
    case F_IMP:
        break;

    case F_REG:
        di.operands = reg_name((op >> 3) & 7);  // bits 5-3 for INR/DCR
        break;

    case F_RPAIR:
        // DAD/INX/DCX use bits 4-5; POP/PUSH use bits 4-5 with SP→PSW
        if (op == 0xC1 || op == 0xC5 || op == 0xD1 || op == 0xD5 ||
            op == 0xE1 || op == 0xE5 || op == 0xF1 || op == 0xF5) {
            di.operands = rp_push_name((op >> 4) & 3);
        } else {
            di.operands = rp_name((op >> 4) & 3);
        }
        break;

    case F_MVI:
        fmt_hex(hex2, sizeof(hex2), b1, 2);
        snprintf(operand_buf, sizeof(operand_buf), "%s, %s",
                 reg_name((op >> 3) & 7), hex2);  // bits 5-3
        di.operands = operand_buf;
        break;

    case F_ALU:
        di.operands = reg_name(op & 7);  // bits 2-0 = source register
        break;

    case F_MOV:
        if (op == 0x76) {
            di.mnemonic = "HLT";
            di.operands = "";
        } else {
            snprintf(operand_buf, sizeof(operand_buf), "%s, %s",
                     reg_name((op >> 3) & 7), reg_name(op & 7));
            di.operands = operand_buf;
        }
        break;

    case F_IMM16:
        fmt_hex(hex2, sizeof(hex2), b1, 2);
        snprintf(operand_buf, sizeof(operand_buf), "%s", hex2);
        di.operands = operand_buf;
        break;

    case F_LXI:
        fmt_hex(hex4, sizeof(hex4), word, 4);
        snprintf(operand_buf, sizeof(operand_buf), "%s, %s",
                 rp_name((op >> 4) & 3), hex4);
        di.operands = operand_buf;
        break;

    case F_JMP:
    case F_CALL:
        fmt_hex(hex4, sizeof(hex4), word, 4);
        di.hasTarget = true;
        di.target = word;
        if ((op & 0x07) != 0x03 && (op & 0x07) != 0x02 &&
            (op & 0x0F) != 0x0A && (op & 0x07) != 0x06 &&
            (op & 0x07) != 0x00 && (op & 0x07) != 0x04) {
            // Unconditional JMP/CALL — no condition prefix
            di.operands = hex4;
        } else {
            // Conditional Jcc / Ccc
            snprintf(operand_buf, sizeof(operand_buf), "%s", hex4);
            di.operands = operand_buf;
        }
        break;

    case F_RET:
        break;

    case F_RST:
        fmt_hex(hex2, sizeof(hex2), ((op >> 3) & 7) * 8, 2);
        di.operands = hex2;
        di.hasTarget = true;
        di.target = static_cast<uint16_t>(((op >> 3) & 7) * 8);
        break;

    case F_STA:
    case F_LDA:
        fmt_hex(hex4, sizeof(hex4), word, 4);
        di.operands = hex4;
        break;

    case F_SHLD:
    case F_LHLD:
        fmt_hex(hex4, sizeof(hex4), word, 4);
        di.operands = hex4;
        break;

    case F_IN:
        fmt_hex(hex2, sizeof(hex2), b1, 2);
        di.operands = hex2;
        break;

    case F_OUT:
        fmt_hex(hex2, sizeof(hex2), b1, 2);
        di.operands = hex2;
        break;

    case F_PCHL:
        break;

    case F_DB:
        fmt_hex(hex2, sizeof(hex2), op, 2);
        di.operands = hex2;
        break;
    }

    // Build combined text
    if (di.operands.empty()) {
        di.text = di.mnemonic;
    } else {
        di.text = di.mnemonic + " " + di.operands;
    }

    return di;
}
