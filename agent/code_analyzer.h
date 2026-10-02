#pragma once

// ---------------------------------------------------------------------------
// code_analyzer.h — Stage 6.16
//
// Control-flow analysis of 8080 machine code.
// Uses the existing disassembler (disassemble()) — does NOT create a
// second opcode decoder.
//
// The analyzer performs BFS from an entry point, following JMP/CALL/Jcc/
// RST targets and fall-through paths.  It detects instruction-boundary
// conflicts (two paths interpreting the same byte as different instruction
// starts) and forms contiguous code ranges.
//
// The analyzer does NOT modify RDB or SymbolDatabase.
// ---------------------------------------------------------------------------

#include "disassembler.h"

#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <queue>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

struct AnalyzedInstruction {
    uint16_t    address  = 0;
    uint8_t     opcode   = 0;
    uint8_t     size     = 0;
    std::string mnemonic;
    std::string operands;
    std::string text;
    bool        hasTarget = false;
    uint16_t    target    = 0;
};

struct ControlFlowRef {
    uint16_t    from = 0;
    uint16_t    to   = 0;
    std::string type;   // "JMP", "JCC", "CALL", "CALLCC", "RST"
};

struct CodeRange {
    uint16_t start = 0;
    uint16_t end   = 0;   // inclusive
};

struct AnalysisConflict {
    uint16_t    address = 0;
    std::string description;
};

struct CodeAnalysisResult {
    uint16_t                       entryPoint = 0;
    std::vector<uint16_t>          entryPoints;    // Stage 6.19: actual entry points analyzed
    std::vector<AnalyzedInstruction> instructions;
    std::vector<ControlFlowRef>     references;
    std::vector<CodeRange>          ranges;
    std::vector<AnalysisConflict>   conflicts;
    size_t   instructionCount = 0;
    size_t   codeBytes        = 0;   // Stage 6.19: sum of instruction sizes
    bool     truncated        = false;
};

// ---------------------------------------------------------------------------
// classifyControlFlow — determine 8080 control-flow type from opcode
// ---------------------------------------------------------------------------

enum class ControlFlowType {
    Sequential,       // normal instruction — continue to next
    UnconditionalJmp, // JMP (C3)
    ConditionalJmp,   // Jcc (C2,CA,D2,DA,E2,EA,F2,FA)
    UnconditionalCall,// CALL (CD)
    ConditionalCall,  // Ccc (C4,CC,D4,DC,E4,EC,F4,FC)
    UnconditionalRet, // RET (C9)
    ConditionalRet,   // Rcc (C0,C8,D0,D8,E0,E8,F0,F8)
    Restart,          // RST n (C7,CF,D7,DF,E7,EF,F7,FF)
    Halt,             // HLT (76)
    IndirectJump,     // PCHL (E9) — target unknown statically
};

inline ControlFlowType classifyControlFlow(uint8_t opcode)
{
    switch (opcode) {
    case 0xC3: return ControlFlowType::UnconditionalJmp;
    case 0xCD: return ControlFlowType::UnconditionalCall;
    case 0xC9: return ControlFlowType::UnconditionalRet;
    case 0xE9: return ControlFlowType::IndirectJump;
    case 0x76: return ControlFlowType::Halt;
    default: break;
    }

    // Stage 6.22 §7: в 8080 условное семейство — это страница 0xC0..0xFF,
    // где условие занимает биты 5-3, а биты 2-0 выбирают породу:
    //     000 -> Rcc,  010 -> Jcc,  100 -> Ccc,  111 -> RST n
    // Безусловные формы той же страницы (C3/C9/CD) и E9/76 возвращены выше.
    // Остальные значения бит 2-0 (001/003/005/006) — не ветвление:
    // D3/E3/F3, DD/ED/FD-префиксы, недокументированные D9/F9 и т. п.
    //
    // Предыдущая проверка (opcode & 0xC7) == 0xC0 требовала нулевых бит 0-2,
    // что верно только для Rcc: ветви ConditionalJmp/ConditionalCall/Restart
    // не выполнялись никогда, условный переход попадал в Sequential и BFS шёл
    // только по fall-through — цели Jcc/Ccc/RST не разбирались вовсе.
    if ((opcode & 0xC0) != 0xC0) return ControlFlowType::Sequential;

    switch (opcode & 0x07) {
    case 0x00: return ControlFlowType::ConditionalRet;   // C0 C8 D0 D8 E0 E8 F0 F8
    case 0x02: return ControlFlowType::ConditionalJmp;   // C2 CA D2 DA E2 EA F2 FA
    case 0x04: return ControlFlowType::ConditionalCall;  // C4 CC D4 DC E4 EC F4 FC
    case 0x07: return ControlFlowType::Restart;          // C7 CF D7 DF E7 EF F7 FF
    default:   return ControlFlowType::Sequential;
    }
}

// ---------------------------------------------------------------------------
// analyzeCodeMulti — BFS control-flow analysis from multiple entry points
// Stage 6.19
//
// Performs a single BFS with all entry points seeded into the work queue.
// The visited set is shared, so overlapping code is analyzed only once.
// ---------------------------------------------------------------------------

inline CodeAnalysisResult analyzeCodeMulti(
    const std::vector<uint16_t>& entryPoints,
    DisasmReadFn readByte,
    size_t maxInstructions)
{
    CodeAnalysisResult result;

    // Deduplicate entry points preserving order
    std::set<uint16_t> seen;
    for (auto ep : entryPoints) {
        if (seen.insert(ep).second) {
            result.entryPoints.push_back(ep);
        }
    }

    if (!result.entryPoints.empty()) {
        result.entryPoint = result.entryPoints.front();
    }

    std::set<uint16_t> visited;
    std::unordered_map<uint16_t, uint8_t> instrEndMap;
    std::queue<uint16_t> workQueue;

    for (auto ep : result.entryPoints) {
        workQueue.push(ep);
    }

    while (!workQueue.empty() && result.instructions.size() < maxInstructions) {
        uint16_t addr = workQueue.front();
        workQueue.pop();

        if (visited.count(addr)) continue;

        // Conflict: this address falls inside an already-decoded instruction
        bool conflict = false;
        for (const auto &kv : instrEndMap) {
            uint16_t instrStart = kv.first;
            uint16_t instrLast  = instrStart + kv.second;
            if (addr > instrStart && addr <= instrLast) {
                result.conflicts.push_back({addr,
                    "instruction boundary conflict: address falls inside "
                    "existing instruction at " + std::to_string(instrStart)});
                conflict = true;
                break;
            }
        }
        if (conflict) continue;

        {
            DisassembledInstruction di = disassemble(addr, readByte);

            AnalyzedInstruction ai;
            ai.address   = di.address;
            ai.opcode    = di.opcode;
            ai.size      = di.length;
            ai.mnemonic  = di.mnemonic;
            ai.operands  = di.operands;
            ai.text      = di.text;
            ai.hasTarget = di.hasTarget;
            ai.target    = di.target;
            result.instructions.push_back(ai);

            visited.insert(addr);
            instrEndMap[addr] = static_cast<uint8_t>(di.length - 1);

            uint16_t nextAddr = addr + di.length;
            ControlFlowType cft = classifyControlFlow(di.opcode);

            switch (cft) {
            case ControlFlowType::UnconditionalJmp:
                result.references.push_back({addr, di.target, "JMP"});
                break;
            case ControlFlowType::ConditionalJmp:
                result.references.push_back({addr, di.target, "JCC"});
                break;
            case ControlFlowType::UnconditionalCall:
                result.references.push_back({addr, di.target, "CALL"});
                break;
            case ControlFlowType::ConditionalCall:
                result.references.push_back({addr, di.target, "CALLCC"});
                break;
            case ControlFlowType::Restart:
                result.references.push_back({addr, di.target, "RST"});
                break;
            default:
                break;
            }

            switch (cft) {
            case ControlFlowType::UnconditionalJmp:
                if (!visited.count(di.target))
                    workQueue.push(di.target);
                break;

            case ControlFlowType::ConditionalJmp:
                if (!visited.count(di.target))
                    workQueue.push(di.target);
                if (!visited.count(nextAddr))
                    workQueue.push(nextAddr);
                break;

            case ControlFlowType::UnconditionalCall:
            case ControlFlowType::ConditionalCall:
                if (!visited.count(di.target))
                    workQueue.push(di.target);
                if (!visited.count(nextAddr))
                    workQueue.push(nextAddr);
                break;

            case ControlFlowType::UnconditionalRet:
            case ControlFlowType::Halt:
            case ControlFlowType::IndirectJump:
                break;

            case ControlFlowType::ConditionalRet:
                if (!visited.count(nextAddr))
                    workQueue.push(nextAddr);
                break;

            case ControlFlowType::Restart:
                if (!visited.count(di.target))
                    workQueue.push(di.target);
                if (!visited.count(nextAddr))
                    workQueue.push(nextAddr);
                break;

            case ControlFlowType::Sequential:
                if (!visited.count(nextAddr))
                    workQueue.push(nextAddr);
                break;
            }
        }
    }

    result.truncated = !workQueue.empty() &&
                       result.instructions.size() >= maxInstructions;
    result.instructionCount = result.instructions.size();

    // Sort instructions by address and compute codeBytes
    std::sort(result.instructions.begin(), result.instructions.end(),
              [](const AnalyzedInstruction &a, const AnalyzedInstruction &b) {
                  return a.address < b.address;
              });

    result.codeBytes = 0;
    for (const auto &inst : result.instructions) {
        result.codeBytes += inst.size;
    }

    // Form contiguous code ranges (inclusive end)
    if (!result.instructions.empty()) {
        uint16_t rangeStart = result.instructions[0].address;
        uint16_t rangeEnd   = rangeStart + result.instructions[0].size - 1;

        for (size_t i = 1; i < result.instructions.size(); ++i) {
            uint16_t curStart = result.instructions[i].address;
            if (curStart <= rangeEnd + 1) {
                uint16_t curEnd = curStart + result.instructions[i].size - 1;
                if (curEnd > rangeEnd) rangeEnd = curEnd;
            } else {
                result.ranges.push_back({rangeStart, rangeEnd});
                rangeStart = curStart;
                rangeEnd   = curStart + result.instructions[i].size - 1;
            }
        }
        result.ranges.push_back({rangeStart, rangeEnd});
    }

    return result;
}

// ---------------------------------------------------------------------------
// analyzeCode — BFS control-flow analysis (single entry point)
// ---------------------------------------------------------------------------

inline CodeAnalysisResult analyzeCode(
    uint16_t startAddress,
    DisasmReadFn readByte,
    size_t maxInstructions)
{
    return analyzeCodeMulti({startAddress}, readByte, maxInstructions);
}

