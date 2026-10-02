// ---------------------------------------------------------------------------
// asm_exporter.cpp — Stage 6.17
//
// Z88DK ASM Exporter implementation.
// See asm_exporter.h for architecture overview.
// ---------------------------------------------------------------------------

#include "asm_exporter.h"
#include "sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Helpers — file I/O
// ---------------------------------------------------------------------------

static bool writeFile(const std::string &path, const std::string &content)
{
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return f.good();
}

static bool mkdirs(const std::string &path)
{
    // Simple recursive mkdir — creates parent directories as needed.
    std::string cmd = "mkdir -p '" + path + "'";
    return system(cmd.c_str()) == 0;
}

static std::string basename(const std::string &path)
{
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

// ---------------------------------------------------------------------------
// Helpers — hex formatting
// ---------------------------------------------------------------------------

static std::string toLower(const std::string &s)
{
    std::string r = s;
    for (auto &c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

static bool isAllHex(const std::string &s)
{
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// Trim surrounding whitespace.  The disassembler emits operands as "B, 082A"
// (a space after the comma); without trimming, the right-hand field is seen as
// " 082A", which is neither all-hex nor the expected width, so it slips through
// unclassified and the "h" suffix / hex interpretation is lost.
static std::string trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Format a hex-digit string as a z80asm numeric literal.  A literal MUST begin
// with a digit; "A1FFh"/"FFh" are parsed as (undefined) symbols, and a bare
// "10" is parsed as *decimal* ten, silently corrupting the byte.  So guard a
// leading hex letter with "0" and always terminate with the "h" suffix.
// Case of the digits is preserved (callers pass upper- or lower-case).
static std::string hexLiteral(const std::string &hexDigits)
{
    std::string s = hexDigits;
    if (!s.empty() && std::isalpha(static_cast<unsigned char>(s[0])))
        s = "0" + s;
    return s + "h";
}

// ---------------------------------------------------------------------------
// AsmExporter — construction
// ---------------------------------------------------------------------------

AsmExporter::AsmExporter(const AsmExportConfig &config)
    : config_(config)
{
}

// ---------------------------------------------------------------------------
// run — full export pipeline
// ---------------------------------------------------------------------------

AsmExportReport AsmExporter::run()
{
    AsmExportReport report;

    // Step 1: Load ROM
    if (!loadRom()) {
        report.errors.push_back({"ROM file not found or empty: " + config_.romPath, 0});
        return report;
    }

    report.romFile   = basename(config_.romPath);
    report.romSize   = rom_.size();
    report.romSha256 = computeSha256(rom_);
    report.origin    = config_.origin;

    // Step 2: Load RDB (optional)
    if (!config_.rdbPath.empty()) {
        if (!loadRdb()) {
            report.warnings.push_back({"RDB file not found: " + config_.rdbPath, 0});
        }
    }

    // Step 3: Run code analysis
    runAnalysis();
    report.mappingEntryPoint = analysis_.entryPoint;
    report.codeRanges        = static_cast<int>(analysis_.ranges.size());
    report.conflicts         = static_cast<int>(analysis_.conflicts.size());

    // Step 4: Build name map
    buildNameMap();

    // Step 4.5: attribute every ROM byte to code / RDB data / gap (Stage 6.23)
    buildLayout();

    // Step 5: Count RDB stats
    if (hasRdb_) {
        report.objectCount = static_cast<int>(rdb_.objectCount());
        // Count links
        auto objects = rdb_.listObjects();
        for (const auto &obj : objects) {
            report.linkCount += static_cast<int>(obj.links.size());
        }
    }

    // Step 6: Emit files
    report = emitFiles();

    // Restore computed fields (emitFiles overwrites some)
    report.romFile   = basename(config_.romPath);
    report.romSize   = rom_.size();
    report.romSha256 = computeSha256(rom_);
    report.origin    = config_.origin;
    report.mappingEntryPoint = analysis_.entryPoint;
    report.codeRanges        = static_cast<int>(analysis_.ranges.size());
    report.conflicts         = static_cast<int>(analysis_.conflicts.size());

    return report;
}

// ---------------------------------------------------------------------------
// loadRom
// ---------------------------------------------------------------------------

bool AsmExporter::loadRom()
{
    std::ifstream f(config_.romPath, std::ios::binary);
    if (!f.is_open()) return false;

    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size <= 0 || size > 65536) return false;

    f.seekg(0, std::ios::beg);
    rom_.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char *>(rom_.data()), size);
    return f.good() || f.eof();
}

// ---------------------------------------------------------------------------
// loadRdb
// ---------------------------------------------------------------------------

bool AsmExporter::loadRdb()
{
    if (!rdb_.load(config_.rdbPath)) return false;
    hasRdb_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// runAnalysis
// ---------------------------------------------------------------------------

void AsmExporter::runAnalysis()
{
    auto readFn = [this](uint16_t addr) -> uint8_t {
        return readByte(addr);
    };

    // Entry points: the ROM mapping vector at 0x0000 plus every function
    // recorded in the RDB that lies inside the ROM window.
    //
    // Stage 6.24 §9/§10/§11: RDB objects for RAM-resident routines (addresses
    // outside [origin, origin+romSize)) are kept in the RDB but must NOT be
    // used as analysis seeds for the ROM export — they would cause the
    // analyzer to decode RAM data as instructions and pollute the reachability
    // graph with thousands of phantom code paths.  The 0x0000 reset vector is
    // always seeded because on real hardware the entry point is reached through
    // it regardless of where the ROM image is mapped.
    std::vector<uint16_t> entries;
    entries.push_back(0x0000);
    if (hasRdb_) {
        const uint32_t seedWinStart = config_.origin;
        const uint32_t seedWinEnd   =
            seedWinStart + static_cast<uint32_t>(rom_.size());
        for (const auto &obj : rdb_.listObjects()) {
            if (obj.type == RdbObjectType::Function ||
                obj.type == RdbObjectType::Label ||
                obj.type == RdbObjectType::Code) {
                // Filter: only add seeds that fall inside the ROM window.
                uint32_t addr = obj.address;
                if (addr >= seedWinStart && addr < seedWinEnd)
                    entries.push_back(obj.address);
            }
        }
    }
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());

    analysis_ = ::analyzeCodeMulti(entries, readFn, config_.maxAnalysisInstructions);

    // Stage 6.23 §2.1 — clip to the ROM window.
    // Reaching 0x0000..origin-1 on purpose is correct for the ANALYSER: on real
    // hardware execution falls through the reset NOP-sled into the entry point,
    // and dropping that seed would break the reachability graph.  It is wrong
    // for the EMITTER: those bytes are not part of the image, so emitting them
    // shifts everything after them by their length and the build no longer
    // reproduces the ROM (256 phantom NOPs here = 91 % differing bytes).
    {
        const uint32_t winStart = config_.origin;
        const uint32_t winEnd   = winStart + static_cast<uint32_t>(rom_.size());
        std::vector<AnalyzedInstruction> kept;
        kept.reserve(analysis_.instructions.size());
        droppedOutsideWindow_ = 0;
        for (const auto &instr : analysis_.instructions) {
            const uint32_t s = instr.address;
            const uint32_t e = s + instr.size;          // exclusive
            if (s >= winStart && e <= winEnd) {
                kept.push_back(instr);
            } else {
                ++droppedOutsideWindow_;
            }
        }
        analysis_.instructions.swap(kept);
        analysis_.instructionCount = analysis_.instructions.size();
        analysis_.codeBytes = 0;
        for (const auto &instr : analysis_.instructions) {
            analysis_.codeBytes += instr.size;
        }
        rebuildRanges();
    }

    // Build code address set
    for (const auto &instr : analysis_.instructions) {
        for (uint8_t i = 0; i < instr.size; ++i) {
            codeAddresses_.insert(instr.address + i);
        }
    }
}

// ---------------------------------------------------------------------------
// rebuildRanges — contiguous code ranges (inclusive end) from the clipped
// instruction list.  analyzeCodeMulti() built ranges over the unclipped set,
// so after §2.1 clipping they must be recomputed or they still name page 0.
// ---------------------------------------------------------------------------

void AsmExporter::rebuildRanges()
{
    analysis_.ranges.clear();
    if (analysis_.instructions.empty()) return;

    std::vector<AnalyzedInstruction> sorted = analysis_.instructions;
    std::sort(sorted.begin(), sorted.end(),
              [](const AnalyzedInstruction &a, const AnalyzedInstruction &b) {
                  return a.address < b.address;
              });

    uint32_t s = sorted.front().address;
    uint32_t e = s + sorted.front().size - 1;
    for (size_t i = 1; i < sorted.size(); ++i) {
        const uint32_t cs = sorted[i].address;
        const uint32_t ce = cs + sorted[i].size - 1;
        if (cs <= e + 1) {
            if (ce > e) e = ce;
        } else {
            analysis_.ranges.push_back({static_cast<uint16_t>(s),
                                        static_cast<uint16_t>(e)});
            s = cs;
            e = ce;
        }
    }
    analysis_.ranges.push_back({static_cast<uint16_t>(s),
                                static_cast<uint16_t>(e)});
}

// ---------------------------------------------------------------------------
// buildNameMap
// ---------------------------------------------------------------------------

void AsmExporter::buildNameMap()
{
    nameMap_.clear();

    // Priority 1: RDB object names
    if (hasRdb_) {
        auto objects = rdb_.listObjects();
        for (const auto &obj : objects) {
            if (!obj.name.empty()) {
                nameMap_[obj.address] = sanitizeLabel(obj.name);
            }
        }
    }

    // Priority 2: Generated labels for code analysis targets
    // (no SymbolDatabase in standalone exporter — RDB is the primary source)
    for (const auto &ref : analysis_.references) {
        if (nameMap_.find(ref.to) == nameMap_.end()) {
            nameMap_[ref.to] = generateLabel(ref.to);
        }
    }
}

// ---------------------------------------------------------------------------
// emitFiles — write all output files
// ---------------------------------------------------------------------------

AsmExportReport AsmExporter::emitFiles()
{
    AsmExportReport report;

    // Ensure output directories exist
    mkdirs(config_.outputDir);
    mkdirs(config_.outputDir + "/code");
    mkdirs(config_.outputDir + "/data");

    // Emit main.asm
    {
        std::string path = config_.outputDir + "/main.asm";
        if (writeFile(path, emitMainAsm())) {
            report.generatedFiles.push_back("main.asm");
        } else {
            report.errors.push_back({"Failed to write main.asm", 0});
        }
    }

    // Emit layout.asm — the address-ordered stream that the build consumes
    // (Stage 6.23 §2.2)
    {
        std::string path = config_.outputDir + "/layout.asm";
        if (writeFile(path, emitLayoutAsm())) {
            report.generatedFiles.push_back("layout.asm");
        } else {
            report.errors.push_back({"Failed to write layout.asm", 0});
        }
    }

    // Emit code.asm
    {
        std::string path = config_.outputDir + "/code/code.asm";
        if (writeFile(path, emitCodeAsm())) {
            report.generatedFiles.push_back("code/code.asm");
        } else {
            report.errors.push_back({"Failed to write code/code.asm", 0});
        }
    }

    // Emit data.asm
    {
        std::string path = config_.outputDir + "/data/data.asm";
        std::string content = emitDataAsm();
        if (writeFile(path, content)) {
            report.generatedFiles.push_back("data/data.asm");
            // Data ranges = ROM regions attributed to an RDB object after
            // byte-level ownership (Stage 6.23 §2.4); gaps are not data.
            report.dataRanges = static_cast<int>(dataUnits_.size());
        } else {
            report.errors.push_back({"Failed to write data/data.asm", 0});
        }
    }

    // Count unresolved references
    for (const auto &ref : analysis_.references) {
        if (nameMap_.find(ref.to) == nameMap_.end()) {
            report.unresolvedReferences++;
        }
    }

    // Add conflict warnings
    for (const auto &conflict : analysis_.conflicts) {
        report.warnings.push_back({conflict.description, conflict.address});
    }

    // Stage 6.23 §2.4 — RDB ranges that overlap each other or code
    for (const auto &nc : nameCollisions_) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "RDB object '%s' at 0x%04X overlaps bytes already owned by "
                 "another unit; name released as `equ`, not emitted as a label",
                 nc.second.c_str(), nc.first);
        report.warnings.push_back({std::string(buf), nc.first});
    }

    // Stage 6.23 §2.3 — coverage.  Before this, export.json reported
    // "warnings: 0" while 170 ROM bytes were silently missing: a lost byte was
    // indistinguishable from a successful export.
    report.coverage = computeCoverage();
    for (const auto &gap : report.coverage.uncovered) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "uncovered %u bytes at 0x%04X-0x%04X — emitted as gap_%04X, "
                 "not attributed to code or RDB",
                 static_cast<unsigned>(gap.second - gap.first + 1),
                 gap.first, gap.second, gap.first);
        report.warnings.push_back({std::string(buf), gap.first});
    }

    // Emit export.json
    report.romFile   = basename(config_.romPath);
    report.romSize   = rom_.size();
    report.romSha256 = computeSha256(rom_);
    report.origin    = config_.origin;
    report.mappingEntryPoint = analysis_.entryPoint;
    report.codeRanges = static_cast<int>(analysis_.ranges.size());
    report.conflicts  = static_cast<int>(analysis_.conflicts.size());

    if (hasRdb_) {
        report.objectCount = static_cast<int>(rdb_.objectCount());
        auto objects = rdb_.listObjects();
        report.linkCount = 0;
        for (const auto &obj : objects) {
            report.linkCount += static_cast<int>(obj.links.size());
        }
    }

    {
        std::string path = config_.outputDir + "/export.json";
        if (writeFile(path, emitExportJson(report))) {
            report.generatedFiles.push_back("export.json");
        } else {
            report.errors.push_back({"Failed to write export.json", 0});
        }
    }

    return report;
}

// ---------------------------------------------------------------------------
// emitMainAsm
// ---------------------------------------------------------------------------

std::string AsmExporter::emitMainAsm()
{
    std::ostringstream os;
    os << "; Generated by v06c-asm-export (Stage 6.17)\n";
    os << "; ROM: " << basename(config_.romPath) << "\n";
    os << "; Size: " << rom_.size() << " bytes\n";
    os << "; SHA-256: " << computeSha256(rom_) << "\n";
    os << "\n";

    char buf[16];
    snprintf(buf, sizeof(buf), "%04X", config_.origin);
    os << "\torg\t" << hexLiteral(buf) << "\n";
    os << "\n";

    // Stage 6.23 §2.2 — the build consumes ONE address-ordered stream.
    // Including code.asm and data.asm back-to-back laid all data after all
    // code, which does not reproduce the image (ROM interleaves them), and a
    // second .org to fix the layout is impossible: z80asm allows `org` only
    // once per module ("ORG redefined").
    os << "\tinclude\t\"layout.asm\"\n";
    os << "\n";
    os << "; Read-only views, not part of the build:\n";
    os << ";   include \"code/code.asm\"\n";
    os << ";   include \"data/data.asm\"\n";

    return os.str();
}

// ---------------------------------------------------------------------------
// emitCodeAsm
// ---------------------------------------------------------------------------

std::string AsmExporter::emitCodeAsm()
{
    std::ostringstream os;
    os << "; Code section — generated by v06c-asm-export\n";
    os << "; View only: the build consumes layout.asm (Stage 6.23 §2.2)\n";
    os << "; Control-flow analysis, clipped to the ROM window (§2.1)\n";
    os << "\n";

    // analysis_.instructions is already address-sorted (analyzeCodeMulti sorts,
    // clipping preserves the order).  The previous loop walked the RANGE list
    // with a monotonic index, which silently dropped instructions whenever the
    // ranges were unsorted or overlapping.
    std::set<std::string> defined;
    size_t ri = 0;
    for (const auto &instr : analysis_.instructions) {
        while (ri < analysis_.ranges.size() &&
               static_cast<uint32_t>(instr.address) > analysis_.ranges[ri].end) {
            ++ri;
        }
        if (ri < analysis_.ranges.size() &&
            static_cast<uint32_t>(instr.address) == analysis_.ranges[ri].start) {
            char rangeBuf[16];
            snprintf(rangeBuf, sizeof(rangeBuf), "%04X",
                     analysis_.ranges[ri].start);
            os << "\n; --- Code range: " << rangeBuf << " ---\n";
        }
        os << emitAddressLabel(instr.address, defined);
        os << emitInstruction(instr);
    }

    return os.str();
}

// ---------------------------------------------------------------------------
// emitDataAsm
// ---------------------------------------------------------------------------

std::string AsmExporter::emitDataAsm()
{
    std::ostringstream os;
    os << "; Data section — generated by v06c-asm-export\n";
    os << "; View only: the build consumes layout.asm (Stage 6.23 §2.2)\n";
    os << "; Regions here are the same byte runs layout.asm emits\n";
    os << "\n";

    if (!hasRdb_) {
        os << "; No RDB available — data section is empty\n";
        os << "; (RDB not found)\n";
        return os.str();
    }

    for (const auto &u : dataUnits_) {
        if (!u.comment.empty()) os << "; " << u.comment << "\n";
        os << u.name << ":\n";
        os << emitByteRows(u.address, u.address + u.size - 1);
        os << "\n";
    }

    for (const auto &gap : computeCoverage().uncovered) {
        char buf[24];
        snprintf(buf, sizeof(buf), "gap_%04X", gap.first);
        os << "; Uncovered ROM bytes — neither code nor an RDB object\n";
        os << buf << ":\n";
        os << emitByteRows(gap.first, gap.second);
        os << "\n";
    }

    return os.str();
}

// ---------------------------------------------------------------------------
// buildLayout — byte ownership of the ROM window (Stage 6.23 §2.2-§2.4)
//
// owner_[addr - origin]: 0 = nobody (gap), 1 = instruction byte, 2 = RDB data.
// Code wins over data: an RDB object may legitimately name a location inside
// an instruction (TESTAY: var_main_loop_target @0x0101 is the operand of the
// self-modified entry JMP, var_delay_loop_seed @0x04A2 is inside func_main_loop).
// Such names are released as `equ` at the end of layout.asm (see emitLayoutAsm).
// ---------------------------------------------------------------------------

void AsmExporter::buildLayout()
{
    dataUnits_.clear();
    nameCollisions_.clear();
    owner_.assign(rom_.size(), 0);
    if (rom_.empty()) return;

    const uint32_t origin = config_.origin;

    for (const auto &instr : analysis_.instructions) {          // already clipped
        for (uint8_t i = 0; i < instr.size; ++i) {
            const uint32_t off = static_cast<uint32_t>(instr.address) + i - origin;
            if (off < owner_.size()) owner_[off] = 1;
        }
    }

    if (!hasRdb_) return;

    // Function/Code/Label are code-ish: their bytes come from the instruction
    // stream, so they must not also be declared as data (double emission was
    // the past behaviour and it corrupted the image).  Everything else —
    // Data/Table/String AND Variable (Stage 6.23 §2.4: Variable used to be
    // filtered out, so 24 named TESTAY variables surfaced as unknown_%04X
    // gaps) — owns its bytes.
    std::vector<RdbObject> objects = rdb_.listObjects();
    std::sort(objects.begin(), objects.end(),
              [](const RdbObject &a, const RdbObject &b) {
                  return a.address < b.address;
              });

    for (const auto &obj : objects) {
        if (obj.type == RdbObjectType::Function ||
            obj.type == RdbObjectType::Code ||
            obj.type == RdbObjectType::Label) {
            continue;
        }

        const uint32_t size = obj.size ? obj.size : 1;   // 0 = one byte (Unknown)
        uint32_t s = obj.address;
        if (s < origin || s - origin >= owner_.size()) continue;   // outside ROM
        uint32_t end = s + size;                                   // exclusive
        if (end - origin > owner_.size()) {
            end = origin + static_cast<uint32_t>(owner_.size());
        }

        std::string baseName = obj.name.empty()
                             ? generateLabel(obj.address)
                             : sanitizeLabel(obj.name);

        // A collision is not an error in the RDB (an alias — one byte serving
        // both a channel state and AY register 0 — is a legitimate finding),
        // but it must be visible: the loser of the collision cannot become a
        // label, so its name only survives as an `equ`.
        if (!obj.name.empty()) {
            for (uint32_t b = s; b < end; ++b) {
                if (owner_[b - origin] == 0) continue;
                nameCollisions_.push_back({static_cast<uint16_t>(s), baseName});
                break;
            }
        }

        bool first = true;
        uint32_t a = s;
        while (a < end) {
            if (owner_[a - origin] != 0) { ++a; continue; }   // owned: skip, splits
            const uint32_t runStart = a;
            while (a < end) {
                const uint32_t off = a - origin;
                if (owner_[off] != 0) break;
                owner_[off] = 2;
                ++a;
            }
            AsmDataUnit unit;
            unit.address = static_cast<uint16_t>(runStart);
            unit.size    = a - runStart;
            // A name may only label the address that actually carries the
            // object.  Attaching it to a later run (when the leading bytes were
            // already owned) moves the symbol, and every operand that
            // resolveName() substitutes then assembles to the wrong address —
            // measured on TESTAY: `shld 048Fh` became `shld 0490h`, one byte of
            // the image silently corrupted.  The name is released as `equ` at
            // its true address instead (see emitLayoutAsm).
            unit.name    = (first && runStart == obj.address)
                           ? baseName
                           : generateLabel(static_cast<uint16_t>(runStart));
            unit.comment = obj.comment;
            dataUnits_.push_back(unit);
            first = false;
        }
    }
}

// ---------------------------------------------------------------------------
// computeCoverage — the numbers behind "did the export lose anything?"
// ---------------------------------------------------------------------------

AsmCoverage AsmExporter::computeCoverage() const
{
    AsmCoverage cov;
    cov.romBytes             = rom_.size();
    cov.instructionCount     = analysis_.instructionCount;
    cov.outsideWindowDropped = droppedOutsideWindow_;

    const uint32_t origin = config_.origin;
    size_t i = 0;
    while (i < owner_.size()) {
        if (owner_[i] == 1) { ++cov.codeBytes; ++i; continue; }
        if (owner_[i] == 2) { ++cov.dataBytes; ++i; continue; }
        size_t j = i;
        while (j < owner_.size() && owner_[j] == 0) ++j;
        cov.uncovered.push_back({static_cast<uint16_t>(origin + i),
                                 static_cast<uint16_t>(origin + j - 1)});
        i = j;
    }
    return cov;
}

// ---------------------------------------------------------------------------
// emitAddressLabel — comment + label in front of an emitted unit
// ---------------------------------------------------------------------------

std::string AsmExporter::emitAddressLabel(uint16_t addr,
                                          std::set<std::string> &defined) const
{
    const std::string name = resolveName(addr);
    if (name.empty()) return "";

    bool worthPrinting = (name != generateLabel(addr));       // named by the RDB
    if (!worthPrinting) {
        for (const auto &ref : analysis_.references) {
            if (ref.to == addr) { worthPrinting = true; break; }
        }
    }
    if (!worthPrinting && hasRdb_) {
        for (const auto &obj : rdb_.listObjects()) {
            if (obj.address == addr) { worthPrinting = true; break; }
        }
    }
    if (!worthPrinting) return "";

    std::ostringstream os;
    if (hasRdb_) {
        const std::string comment = rdb_.getComment(addr);
        if (!comment.empty()) os << "; " << comment << "\n";
    }
    os << name << ":\n";
    defined.insert(name);
    return os.str();
}

// ---------------------------------------------------------------------------
// emitByteRows — defb rows for an absolute inclusive address range
// ---------------------------------------------------------------------------

std::string AsmExporter::emitByteRows(uint32_t start, uint32_t end) const
{
    std::ostringstream os;
    const uint32_t origin = config_.origin;
    const uint32_t bytesPerLine = 8;

    for (uint32_t a = start; a <= end; a += bytesPerLine) {
        const uint32_t last = std::min(end + 1, a + bytesPerLine);
        os << "\tdefb\t";
        for (uint32_t b = a; b < last; ++b) {
            if (b > a) os << ",";
            const uint32_t off = b - origin;
            os << formatByte(off < rom_.size() ? rom_[off] : 0x00);
        }
        os << "\n";
    }
    return os.str();
}

// ---------------------------------------------------------------------------
// emitLayoutAsm — Stage 6.23 §2.2: one address-ordered stream
//
// Units (instruction / data region / gap) are sorted by address and emitted
// back to back, so PC advances exactly like the ROM image: a gap can never
// silently shift what follows it.  code.asm and data.asm stay as views.
// ---------------------------------------------------------------------------

std::string AsmExporter::emitLayoutAsm()
{
    const uint32_t origin = config_.origin;
    const uint32_t winEnd = origin + static_cast<uint32_t>(rom_.size());

    struct Item {
        uint32_t    start;
        uint32_t    end;      // inclusive
        int         kind;     // 0 = code, 1 = data, 2 = gap
        size_t      instr;    // kind 0: index into analysis_.instructions
        std::string label;
        std::string comment;
    };

    std::vector<Item> items;
    items.reserve(analysis_.instructions.size() + dataUnits_.size() + 8);

    for (size_t i = 0; i < analysis_.instructions.size(); ++i) {
        const auto &in = analysis_.instructions[i];
        items.push_back({in.address,
                         static_cast<uint32_t>(in.address) + in.size - 1,
                         0, i, "", ""});
    }
    for (const auto &u : dataUnits_) {
        items.push_back({u.address,
                         static_cast<uint32_t>(u.address) + u.size - 1,
                         1, 0, u.name, u.comment});
    }
    for (const auto &gap : computeCoverage().uncovered) {
        char buf[24];
        snprintf(buf, sizeof(buf), "gap_%04X", gap.first);
        items.push_back({gap.first, gap.second, 2, 0, buf,
                         "Uncovered ROM bytes — neither code nor an RDB object"});
    }
    std::sort(items.begin(), items.end(),
              [](const Item &a, const Item &b) { return a.start < b.start; });

    std::ostringstream os;
    os << "; Address-ordered layout — generated by v06c-asm-export (Stage 6.23)\n";
    os << "; Every byte of the ROM window is emitted exactly once: instruction,\n";
    os << "; RDB data region, or explicit gap_ region.  This is what the build\n";
    os << "; consumes; code/code.asm and data/data.asm are read-only views.\n\n";

    std::set<std::string> defined;
    uint32_t pc = origin;
    for (const auto &it : items) {
        if (it.end < pc) continue;                       // already covered
        if (it.start > pc) {
            // Defensive padding: with gaps emitted this must not happen, and a
            // silent shift is precisely the bug this stage removes.
            os << "\tdefs\t" << (it.start - pc) << "\n";
        }
        pc = it.end + 1;

        if (it.kind == 0) {
            const auto &instr = analysis_.instructions[it.instr];
            os << emitAddressLabel(instr.address, defined);
            os << emitInstruction(instr);
            continue;
        }

        if (!it.comment.empty()) os << "; " << it.comment << "\n";
        if (!it.label.empty()) {
            os << it.label << ":\n";
            defined.insert(it.label);
        }
        os << emitByteRows(it.start, it.end);
        os << "\n";
    }
    if (pc < winEnd) os << "\tdefs\t" << (winEnd - pc) << "\n";

    // Names that could not become labels because their bytes belong to another
    // unit (variable inside an instruction, object outside the window).  Every
    // name used as an operand must be defined somewhere, or z80asm fails with
    // "undefined symbol" — this block is what makes name substitution safe.
    std::ostringstream eq;
    bool header = false;
    for (const auto &kv : nameMap_) {
        if (defined.count(kv.second)) continue;
        if (!header) {
            eq << "\n; --- Symbols at addresses owned by another unit ---\n";
            header = true;
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "%04X", kv.first);
        eq << kv.second << "\tequ\t" << hexLiteral(buf) << "\n";
        defined.insert(kv.second);
    }
    os << eq.str();

    return os.str();
}

// ---------------------------------------------------------------------------
// emitExportJson
// ---------------------------------------------------------------------------

std::string AsmExporter::emitExportJson(const AsmExportReport &report)
{
    std::ostringstream os;
    os << "{\n";
    os << "  \"rom\": \"" << report.romFile << "\",\n";
    os << "  \"rom_size\": " << report.romSize << ",\n";

    char originBuf[8], entryBuf[8];
    snprintf(originBuf, sizeof(originBuf), "0x%04X", report.origin);
    snprintf(entryBuf, sizeof(entryBuf), "0x%04X", report.mappingEntryPoint);
    os << "  \"origin\": \"" << originBuf << "\",\n";
    os << "  \"mapping_entry_point\": \"" << entryBuf << "\",\n";
    os << "  \"rom_sha256\": \"" << report.romSha256 << "\",\n";
    os << "  \"objects\": " << report.objectCount << ",\n";
    os << "  \"links\": " << report.linkCount << ",\n";
    os << "  \"code_ranges\": " << report.codeRanges << ",\n";
    os << "  \"data_ranges\": " << report.dataRanges << ",\n";
    os << "  \"warnings\": " << report.warnings.size() << ",\n";
    os << "  \"errors\": " << report.errors.size() << ",\n";
    os << "  \"conflicts\": " << report.conflicts << ",\n";
    os << "  \"unresolved_references\": " << report.unresolvedReferences << ",\n";

    // Stage 6.23 §2.3
    const AsmCoverage &cov = report.coverage;
    os << "  \"coverage\": {\n";
    os << "    \"rom_bytes\": " << cov.romBytes << ",\n";
    os << "    \"code_bytes\": " << cov.codeBytes << ",\n";
    os << "    \"data_bytes\": " << cov.dataBytes << ",\n";
    os << "    \"instruction_count\": " << cov.instructionCount << ",\n";
    os << "    \"outside_window_dropped\": " << cov.outsideWindowDropped << ",\n";
    os << "    \"uncovered\": [";
    for (size_t i = 0; i < cov.uncovered.size(); ++i) {
        if (i) os << ", ";
        os << "{\"start\": \"0x" << std::hex << std::uppercase
           << std::setw(4) << std::setfill('0') << cov.uncovered[i].first
           << std::dec << "\", \"end\": \"0x" << std::hex << std::uppercase
           << std::setw(4) << std::setfill('0') << cov.uncovered[i].second
           << std::dec << "\"}";
    }
    os << "],\n";
    os << "    \"complete\": " << (cov.complete() ? "true" : "false") << "\n";
    os << "  }\n";
    os << "}\n";

    return os.str();
}

// ---------------------------------------------------------------------------
// emitInstruction — convert one analyzed instruction to z80asm text
// ---------------------------------------------------------------------------

std::string AsmExporter::emitInstruction(const AnalyzedInstruction &instr)
{
    std::ostringstream os;
    os << "\t" << toLower(instr.mnemonic);

    if (instr.hasTarget) {
        // JMP, CALL, Jcc, Ccc, RST — replace target with symbol name
        std::string name = resolveName(instr.target);
        if (!name.empty()) {
            os << "\t" << name;
        } else {
            // Fallback: numeric address with h suffix (leading digit guard so
            // targets >= 0xA000, e.g. 0xA1FF -> "0A1FFh", aren't read as symbols)
            char buf[8];
            snprintf(buf, sizeof(buf), "%04X", instr.target);
            os << "\t" << hexLiteral(buf);
        }
    } else if (!instr.operands.empty()) {
        std::string text = convertOperands(instr.mnemonic, instr.operands);

        // Stage 6.23 §2.4: LDA/STA/LHLD/SHLD address an absolute location, so a
        // named RDB object at that address becomes a symbol reference.  Other
        // 16-bit operands (LXI, imm16) are *values* and must stay numeric — that
        // distinction is why the substitution is limited to these four opcodes.
        const std::string &mn = instr.mnemonic;
        if (mn == "LDA" || mn == "STA" || mn == "LHLD" || mn == "SHLD") {
            const std::string tok = trim(instr.operands);
            if (isAllHex(tok) && tok.size() == 4) {
                unsigned v = 0;
                if (sscanf(tok.c_str(), "%x", &v) == 1) {
                    const std::string name = resolveName(static_cast<uint16_t>(v));
                    if (!name.empty()) text = name;
                }
            }
        }

        os << "\t" << text;
    }

    os << "\n";
    return os.str();
}

// ---------------------------------------------------------------------------
// emitDataBlock — format bytes as defb lines
// ---------------------------------------------------------------------------

std::string AsmExporter::emitDataBlock(const std::string &label,
                                        const std::string &comment,
                                        const uint8_t *data, size_t size)
{
    std::ostringstream os;

    if (!comment.empty()) {
        os << "; " << comment << "\n";
    }
    os << label << ":\n";

    // Emit 8 bytes per line for readability
    const size_t bytesPerLine = 8;
    for (size_t i = 0; i < size; i += bytesPerLine) {
        os << "\tdefb\t";
        size_t count = std::min(bytesPerLine, size - i);
        for (size_t j = 0; j < count; ++j) {
            if (j > 0) os << ",";
            os << formatByte(data[i + j]);
        }
        os << "\n";
    }

    return os.str();
}

// ---------------------------------------------------------------------------
// convertOperands — static helper for z80asm operand conversion
// ---------------------------------------------------------------------------

std::string AsmExporter::convertOperands(const std::string &mnemonic,
                                          const std::string &operands)
{
    (void)mnemonic;  // kept for API stability / future mnemonic-specific rules

    // Register mapping: 8080 names -> z80asm lowercase.
    // NOTE: M and PSW map to the 8080 names "m"/"psw", NOT the Z80 "(hl)"/"af".
    // The latter are rejected by `z80asm -m=8080_strict` (and "(hl)" fails even
    // in default mode for the two-register MOV), so emitting them made the export
    // unbuildable.  Verified byte-identical: sub m=96, mov a,m=7e, pop psw=f1.
    static const std::map<std::string, std::string> regMap = {
        {"A", "a"}, {"B", "b"}, {"C", "c"}, {"D", "d"},
        {"E", "e"}, {"H", "h"}, {"L", "l"}, {"M", "m"},
        {"PSW", "psw"}, {"SP", "sp"},
    };

    // Classify one already-trimmed operand token: register, hex literal, or
    // pass-through.  Used for both the single and the two-operand forms so the
    // register-vs-number decision lives in exactly one place.
    auto classify = [&](const std::string &tok) -> std::string {
        auto it = regMap.find(tok);
        if (it != regMap.end()) return it->second;
        if (isAllHex(tok) && (tok.size() == 2 || tok.size() == 4))
            return hexLiteral(toLower(tok));
        return tok;
    };

    auto commaPos = operands.find(',');
    if (commaPos == std::string::npos) {
        return classify(trim(operands));
    }

    std::string left  = trim(operands.substr(0, commaPos));
    std::string right = trim(operands.substr(commaPos + 1));
    return classify(left) + "," + classify(right);
}

// ---------------------------------------------------------------------------
// generateLabel — stable label for an address
// ---------------------------------------------------------------------------

std::string AsmExporter::generateLabel(uint16_t address)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "label_%04X", address);
    return buf;
}

// ---------------------------------------------------------------------------
// sanitizeLabel — make a name safe for z80asm
// ---------------------------------------------------------------------------

std::string AsmExporter::sanitizeLabel(const std::string &name)
{
    std::string result;
    result.reserve(name.size());

    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result += c;
        } else {
            result += '_';
        }
    }

    // Ensure it doesn't start with a digit
    if (!result.empty() && std::isdigit(static_cast<unsigned char>(result[0]))) {
        result = "_" + result;
    }

    return result;
}

// ---------------------------------------------------------------------------
// formatByte — format a byte as z80asm hex
// ---------------------------------------------------------------------------

std::string AsmExporter::formatByte(uint8_t b)
{
    // z80asm hex literal: always starts with a digit and ends with 'h'.
    // "%02Xh" alone yields "FFh"/"A1h" for bytes >= 0xA0, which z80asm reads as
    // undefined symbols.  hexLiteral() guards the leading letter with '0'.
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X", b);
    return hexLiteral(buf);
}

// ---------------------------------------------------------------------------
// resolveName — look up name for address
// ---------------------------------------------------------------------------

std::string AsmExporter::resolveName(uint16_t address) const
{
    auto it = nameMap_.find(address);
    if (it != nameMap_.end()) return it->second;
    return "";
}

// ---------------------------------------------------------------------------
// readByte — safe ROM read with wrap-around
// ---------------------------------------------------------------------------

uint8_t AsmExporter::readByte(uint16_t addr) const
{
    // NOTE: origin-relative mapping fix (found via MCP ROM-analysis agent).
    // The agent reported it as "relative addresses not accounted for", but the
    // real defect was that rom_ was indexed by absolute CPU address while it
    // actually holds file bytes mapped at config_.origin (default 0x0100) —
    // causing an off-by-origin misread for any non-zero origin.  i8080 has no
    // PC-relative jumps; the fix below maps addr -> (addr - origin) instead.
    //
    // rom_ holds the file bytes mapped at config_.origin, so a CPU address maps
    // to buffer offset (addr - origin).  Outside the ROM window -> 0x00 (NOP).
    if (addr < config_.origin) return 0x00;
    size_t off = static_cast<size_t>(addr) - config_.origin;
    if (off < rom_.size()) return rom_[off];
    return 0x00; // NOP for addresses beyond ROM
}

// ---------------------------------------------------------------------------
// computeSha256
// ---------------------------------------------------------------------------

std::string AsmExporter::computeSha256(const std::vector<uint8_t> &data)
{
    return Sha256::bufferDigest(data.data(), data.size());
}
