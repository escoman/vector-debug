#include "symbol_database.h"
#include "opcode_info.h"
#include "rdb_types.h"   // normalizeRdbText — shared text sanitizer (header-only)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------

bool SymbolDatabase::addSymbol(uint16_t addr, const std::string &name, SymbolType type)
{
    if (symbols_.count(addr)) {
        return false;  // already exists
    }
    DebugSymbol sym;
    sym.address = addr;
    sym.name = name;
    sym.type = type;
    symbols_[addr] = sym;
    invalidateXrefs();
    return true;
}

bool SymbolDatabase::removeSymbol(uint16_t addr)
{
    bool removed = symbols_.erase(addr) > 0;
    if (removed) invalidateXrefs();
    return removed;
}

bool SymbolDatabase::renameSymbol(uint16_t addr, const std::string &newName)
{
    auto it = symbols_.find(addr);
    if (it == symbols_.end()) return false;
    it->second.name = newName;
    return true;
}

bool SymbolDatabase::setComment(uint16_t addr, const std::string &comment)
{
    auto it = symbols_.find(addr);
    if (it == symbols_.end()) return false;
    it->second.comment = normalizeRdbText(comment);   // R1: no line breaks in comments
    return true;
}

const DebugSymbol *SymbolDatabase::findSymbol(uint16_t addr) const
{
    auto it = symbols_.find(addr);
    if (it == symbols_.end()) return nullptr;
    return &it->second;
}

const DebugSymbol *SymbolDatabase::findSymbolByName(const std::string &name) const
{
    for (const auto &pair : symbols_) {
        if (pair.second.name == name) {
            return &pair.second;
        }
    }
    return nullptr;
}

std::vector<DebugSymbol> SymbolDatabase::allSymbols() const
{
    // std::map is already sorted by key (address)
    std::vector<DebugSymbol> result;
    result.reserve(symbols_.size());
    for (const auto &pair : symbols_) {
        result.push_back(pair.second);
    }
    return result;
}

size_t SymbolDatabase::symbolCount() const
{
    return symbols_.size();
}

std::string SymbolDatabase::autoName(uint16_t addr)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "sub_%04X", addr);
    return buf;
}

std::string SymbolDatabase::displayName(uint16_t addr) const
{
    const DebugSymbol *sym = findSymbol(addr);
    if (sym) {
        return sym->name;  // user-defined name
    }
    // If it's a known call target, return auto-name
    if (callTargets_.count(addr)) {
        return autoName(addr);
    }
    return "";
}

void SymbolDatabase::markCallTarget(uint16_t addr)
{
    callTargets_[addr] = true;
}

// ---------------------------------------------------------------------------
// Memory classification
// ---------------------------------------------------------------------------

bool SymbolDatabase::setRegion(uint16_t start, uint16_t end, MemoryRegionType type)
{
    if (start > end) return false;

    // Remove any overlapping portions of existing regions, then insert new one.
    // This is a simplified approach: we split/trim existing regions to avoid overlap.

    std::vector<MemoryRegion> adjusted;
    for (const auto &r : regions_) {
        if (r.end < start || r.start > end) {
            // No overlap — keep as is
            adjusted.push_back(r);
        } else {
            // Overlap exists. Keep the non-overlapping parts.
            if (r.start < start) {
                // Part before the new region
                MemoryRegion before = r;
                before.end = static_cast<uint16_t>(start - 1);
                adjusted.push_back(before);
            }
            if (r.end > end) {
                // Part after the new region
                MemoryRegion after = r;
                after.start = static_cast<uint16_t>(end + 1);
                adjusted.push_back(after);
            }
            // The overlapping part is removed (replaced by the new region)
        }
    }

    // Insert the new region
    MemoryRegion newRegion;
    newRegion.start = start;
    newRegion.end = end;
    newRegion.type = type;
    adjusted.push_back(newRegion);

    // Sort by start address
    std::sort(adjusted.begin(), adjusted.end(),
              [](const MemoryRegion &a, const MemoryRegion &b) {
                  return a.start < b.start;
              });

    regions_ = adjusted;
    invalidateXrefs();
    return true;
}

bool SymbolDatabase::removeRegion(uint16_t start)
{
    for (auto it = regions_.begin(); it != regions_.end(); ++it) {
        if (it->start == start) {
            regions_.erase(it);
            invalidateXrefs();
            return true;
        }
    }
    return false;
}

MemoryRegionType SymbolDatabase::classify(uint16_t addr) const
{
    for (const auto &r : regions_) {
        if (addr >= r.start && addr <= r.end) {
            return r.type;
        }
    }
    return MemoryRegionType::Unknown;
}

std::vector<MemoryRegion> SymbolDatabase::allRegions() const
{
    return regions_;  // already sorted by start
}

// ---------------------------------------------------------------------------
// Opcode classification helpers (8080)
// ---------------------------------------------------------------------------

bool SymbolDatabase::isCallOpcode(uint8_t opcode)
{
    // CALL variants: CD, C4, CC, D4, DC, E4, EC, F4, FC
    switch (opcode) {
        case 0xCD: // CALL addr
        case 0xC4: // CNZ
        case 0xCC: // CZ
        case 0xD4: // CNC
        case 0xDC: // CC
        case 0xE4: // CPO
        case 0xEC: // CPE
        case 0xF4: // CP
        case 0xFC: // CM
            return true;
        default:
            return false;
    }
}

bool SymbolDatabase::isJmpOpcode(uint8_t opcode)
{
    // JMP variants: C3, C2, CA, D2, DA, E2, EA, F2, FA
    switch (opcode) {
        case 0xC3: // JMP addr
        case 0xC2: // JNZ
        case 0xCA: // JZ
        case 0xD2: // JNC
        case 0xDA: // JC
        case 0xE2: // JPO
        case 0xEA: // JPE
        case 0xF2: // JP
        case 0xFA: // JM
            return true;
        default:
            return false;
    }
}

bool SymbolDatabase::isRstOpcode(uint8_t opcode)
{
    // RST N: C7, CF, D7, DF, E7, EF, F7, FF
    switch (opcode) {
        case 0xC7: case 0xCF:
        case 0xD7: case 0xDF:
        case 0xE7: case 0xEF:
        case 0xF7: case 0xFF:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Cross-references
// ---------------------------------------------------------------------------

void SymbolDatabase::rebuildXrefs(std::function<uint8_t(uint16_t)> readByte)
{
    xrefs_.clear();
    callTargets_.clear();

    auto readWord = [&readByte](uint16_t a) -> uint16_t {
        uint16_t hi = static_cast<uint16_t>(a + 1);  // wraps at 0xFFFF
        uint8_t lo = readByte(a);
        uint8_t hb = readByte(hi);
        return static_cast<uint16_t>((hb << 8) | lo);
    };

    // Scan ranges: Code regions if any, otherwise the whole 64K space.
    std::vector<std::pair<uint16_t, uint16_t>> scanRanges;
    for (const auto &r : regions_) {
        if (r.type == MemoryRegionType::Code)
            scanRanges.push_back({r.start, r.end});
    }
    const bool haveCodeRegions = !scanRanges.empty();
    if (scanRanges.empty())
        scanRanges.push_back({0x0000, 0xFFFF});

    // A source address is a single instruction, so record at most one xref
    // per source. Call/RST sources additionally register their target.
    std::set<uint16_t> seenFrom;
    auto record = [&](uint16_t from, uint16_t to, bool isCall) {
        if (seenFrom.insert(from).second) {
            XrefEntry xref;
            xref.from = from;
            xref.to = to;
            xrefs_.push_back(xref);
        }
        if (isCall) callTargets_[to] = true;
    };

    auto decodeAt = [&](uint16_t addr, uint8_t opcode, uint8_t len) {
        if (isCallOpcode(opcode) && len == 3) {
            record(addr, readWord(static_cast<uint16_t>(addr + 1)), true);
        } else if (isJmpOpcode(opcode) && len == 3) {
            record(addr, readWord(static_cast<uint16_t>(addr + 1)), false);
        } else if (isRstOpcode(opcode)) {
            record(addr, static_cast<uint16_t>(opcode & 0x38), true);
        }
    };

    // ---- Pass 1: aligned linear decode, seeded at every region start and
    // every known symbol address. Seeding restarts instruction alignment at
    // each boundary, so embedded data no longer desynchronises the whole walk.
    std::vector<uint16_t> seeds;
    for (const auto &range : scanRanges) seeds.push_back(range.first);
    for (const auto &kv : symbols_) seeds.push_back(kv.first);
    std::sort(seeds.begin(), seeds.end());
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());

    for (uint16_t seed : seeds) {
        const std::pair<uint16_t, uint16_t> *containing = nullptr;
        for (const auto &range : scanRanges) {
            if (seed >= range.first && seed <= range.second) {
                containing = &range;
                break;
            }
        }
        if (!containing) continue;

        uint16_t addr = seed;
        while (addr <= containing->second) {
            uint8_t opcode = readByte(addr);
            uint8_t len = opcode_info::get_length(opcode);
            if (len == 0) len = 1;
            decodeAt(addr, opcode, len);
            uint16_t next = static_cast<uint16_t>(addr + len);
            if (next <= addr) break;  // wrapped
            addr = next;
        }
    }

    // ---- Pass 2: exhaustive every-byte CALL/RST recovery, accepted only when
    // the 16-bit target points to a known entity (symbol, existing call target,
    // or inside a Code region). This recovers call sites Pass 1 desynchronised
    // over, while the filter keeps data false-positives out of the graph.
    // Skipped when nothing is known to filter against (honest full-scan default).
    if (haveCodeRegions || !symbols_.empty()) {
        auto targetIsPlausible = [&](uint16_t target) -> bool {
            if (symbols_.count(target)) return true;
            if (callTargets_.count(target)) return true;
            for (const auto &r : regions_) {
                if (r.type == MemoryRegionType::Code &&
                    target >= r.start && target <= r.end) return true;
            }
            return false;
        };

        for (uint16_t addr = 0x0000; ; addr = static_cast<uint16_t>(addr + 1)) {
            if (!seenFrom.count(addr)) {
                uint8_t opcode = readByte(addr);
                if (isCallOpcode(opcode)) {
                    uint16_t target = readWord(static_cast<uint16_t>(addr + 1));
                    if (targetIsPlausible(target)) record(addr, target, true);
                } else if (isRstOpcode(opcode)) {
                    uint16_t target = static_cast<uint16_t>(opcode & 0x38);
                    if (targetIsPlausible(target)) record(addr, target, true);
                }
            }
            if (addr == 0xFFFF) break;
        }
    }

    xrefsDirty_ = false;
}

std::vector<XrefEntry> SymbolDatabase::xrefsTo(uint16_t addr) const
{
    std::vector<XrefEntry> result;
    for (const auto &x : xrefs_) {
        if (x.to == addr) {
            result.push_back(x);
        }
    }
    return result;
}

std::vector<XrefEntry> SymbolDatabase::xrefsFrom(uint16_t addr) const
{
    std::vector<XrefEntry> result;
    for (const auto &x : xrefs_) {
        if (x.from == addr) {
            result.push_back(x);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Call graph
// ---------------------------------------------------------------------------

std::vector<SymbolDatabase::CallEdge> SymbolDatabase::callGraph() const
{
    // Call graph = xrefs where the source instruction is a CALL or RST
    // (not JMP — JMP is a jump, not a call).
    // We need the opcode at the source address to distinguish.
    // Since we don't store the opcode in XrefEntry, we use a heuristic:
    // call graph edges are xrefs where the target is a call target.
    // This is an approximation — for precise results, we'd need to store
    // the opcode type in XrefEntry.

    // For now, return all xrefs where target is a known call target.
    // This includes CALL and RST targets but excludes pure JMP targets.
    std::vector<CallEdge> edges;
    for (const auto &x : xrefs_) {
        if (callTargets_.count(x.to)) {
            CallEdge edge;
            edge.from = x.from;
            edge.to = x.to;
            edges.push_back(edge);
        }
    }
    return edges;
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void SymbolDatabase::clear()
{
    symbols_.clear();
    regions_.clear();
    xrefs_.clear();
    callTargets_.clear();
    xrefsDirty_ = true;
}

// ---------------------------------------------------------------------------
// MAP symbols (Stage 6.2)
// ---------------------------------------------------------------------------

void SymbolDatabase::clearMapSymbols()
{
    for (auto it = symbols_.begin(); it != symbols_.end(); ) {
        if (it->second.fromMap) {
            it = symbols_.erase(it);
        } else {
            ++it;
        }
    }
}
