#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <functional>

// ---------------------------------------------------------------------------
// Symbol Database — Stage 4.1
//
// Core data structures for reverse-engineering annotations:
//   - Symbols (functions, labels) with names and comments
//   - Memory region classification (Code / Data / Unknown)
//   - Cross-references (xrefs) between addresses
//   - Call graph edges
//
// No GUI or board dependency — fully testable in isolation.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Symbol types
// ---------------------------------------------------------------------------

enum class SymbolType
{
    Function,
    Label
};

struct DebugSymbol
{
    uint16_t address;
    std::string name;
    std::string comment;
    SymbolType type;
    // Stage 6.2: MAP source location
    std::string sourceFile;  // e.g. "main.c"
    int         sourceLine = 0;
    // Stage 6.2: origin tracking (for ROM reload cleanup)
    bool        fromMap    = false;
};

// ---------------------------------------------------------------------------
// Memory region classification
// ---------------------------------------------------------------------------

enum class MemoryRegionType
{
    Unknown,
    Code,
    Data
};

struct MemoryRegion
{
    uint16_t start;
    uint16_t end;       // inclusive
    MemoryRegionType type;
    std::string comment;
};

// ---------------------------------------------------------------------------
// Cross-references
// ---------------------------------------------------------------------------

struct XrefEntry
{
    uint16_t from;   // source address (where the CALL/JMP is)
    uint16_t to;     // target address
};

// ---------------------------------------------------------------------------
// SymbolDatabase
// ---------------------------------------------------------------------------

class SymbolDatabase
{
public:
    // -- Symbols -------------------------------------------------------------

    // Add a symbol. Returns false if address already has a symbol.
    bool addSymbol(uint16_t addr, const std::string &name, SymbolType type);

    // Remove symbol at address. Returns false if not found.
    bool removeSymbol(uint16_t addr);

    // Rename symbol. Returns false if not found.
    bool renameSymbol(uint16_t addr, const std::string &newName);

    // Set/update comment. Returns false if symbol not found.
    bool setComment(uint16_t addr, const std::string &comment);

    // Lookup by address. Returns nullptr if not found.
    const DebugSymbol *findSymbol(uint16_t addr) const;

    // Lookup by name (case-sensitive). Returns nullptr if not found.
    const DebugSymbol *findSymbolByName(const std::string &name) const;

    // All symbols sorted by address.
    std::vector<DebugSymbol> allSymbols() const;

    // Number of symbols.
    size_t symbolCount() const;

    // Auto-generated name for unknown function: "sub_XXXX"
    static std::string autoName(uint16_t addr);

    // Display name: user-defined name if exists, otherwise autoName if the
    // address is a known call target, otherwise empty string.
    std::string displayName(uint16_t addr) const;

    // Mark an address as a known call target (for auto-name generation).
    // Does NOT create a user symbol — just enables autoName for this address.
    void markCallTarget(uint16_t addr);

    // -- Memory classification -----------------------------------------------

    // Set a region classification. If overlapping regions exist, they are
    // adjusted. Returns true on success.
    bool setRegion(uint16_t start, uint16_t end, MemoryRegionType type);

    // Remove region starting at 'start'. Returns false if not found.
    bool removeRegion(uint16_t start);

    // Get classification for a single address.
    MemoryRegionType classify(uint16_t addr) const;

    // All regions sorted by start address.
    std::vector<MemoryRegion> allRegions() const;

    // -- Cross-references ----------------------------------------------------

    // Rebuild xrefs by scanning all memory for CALL/JMP/PCHL targets.
    // readByte: function to read a byte from memory (e.g. backend.readMemory).
    // Only scans regions classified as Code (or all memory if none classified).
    void rebuildXrefs(std::function<uint8_t(uint16_t)> readByte);

    // Invalidate the cached xref / call-target scan so the next rebuildXrefs()
    // recomputes from current memory + symbols. Called on ROM load, clear() and
    // whenever symbols/regions change (createFunction, analyzeCode, setRegion).
    void invalidateXrefs() { xrefsDirty_ = true; }

    // Whether the cached xrefs are stale and must be rebuilt before querying.
    bool xrefsAreDirty() const { return xrefsDirty_; }

    // All references TO a given address.
    std::vector<XrefEntry> xrefsTo(uint16_t addr) const;

    // All references FROM a given address.
    std::vector<XrefEntry> xrefsFrom(uint16_t addr) const;

    // All xrefs.
    const std::vector<XrefEntry> &allXrefs() const { return xrefs_; }

    // -- Call graph ----------------------------------------------------------

    struct CallEdge
    {
        uint16_t from;
        uint16_t to;
    };

    // Call graph edges (derived from xrefs: only CALL instructions).
    std::vector<CallEdge> callGraph() const;

    // -- Clear all data ------------------------------------------------------

    void clear();

    // -- MAP symbols (Stage 6.2) ---------------------------------------------

    // Remove all symbols that were loaded from a MAP file.
    // Preserves user-defined symbols (fromMap == false).
    void clearMapSymbols();

private:
    // Symbols indexed by address
    std::map<uint16_t, DebugSymbol> symbols_;

    // Memory regions sorted by start address
    std::vector<MemoryRegion> regions_;

    // Cross-references
    std::vector<XrefEntry> xrefs_;

    // Known call targets (for auto-name generation)
    std::map<uint16_t, bool> callTargets_;

    // Cached xref scan validity. Starts dirty (nothing scanned yet) and is
    // cleared by rebuildXrefs(); set again by invalidateXrefs()/clear().
    bool xrefsDirty_ = true;

    // Helper: check if opcode is CALL
    static bool isCallOpcode(uint8_t opcode);

    // Helper: check if opcode is JMP
    static bool isJmpOpcode(uint8_t opcode);

    // Helper: check if opcode is RST
    static bool isRstOpcode(uint8_t opcode);
};
