#pragma once

// ---------------------------------------------------------------------------
// asm_exporter.h — Stage 6.17
//
// Z88DK ASM Exporter — converts ROM + RDB + code analysis into z80asm source.
//
// This is a read-only tool: it never modifies RDB.
// It does NOT contain its own 8080 opcode decoder — it uses the existing
// disassembler (disassemble()) and code analyzer (analyzeCode()).
//
// Pipeline:
//   ROM file + RDB file → AsmExporter → .asm / .inc / export.json
// ---------------------------------------------------------------------------

#include "code_analyzer.h"
#include "rdb_controller.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Export configuration
// ---------------------------------------------------------------------------

struct AsmExportConfig
{
    std::string romPath;
    std::string rdbPath;        // empty = no RDB
    std::string outputDir;
    uint16_t    origin = 0x0100; // ROM load origin
    size_t      maxAnalysisInstructions = 10000;
};

// ---------------------------------------------------------------------------
// ROM coverage (Stage 6.23 §2.3) — "потеря не должна выглядеть как успех"
// ---------------------------------------------------------------------------

struct AsmCoverage
{
    uint64_t romBytes         = 0;   // размер образа
    uint64_t codeBytes        = 0;   // байты, занятые инструкциями в окне ROM
    uint64_t dataBytes        = 0;   // байты, приписанные объектам RDB
    uint64_t instructionCount = 0;   // инструкций в окне ROM
    uint64_t outsideWindowDropped = 0;   // инструкций отброшено как вне ROM (§2.1)
    // Непокрытые интервалы [start, end] включительно — эмитируются как gap_%04X
    std::vector<std::pair<uint16_t, uint16_t>> uncovered;

    bool complete() const { return uncovered.empty(); }
};

// ---------------------------------------------------------------------------
// Attributed ROM data region (Stage 6.23 §2.4)
//
// One run of bytes owned by a single RDB object.  A region split by
// instruction bytes or by an earlier region becomes several units; only the
// first keeps the RDB name, the rest get address labels.
// ---------------------------------------------------------------------------

struct AsmDataUnit
{
    uint16_t    address = 0;
    uint32_t    size    = 0;   // always > 0
    std::string name;          // empty = anonymous continuation run
    std::string comment;
};

// ---------------------------------------------------------------------------
// Export report — machine-readable summary
// ---------------------------------------------------------------------------

struct ExportWarning
{
    std::string message;
    uint16_t    address = 0;
};

struct AsmExportReport
{
    // ROM identity
    std::string romFile;
    uint64_t    romSize    = 0;
    std::string romSha256;

    // Addresses
    uint16_t    origin           = 0;
    uint16_t    mappingEntryPoint = 0;

    // Counts
    int objectCount  = 0;
    int linkCount    = 0;
    int codeRanges   = 0;
    int dataRanges   = 0;

    // Generated files
    std::vector<std::string> generatedFiles;

    // Diagnostics
    std::vector<ExportWarning> warnings;
    std::vector<ExportWarning> errors;
    int conflicts            = 0;
    int unresolvedReferences = 0;

    // Stage 6.23 §2.3 — побайтовое покрытие окна ROM
    AsmCoverage coverage;

    bool hasErrors() const { return !errors.empty(); }
};

// ---------------------------------------------------------------------------
// AsmExporter
// ---------------------------------------------------------------------------

class AsmExporter
{
public:
    explicit AsmExporter(const AsmExportConfig &config);

    // Run the full export pipeline.  Returns the report.
    AsmExportReport run();

    // --- Static helpers (testable in isolation) ---

    // Convert debugger operand text to z80asm format (target: -m=8080_strict).
    // Handles the disassembler's "B, 082A" (space after comma) by trimming.
    // Hex literals always start with a digit and end with 'h'; 8080 register
    // names M/PSW stay "m"/"psw" (NOT the Z80 "(hl)"/"af", illegal in strict).
    // e.g. "H, 0100" -> "h,0100h", "A,D3" -> "a,0d3h", "E, M" -> "e,m"
    static std::string convertOperands(const std::string &mnemonic,
                                       const std::string &operands);

    // Generate a stable label for an address: "label_XXXX"
    static std::string generateLabel(uint16_t address);

    // Sanitize a name for use as z80asm identifier.
    static std::string sanitizeLabel(const std::string &name);

    // Format a byte as a z80asm hex literal.  A literal must begin with a
    // digit, so bytes >= 0xA0 get a leading '0': 0x0A -> "0Ah", 0xFF -> "0FFh".
    static std::string formatByte(uint8_t b);

private:
    AsmExportConfig config_;

    // ROM data
    std::vector<uint8_t> rom_;

    // RDB (may be empty if no RDB file)
    RdbController rdb_;
    bool hasRdb_ = false;

    // Code analysis result
    CodeAnalysisResult analysis_;

    // Name map: address → label name
    std::map<uint16_t, std::string> nameMap_;

    // Set of addresses that are code (from analysis)
    std::set<uint16_t> codeAddresses_;

    // Layout state (Stage 6.23) — built once by buildLayout(), after analysis
    // and name mapping, and consumed by every emitter and by the coverage
    // report.  owner_ is indexed by (address - origin).
    std::vector<AsmDataUnit> dataUnits_;   // attributed ROM regions
    std::vector<uint8_t>     owner_;       // 0 = gap, 1 = code, 2 = data
    uint64_t droppedOutsideWindow_ = 0;    // clipped instructions (§2.1)
    // RDB objects whose declared range collided with bytes that were already
    // owned (by code or by an earlier object) — reported as warnings
    std::vector<std::pair<uint16_t, std::string>> nameCollisions_;

    // --- Pipeline steps ---

    bool loadRom();
    bool loadRdb();
    void runAnalysis();
    void rebuildRanges();     // §2.1: ranges after clipping to the ROM window
    void buildNameMap();
    void buildLayout();       // §2.2/§2.3/§2.4: byte ownership + data units
    AsmCoverage computeCoverage() const;
    AsmExportReport emitFiles();

    // File emitters
    std::string emitMainAsm();
    std::string emitLayoutAsm();   // §2.2 — the stream the build actually uses
    std::string emitCodeAsm();
    std::string emitDataAsm();

    // Emission helpers shared by the layout and by the read-only views
    // Comment + label for an address, or "" when nothing is worth printing.
    // `defined` collects the labels actually emitted, so that names whose bytes
    // belong to another unit can be released as `equ` instead (Stage 6.23 §2.4).
    std::string emitAddressLabel(uint16_t addr,
                                 std::set<std::string> &defined) const;
    // defb rows for the absolute inclusive range [start, end]
    std::string emitByteRows(uint32_t start, uint32_t end) const;
    std::string emitExportJson(const AsmExportReport &report);

    // Instruction emitter
    std::string emitInstruction(const AnalyzedInstruction &instr);

    // Data emitter
    std::string emitDataBlock(const std::string &label,
                              const std::string &comment,
                              const uint8_t *data, size_t size);

    // Name resolution
    std::string resolveName(uint16_t address) const;

    // ROM read function for disassembler
    uint8_t readByte(uint16_t addr) const;

    // SHA-256 of ROM
    static std::string computeSha256(const std::vector<uint8_t> &data);
};
