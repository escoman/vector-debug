#pragma once

#include "agent_types.h"
#include "agent_log.h"
#include "idebug_backend.h"
#include "events.h"
#include "code_analyzer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AgentApi — Stage 6.3
//
// AI Agent interface to the Vector-06C Debugger.
//
// All operations go through IDebugBackend — no direct access to Board,
// Memory, CPU, IO, TV, or any emulator internals.
//
// Stage 6.3 Contract:
//   - All public operations that can fail return AgentApiResult<T>
//   - ErrorCode is used consistently (including NotFound)
//   - No CommandResult exposed in public API
//   - No IDebugBackend types in public result structures
//   - std::optional<uint16_t> for getCallGraph (no $0000 ambiguity)
//   - Single loadRom() method (no loadRomInfo duplication)
// ---------------------------------------------------------------------------

class AgentApi
{
public:
    explicit AgentApi(IDebugBackend &backend);

    // -- Execution control (Stage 6.3: AgentApiResult<void>) -----------------

    AgentApiResult<void> run();
    AgentApiResult<void> pause();
    AgentApiResult<void> step();
    AgentApiResult<void> reset();
    bool isRunning() const;

    // -- CPU state (Stage 6.3: AgentApiResult<CpuState>) ---------------------

    AgentApiResult<CpuState> getCpuState();

    // -- Memory access (Stage 6.3: AgentApiResult + overflow checks) ---------

    AgentApiResult<std::vector<uint8_t>> readMemory(uint16_t address, size_t size);
    AgentApiResult<void> writeMemory(uint16_t address, const std::vector<uint8_t> &data);

    // -- I/O ports (Stage 6.3: uniform AgentApiResult) -----------------------

    AgentApiResult<uint8_t> readIo(uint8_t port);
    AgentApiResult<void> writeIo(uint8_t port, uint8_t value);

    // -- Virtual keyboard injection ------------------------------------------
    // Keys are named (case-insensitive) — see listKeys() for the authoritative
    // table.  pressKey/releaseKey drive the raw matrix bits; the ROM only sees
    // a key while the emulated CPU is running and polls the keyboard port, so
    // typeKey() presses, holds in real time, then releases.  Modifier keys
    // (SS/US/RUS) latch until released.

    AgentApiResult<void> pressKey(const std::string &keyName);
    AgentApiResult<void> releaseKey(const std::string &keyName);
    AgentApiResult<void> typeKey(const std::string &keyName);
    AgentApiResult<std::vector<KeyboardKeyInfo>> listKeys();

    // -- Breakpoints (Stage 6.3: AgentApiResult<void>) -----------------------

    AgentApiResult<void> setBreakpoint(uint16_t address);
    AgentApiResult<void> clearBreakpoint(uint16_t address);
    AgentApiResult<void> setBreakpointEnabled(uint16_t address, bool enabled);
    AgentApiResult<std::vector<DebuggerBreakpoint>> listBreakpoints();
    AgentApiResult<void> clearAllBreakpoints();

    // -- Registers -----------------------------------------------------------

    AgentApiResult<void> setRegister(const std::string &name, uint16_t value);

    // -- Disassembly ---------------------------------------------------------

    AgentApiResult<std::vector<DisassembledInstructionResult>>
    disassemble(uint16_t address, size_t count);

    // -- Code Analysis (Stage 6.16, 6.19) ------------------------------------

    AgentApiResult<CodeAnalysisResult>
    analyzeCode(uint16_t startAddress, size_t maxInstructions);

    AgentApiResult<CodeAnalysisResult>
    analyzeCode(const std::vector<uint16_t>& entryPoints, size_t maxInstructions);

    // -- Range Disassembly (Stage 6.18) --------------------------------------

    AgentApiResult<DisassembleRangeResult>
    disassembleRange(uint16_t address, uint16_t size);

    // -- Batch analysis (Stage 6.26) ------------------------------------------
    // Deterministic aggregation of existing operations for ROM analysis.
    // No code/data classification, no RDB modification, no state changes.

    // Whole-image linear sweep. Equivalent to disassembleRange() over the same
    // range (identical sweep algorithm), but accepts length up to 64K and
    // fails with LimitExceeded instead of silently truncating the result.
    AgentApiResult<DisassembleRangeResult>
    disassembleImage(uint16_t address, uint32_t length);

    // Coverage report: analyze code from entry points, then aggregate
    // code ranges / uncovered gaps / branch targets without analyzed code.
    // entryPoints is the single internal form (single start_address = size-1
    // vector) — callers must not care about analyzeCode()'s param exclusivity.
    AgentApiResult<CoverageReportResult>
    coverageReport(const std::vector<uint16_t> &entryPoints,
                   uint16_t imageStart, uint32_t imageLength,
                   size_t maxInstructions);

    // Complete byte-for-byte memory diff of two snapshots over a range.
    // Unlike compareMemorySnapshots(), reports old/new values and fails
    // with LimitExceeded rather than under-reporting dense diffs.
    AgentApiResult<MemoryDiffResult>
    diffMemorySnapshots(uint32_t idA, uint32_t idB,
                        uint16_t start, uint32_t length);

    // Byte-pattern search with optional per-byte mask (0 = wildcard).
    // mask empty ⇒ exact match. Addresses ascending, overlapping matches kept.
    AgentApiResult<ByteSequenceSearchResult>
    findBytecodeSequence(uint16_t rangeStart, uint16_t rangeEnd,
                         const std::vector<uint8_t> &pattern,
                         const std::vector<uint8_t> &mask,
                         size_t maxMatches);

    // Numeric operand (immediate/address) search using the debugger's own
    // disassembler for instruction interpretation — no second opcode decoder.
    AgentApiResult<ImmediateSearchResult>
    findImmediateInRange(uint16_t rangeStart, uint16_t rangeEnd,
                         uint16_t value, size_t maxMatches);

    // Raw VRAM bytes (memory-mapped plane region 0x8000..0xFFFF).
    AgentApiResult<std::vector<uint8_t>>
    getVramBytes(uint16_t address, uint32_t length);

    // -- Instruction History -------------------------------------------------

    AgentApiResult<std::vector<InstructionHistoryEntry>>
    getInstructionHistory(size_t count);

    // -- Trace / I/O (Stage 6.3: AgentApiResult) -----------------------------

    AgentApiResult<std::vector<InstructionEvent>> getExecutionTrace(size_t maxEntries = 1000);
    AgentApiResult<std::vector<IoAccessEvent>>    getIoTrace(size_t maxEntries = 1000);

    // -- Screen (Stage 6.3: AgentScreenSnapshot, no IDebugBackend types) -----

    AgentApiResult<AgentScreenSnapshot> getScreen();

    // -- Beam / raster (Stage 6.27: racing-the-beam debugging) ---------------
    // Read-only; never pauses/steps/re-renders. All values come from
    // DebugAdapter via the backend facade.

    AgentApiResult<AgentBeamState> getBeamState();
    // Real TV-frame buffer encoded to PNG (base64) + frame metadata.
    AgentApiResult<AgentScreenSnapshot> getScreenSnapshot();
    // OUT-with-beam-position events. Filters: frame (0=any), v_cycle range,
    // port (-1=any), pc (0xFFFF=any), maxResults.
    AgentApiResult<std::vector<AgentRasterEvent>> getRasterEvents(
        uint64_t frame = 0, uint32_t vCycleStart = 0,
        uint32_t vCycleEnd = 0xFFFFFFFFu, int port = -1,
        uint16_t pc = 0xFFFF,
        size_t maxResults = AgentLimits::RASTER_EVENTS_DEFAULT_LIMIT);
    AgentApiResult<void> clearRasterEvents();

    // -- Annotations (Stage 6.3: AgentApiResult<void>) -----------------------

    AgentApiResult<void> createFunction(uint16_t address, uint16_t size = 0);
    AgentApiResult<void> renameFunction(uint16_t address, const std::string &name);
    AgentApiResult<void> setFunctionComment(uint16_t address, const std::string &comment);
    AgentApiResult<void> deleteFunction(uint16_t address);
    AgentApiResult<void> addLabel(uint16_t address, const std::string &name);
    AgentApiResult<void> setComment(uint16_t address, const std::string &comment);
    AgentApiResult<void> applyAnnotation(const Annotation &annotation);

    // -- High-level analysis (Stage 6.3: AgentApiResult) ---------------------

    AgentApiResult<FunctionContext> getFunctionContext(uint16_t address);
    AgentApiResult<TraceResult>     traceFunction(uint16_t address);

    // -- Stack ---------------------------------------------------------------

    AgentApiResult<std::vector<StackEntry>>
    getStack(size_t limit);

    // -- Memory Map ----------------------------------------------------------

    AgentApiResult<std::vector<MemoryMapBlock>>
    getMemoryMap();

    // -- Runtime Memory Analysis (Stage 6.20) --------------------------------

    AgentApiResult<void> clearMemoryAccessMap();

    AgentApiResult<std::vector<RuntimeAccessBlock>>
    getMemoryAccessMap();

    AgentApiResult<std::vector<RuntimeAccessLogEntry>>
    getMemoryAccessLog(size_t maxEntries = 1000);

    AgentApiResult<uint32_t>
    createMemorySnapshot(uint16_t start = 0, size_t size = 65536);

    AgentApiResult<MemorySnapshotData>
    getMemorySnapshot(uint32_t snapshotId);

    AgentApiResult<MemorySnapshotDiff>
    compareMemorySnapshots(uint32_t idA, uint32_t idB);

    // -- Screen Info ---------------------------------------------------------

    AgentApiResult<ScreenInfoResult> getScreenInfo();

    // -- VRAM Info -----------------------------------------------------------

    AgentApiResult<VramInfoResult> getVramInfo();

    // -- Symbols -------------------------------------------------------------

    AgentApiResult<std::vector<SymbolInfo>>
    getSymbols(size_t limit = 0);

    AgentApiResult<SymbolInfo> getFunction(uint16_t address);

    // -- Xrefs ---------------------------------------------------------------

    AgentApiResult<std::vector<XrefResult>>
    getXrefs(uint16_t address);

    // -- Call Graph (Stage 6.3: std::optional<uint16_t> — no $0000 ambiguity) -

    AgentApiResult<std::vector<CallGraphEdge>>
    getCallGraph(std::optional<uint16_t> address = std::nullopt, size_t limit = 0);

    // -- ROM (Stage 6.3: single loadRom method) ------------------------------

    AgentApiResult<LoadRomResult> loadRom(const std::string &path, uint32_t org = 0);

    // -- Debug State ---------------------------------------------------------

    AgentApiResult<DebugStateResult> getDebugState();

    // -- ROM Database (Stage 6.11) -------------------------------------------

    AgentApiResult<RdbInfoResult> getRdbInfo();

    AgentApiResult<std::vector<RdbObjectResult>> listRdbObjects(size_t limit = 0);

    AgentApiResult<RdbObjectResult> getRdbObject(uint16_t address);

    AgentApiResult<RdbObjectResult> findRdbObject(const std::string &name);

    AgentApiResult<void> addRdbObject(uint16_t address, const std::string &name,
                                      const std::string &type = "Label",
                                      uint32_t size = 0);

    AgentApiResult<void> updateRdbObject(uint16_t address, const std::string &name,
                                         const std::string &type,
                                         uint32_t size, bool hasSize);

    AgentApiResult<void> removeRdbObject(uint16_t address);

    AgentApiResult<void> setRdbComment(uint16_t address, const std::string &comment);

    AgentApiResult<void> setRdbProperty(uint16_t address,
                                        const std::string &propName,
                                        const std::string &propValue);

    AgentApiResult<void> saveRdb();

    AgentApiResult<void> reloadRdb();

    // -- RDB Links (Stage 6.13) -----------------------------------------------

    AgentApiResult<void> addRdbLink(uint16_t sourceAddress, uint16_t targetAddress);

    AgentApiResult<void> removeRdbLink(uint16_t sourceAddress, uint16_t targetAddress);

    AgentApiResult<std::vector<uint16_t>> getRdbLinks(uint16_t sourceAddress);

    // -- Agent log -----------------------------------------------------------

    const AgentLog &log() const;
    void clearLog();

private:
    IDebugBackend &backend_;
    AgentLog       log_;

    // -- Internal helpers ---------------------------------------------------

    // Shared linear sweep used by disassembleRange() and disassembleImage().
    // No validation — caller guarantees size fits the address space.
    DisassembleRangeResult
    linearDisassemble(uint16_t address, uint32_t size);

    // Stage: fix empty callers/xrefs — rebuild the SymbolDatabase xref/call
    // graph lazily (only when its cache is dirty) from current memory +
    // symbols, so getXrefs()/getCallGraph()/getFunctionContext() reflect the
    // loaded ROM instead of always returning empty. Cheap: skipped when clean.
    void ensureXrefsBuilt();

    // Disassemble a function starting at 'address' until RET/HLT/unconditional
    // JMP or the next known symbol.  Returns the instruction list.
    std::vector<FunctionContext::Instruction> disassembleFunction(uint16_t address);

    // Estimate function size by linear disassembly until RET/HLT/JMP.
    uint16_t estimateFunctionSize(uint16_t address);

    // Collect attributed memory/IO/VRAM events from history using
    // instruction sequence range [startSeq, endSeq).
    void collectTraceEvents(
        uint64_t startSeq, uint64_t endSeq,
        std::vector<TraceMemoryAccess> &memReads,
        std::vector<TraceMemoryAccess> &memWrites,
        std::vector<TraceIoAccess> &ioReads,
        std::vector<TraceIoAccess> &ioWrites,
        std::vector<TraceVramWrite> &vramWrites);

    // Timer helper for logging.
    static double elapsedMs(std::chrono::steady_clock::time_point start);
};
