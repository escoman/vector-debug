#include "backend.h"
#include "events.h"
#include "opcode_info.h"
#include "ring_buffer.h"
#include "debug_memory.h"
#include "map_loader.h"
#include "map_import.h"
#include "sha256.h"

#include <cstdio>
#include <cstring>
#include <climits>
#include <memory>
#include <thread>
#include <fstream>
#include <sstream>


// ---------------------------------------------------------------------------
// Default ring-buffer capacities
// ---------------------------------------------------------------------------

static const size_t DEFAULT_INSTRUCTION_HISTORY = 10000;
static const size_t DEFAULT_MEMORY_HISTORY      = 50000;
static const size_t DEFAULT_IO_HISTORY          = 10000;
static const size_t DEFAULT_RUNTIME_ACCESS_LOG  = 50000;  // Stage 6.20

// ---------------------------------------------------------------------------
// Impl — heap-allocated ring buffers + memory statistics
// ---------------------------------------------------------------------------

struct DebugBackend::Impl
{
    RingBuffer<InstructionEvent>  instrHistory;
    RingBuffer<MemoryAccessEvent> memHistory;
    RingBuffer<IoAccessEvent>     ioHistory;
    MemoryStats                   memStats[65536];

    Impl()
        : instrHistory(DEFAULT_INSTRUCTION_HISTORY)
        , memHistory(DEFAULT_MEMORY_HISTORY)
        , ioHistory(DEFAULT_IO_HISTORY)
    {
        clearStats();
    }

    void clearStats()
    {
        using TimePoint = MemoryStats::TimePoint;
        for (int i = 0; i < 65536; ++i) {
            memStats[i].reads = 0;
            memStats[i].writes = 0;
            memStats[i].lastReadSequence = UINT64_MAX;
            memStats[i].lastWriteSequence = UINT64_MAX;
            memStats[i].lastReadTime = TimePoint::min();
            memStats[i].lastWriteTime = TimePoint::min();
        }
    }
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DebugBackend::DebugBackend(IDebugTarget &target)
    : target_(&target)
    , state_(DebuggerState::Paused)
    , nextId_(1)
    , instructionSequence_(0)
    , instrumentationEnabled_(true)
    , fetchRemaining_(0)
    , impl_(new Impl())
    , rdb_(new RdbController())
    , runtimeAccessLog_(new RingBuffer<RuntimeAccessLogEntry>(DEFAULT_RUNTIME_ACCESS_LOG))
{
    clearActivityCounters();
    // Initialize runtime access map (Stage 6.20)
    for (int i = 0; i < 256; ++i) {
        runtimeAccessMap_[i].address = static_cast<uint16_t>(i * 256);
    }
    installMemoryCallbacks();
}

DebugBackend::~DebugBackend()
{
    // Clear memory callbacks before destroying
    if (target_) {
        target_->setMemoryCallbacks(nullptr, nullptr);
    }
    delete runtimeAccessLog_;
    delete impl_;
    delete rdb_;
}

// ---------------------------------------------------------------------------
// Target integration (Stage 3.13a)
// ---------------------------------------------------------------------------

void DebugBackend::attachTarget(IDebugTarget *target)
{
    target_ = target;
}

bool DebugBackend::loadRom(const std::string &path, uint32_t org)
{
    if (!target_) return false;

    // Replacing the machine is handed to the emulation thread: a frame of the
    // previous ROM may still be running when the user loads a new one, because
    // the pause flag is only sampled once per frame (poll_debugger), and the
    // load zeroes all 64 KiB under the CPU's feet.
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::LoadRom;
    cmd->name = path;
    cmd->org = org;

    CommandResult loadResult;
    if (!testSynchronous_ && !emulationLoopRunning_.load(std::memory_order_acquire)) {
        // gui/main.cpp loads the command-line ROM before the emulation thread
        // exists: nothing can race with us, and there is nobody to wait for.
        auto future = cmd->promise.get_future();
        cmd->state.store(CommandState::Executing, std::memory_order_release);
        executeCommand(*cmd);
        loadResult = future.get();
    } else {
        loadResult = submitAndWait(std::move(cmd));
    }

    if (!loadResult.success) {
        printf("DebugBackend::loadRom(): failed to load %s\n", path.c_str());
        return false;
    }

    printf("DebugBackend::loadRom(): loaded %s at %04x\n", path.c_str(), org);

    // Clear debug history
    clearHistory();
    instructionSequence_ = 0;
    freeRunSequenceStarted_ = false;

    // Stage 6.20.1: Clear breakpoints from previous ROM session.
    // Breakpoints belong to the debug session, not the ROM —
    // stale breakpoints must not carry over to the newly loaded ROM.
    // syncBreakpointsToTarget() propagates empty list to DebugAdapter
    // and Board, clearing syncedBreakpoints_ as well.
    clearBreakpoints();
    syncBreakpointsToTarget();

    // Stage 6.20: Invalidate runtime analysis state from previous ROM
    clearRuntimeAccessMap();
    invalidateAllSnapshots();

    // Set state to Paused — regardless of previous running state.
    // running_ must be cleared so the emulation thread stops executing
    // frames and blocks on the command queue until the user presses RUN.
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = DebuggerState::Paused;
    }
    pauseRequestedAtomic_.store(false, std::memory_order_release);

    // Stage 6.2: Clear MAP symbols from previous ROM
    symbols_.clearMapSymbols();

    // Stage 6.11: Load ROM Database (.rdb)
    loadRdb(path);

    // Stage 6.11: If RDB is empty and .map exists — auto-import MAP into RDB
    if (rdb_->objectCount() == 0) {
        std::string mapPath = MapLoader::mapPathFromRom(path);
        MapLoadResult mapResult = MapLoader::loadMapFile(mapPath);
        if (mapResult.success) {
            int imported = MapImport::importMapToRdb(mapResult, *rdb_);
            printf("DebugBackend::loadRom(): imported MAP %s → RDB (%d objects, %d skipped lines)\n",
                   mapPath.c_str(), imported, mapResult.skippedLines);
        } else {
            printf("DebugBackend::loadRom(): MAP not loaded (%s)\n",
                   mapResult.errorMessage.c_str());
        }
    }

    // Stage 6.11: Sync RDB → SymbolDatabase (backward compatibility)
    {
        int synced = MapImport::syncRdbToSymbolDatabase(*rdb_, symbols_);
        if (synced > 0) {
            printf("DebugBackend::loadRom(): synced RDB → SymbolDatabase (%d symbols)\n", synced);
        }
    }

    // The xref / call-graph cache is a derived view of the loaded ROM image.
    // Invalidate it so the next getXrefs()/getCallGraph()/getFunctionContext()
    // rebuilds against this ROM instead of returning stale or empty results.
    symbols_.invalidateXrefs();

    return true;
}

// ---------------------------------------------------------------------------
// ROM Database (Stage 6.11) — .rdb lifecycle
// ---------------------------------------------------------------------------

std::string DebugBackend::rdbPathFromRom(const std::string &romPath)
{
    // .rdb sits next to the ROM: replace extension with .rdb
    // e.g. "clrs.rom" -> "clrs.rdb"
    size_t dotPos = romPath.rfind('.');
    size_t slashPos = romPath.rfind('/');
    
    // Only strip if dot is after the last slash (i.e., it's a file extension)
    if (dotPos != std::string::npos && (slashPos == std::string::npos || dotPos > slashPos)) {
        return romPath.substr(0, dotPos) + ".rdb";
    }
    // No extension — just append
    return romPath + ".rdb";
}

void DebugBackend::loadRdb(const std::string &romPath)
{
    // Reset RDB controller to clean state
    rdb_->close();

    // Compute ROM identity: filename, size, SHA-256
    RdbRomIdentity identity;
    
    // Store only filename (not full path) for portability
    size_t lastSlash = romPath.rfind('/');
    identity.file = (lastSlash != std::string::npos) 
        ? romPath.substr(lastSlash + 1) 
        : romPath;

    // Get file size
    std::ifstream romFile(romPath, std::ios::binary | std::ios::ate);
    if (romFile.is_open()) {
        identity.size = static_cast<int64_t>(romFile.tellg());
        romFile.close();
    } else {
        identity.size = -1;
    }

    // Compute SHA-256
    identity.sha256 = Sha256::fileDigest(romPath);

    // Set ROM identity on the controller
    rdb_->setRomIdentity(identity);

    // Derive .rdb path and try to load
    rdbPath_ = rdbPathFromRom(romPath);

    // Check if .rdb file exists
    std::ifstream testFile(rdbPath_);
    if (testFile.is_open()) {
        testFile.close();
        if (rdb_->load(rdbPath_)) {
            printf("DebugBackend::loadRdb(): loaded %s (%zu objects)\n",
                   rdbPath_.c_str(), rdb_->objectCount());
        } else {
            printf("DebugBackend::loadRdb(): failed to parse %s\n",
                   rdbPath_.c_str());
        }
    } else {
        // No .rdb file — initialize empty in-memory RDB with path for future save
        rdb_->initialize("vector06c", identity, rdbPath_);
        printf("DebugBackend::loadRdb(): no .rdb file, using in-memory RDB\n");
    }
}

bool DebugBackend::loadWav(const std::string &path)
{
    if (!target_) return false;

    // Pause emulation before loading (same as loadRom)
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = DebuggerState::Paused;
    }
    pauseRequestedAtomic_.store(false, std::memory_order_release);

    if (!target_->loadWav(path)) {
        printf("DebugBackend::loadWav(): failed to load %s\n", path.c_str());
        return false;
    }

    printf("DebugBackend::loadWav(): loaded %s\n", path.c_str());

    // Clear debug history
    clearHistory();
    instructionSequence_ = 0;
    freeRunSequenceStarted_ = false;

    return true;
}

// ---------------------------------------------------------------------------
// Keyboard injection
// ---------------------------------------------------------------------------

void DebugBackend::pressKey(int scancode)
{
    if (target_) target_->pressKey(scancode);
}

void DebugBackend::releaseKey(int scancode)
{
    if (target_) target_->releaseKey(scancode);
}

bool DebugBackend::isRuslatMode() const
{
    if (target_) return target_->isRuslatMode();
    return false;
}

// ---------------------------------------------------------------------------
// I/O ports (Stage 6.1 Iteration 3)
// ---------------------------------------------------------------------------

uint8_t DebugBackend::readIoPort(uint8_t port)
{
    if (!target_) return 0xFF;
    return target_->readIoPort(port);
}

CommandResult DebugBackend::writeIoPort(uint8_t port, uint8_t value)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::IoWrite;
    cmd->address = port;  // port number fits in uint16_t
    cmd->ioValue = value;
    return submitAndWait(std::move(cmd));
}

// ---------------------------------------------------------------------------
// Memory callback installation (with chaining)
// ---------------------------------------------------------------------------

void DebugBackend::installMemoryCallbacks()
{
    if (!target_) return;

    DebugBackend *self = this;

    IDebugTarget::MemoryReadCallback readCb =
        [self](uint32_t virt, uint32_t phys, bool stack, uint8_t value,
               uint16_t pc) {
            self->onMemoryRead(virt, phys, stack, value, pc);
        };

    IDebugTarget::MemoryWriteCallback writeCb =
        [self](uint32_t virt, uint32_t phys, bool stack, uint8_t value) {
            self->onMemoryWrite(virt, phys, stack, value);
        };

    target_->setMemoryCallbacks(readCb, writeCb);

    // Stage 6.24: instruction-begin detection is now inside onMemoryRead(),
    // driven by the pc parameter of MemoryReadCallback.  No separate
    // setInstructionBeginCallback() call is needed.
}

// ---------------------------------------------------------------------------
// Memory access (Stage 3.3 — for Memory Inspector)
// ---------------------------------------------------------------------------

uint8_t DebugBackend::readMemory(uint16_t address)
{
    return target_->readMemory(address);
}

MemorySnapshot DebugBackend::readMemorySnapshot(uint16_t start, size_t size)
{
    MemorySnapshot snapshot;
    snapshot.start = start;
    snapshot.data.reserve(size);

    for (size_t i = 0; i < size; ++i) {
        uint16_t addr = static_cast<uint16_t>((start + i) & 0xFFFF);
        snapshot.data.push_back(target_->readMemoryRaw(addr));
    }

    return snapshot;
}

// ---------------------------------------------------------------------------
// Memory write (Stage 5.3.2 — through Command Queue)
// ---------------------------------------------------------------------------

bool DebugBackend::writeMemoryByte(uint16_t address, uint8_t value)
{
    return writeMemory(address, &value, 1);
}

bool DebugBackend::writeMemory(uint16_t address, const uint8_t* data, size_t size)
{
    if (!data || size == 0) return false;

    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::MemoryWrite;
    cmd->address = address;
    cmd->writeData.assign(data, data + size);
    auto result = submitAndWait(std::move(cmd));
    return result.success;
}

// ---------------------------------------------------------------------------
// Register write (Stage 5.3.2 — through Command Queue)
// ---------------------------------------------------------------------------

bool DebugBackend::writeRegister(RegisterId id, uint16_t value)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::RegisterWrite;
    cmd->regId = id;
    cmd->regValue = value;
    auto result = submitAndWait(std::move(cmd));
    return result.success;
}

// ---------------------------------------------------------------------------
// CPU state
// ---------------------------------------------------------------------------

CpuState DebugBackend::getCpuState() const
{
    CpuState s = target_->getCpuState();
    s.last_pc = lastPc_;
    return s;
}

DebuggerState DebugBackend::getState() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return state_;
}

bool DebugBackend::isPaused() const
{
    // Stage 5.3.3.2: thread-safe read of state_ via stateMutex_
    std::lock_guard<std::mutex> lock(stateMutex_);
    return state_ == DebuggerState::Paused;
}

// ---------------------------------------------------------------------------
// Execution control
// ---------------------------------------------------------------------------

void DebugBackend::reset()
{
    target_->reset(true);  // Reset: attach boot ROM, PC=0

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = DebuggerState::Paused;
    }
    pauseRequestedAtomic_.store(false, std::memory_order_release);
    stopReason_ = StopReason::Reset;

    instructionSequence_ = 0;
    freeRunSequenceStarted_ = false;
    impl_->instrHistory.clear();
    impl_->memHistory.clear();
    impl_->ioHistory.clear();
    impl_->clearStats();
    clearActivityCounters();
}

void DebugBackend::restart()
{
    target_->reset(false);  // Restart: detach boot ROM, PC=0, execute from RAM

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = DebuggerState::Paused;
    }
    pauseRequestedAtomic_.store(false, std::memory_order_release);
    stopReason_ = StopReason::Reset;

    instructionSequence_ = 0;
    freeRunSequenceStarted_ = false;
    impl_->instrHistory.clear();
    impl_->memHistory.clear();
    impl_->ioHistory.clear();
    impl_->clearStats();
    clearActivityCounters();
}

void DebugBackend::stepInstruction()
{
    stepInstructionDetailed();
}

StepResult DebugBackend::stepInstructionDetailed()
{
    StepResult r;

    // Track last_pc before execution (Stage 3.6)
    lastPc_ = target_->getCpuState().pc;
    stopReason_ = StopReason::Step;

    // --- before ---
    r.before   = target_->getCpuState();
    r.before.last_pc = lastPc_;
    r.pcBefore = r.before.pc;

    // Read opcode via peek (no callbacks)
    r.opcode  = target_->peekMemory(r.pcBefore);
    r.length  = opcode_info::get_length(r.opcode);

    // Capture operand bytes BEFORE execution
    uint8_t operandBytes[2] = {0, 0};
    if (r.length >= 2) operandBytes[0] = target_->peekMemory((r.pcBefore + 1) & 0xffff);
    if (r.length >= 3) operandBytes[1] = target_->peekMemory((r.pcBefore + 2) & 0xffff);

    // Set fetch window
    fetchBasePc_    = r.pcBefore;
    fetchRemaining_ = r.length;

    // --- execute exactly one 8080 instruction ---
    // steppingInProgress_ guards the instruction-start detection inside
    // onMemoryRead() so the fetch counters owned by this function are not
    // double-counted when the callback fires for each operand byte.
    steppingInProgress_ = true;
    target_->stepInstruction();
    steppingInProgress_ = false;
    int cycles = target_->getCpuState().cycles;

    // Fetch window should be consumed; reset defensively.
    fetchRemaining_ = 0;

    // --- after ---
    r.after   = target_->getCpuState();
    r.after.last_pc = lastPc_;
    r.pcAfter = r.after.pc;
    r.cycles  = static_cast<uint32_t>(cycles > 0 ? cycles : 0);

    if (!instrumentationEnabled_) {
        return r;
    }

    // --- record InstructionEvent ---
    InstructionEvent ie;
    ie.sequence  = instructionSequence_;
    ie.pcBefore  = r.pcBefore;
    ie.pcAfter   = r.pcAfter;
    ie.opcode    = r.opcode;
    ie.length    = r.length;
    ie.cycles    = cycles;
    ie.operandBytes[0] = operandBytes[0];
    ie.operandBytes[1] = operandBytes[1];
    ie.before    = r.before;
    ie.after     = r.after;

    impl_->instrHistory.push(ie);

    // Stage 4.2: increment per-address execute counter
    executeCount_[r.pcBefore]++;

    // --- trace output ---
    if (onTrace) {
        char line[128];
        formatTraceLine(line, sizeof(line), r.pcBefore, r.opcode, operandBytes);
        onTrace(line);
    }

    // --- event callback ---
    if (onInstruction) {
        onInstruction(ie);
    }

    instructionSequence_++;
    // Stage 6.22 §2: from now on the free-run detection in onMemoryRead()
    // continues the numbering.
    freeRunSequenceStarted_ = true;

    return r;
}

void DebugBackend::requestSkipInstruction()
{
    if (!target_) return;

    // Calculate the address of the next instruction
    uint16_t pc = target_->getCpuState().pc;
    uint8_t opcode = target_->peekMemory(pc);
    uint8_t len = opcode_info::get_length(opcode);
    uint16_t nextAddr = static_cast<uint16_t>(pc + len);

    // Use a temporary breakpoint at the next instruction address.
    // This is thread-safe (goes through the command queue) and works
    // at instruction granularity — no risk of missing the target.
    skipTempBreakpoint_ = nextAddr;
    skipTempBreakpointAdded_ = false;

    // Only add if no breakpoint already exists there
    if (!hasBreakpoint(nextAddr)) {
        requestAddBreakpoint(nextAddr);
        skipTempBreakpointAdded_ = true;
    }

    // Run emulation — will stop at the breakpoint
    requestRun();

    // Remove the temporary breakpoint after run stops
    if (skipTempBreakpointAdded_) {
        requestRemoveBreakpoint(nextAddr);
        skipTempBreakpointAdded_ = false;
    }
    skipTempBreakpoint_ = 0;

    stopReason_ = StopReason::Skip;
}

void DebugBackend::run()
{
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = DebuggerState::Running;
    }
    pauseRequestedAtomic_.store(false, std::memory_order_release);

    skipBreakpoint_ = checkBreakpoint();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (state_ != DebuggerState::Running) break;
        }

        if (checkBreakpoint()) {
            if (skipBreakpoint_) {
                skipBreakpoint_ = false;
                stepInstruction();
                continue;
            }
            stopReason_ = StopReason::Breakpoint;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                state_ = DebuggerState::Paused;
            }
            break;
        }

        if (pauseRequestedAtomic_.load(std::memory_order_acquire)) {
            stopReason_ = StopReason::UserPause;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                state_ = DebuggerState::Paused;
            }
            pauseRequestedAtomic_.store(false, std::memory_order_release);
            break;
        }

        stepInstruction();
    }

    // Stage 5.3.3.3: Fulfill any pending Run pause promises after pausing
    fulfillPausePromises_();
}

void DebugBackend::pause()
{
    // Stage 5.3.3.1: Use atomic pause signal
    pauseRequestedAtomic_.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Breakpoints (Stage 3.7)
// ---------------------------------------------------------------------------

std::map<int, DebuggerBreakpoint>::iterator
DebugBackend::findBreakpointByAddress(uint16_t address)
{
    for (auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
        if (it->second.address == address) return it;
    }
    return breakpoints_.end();
}

std::map<int, DebuggerBreakpoint>::const_iterator
DebugBackend::findBreakpointByAddress(uint16_t address) const
{
    for (auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
        if (it->second.address == address) return it;
    }
    return breakpoints_.end();
}

int DebugBackend::addBreakpoint(uint16_t address)
{
    if (findBreakpointByAddress(address) != breakpoints_.end()) {
        return -1;
    }
    int id = nextId_++;
    breakpoints_[id] = { address, true };
    return id;
}

bool DebugBackend::removeBreakpoint(uint16_t address)
{
    auto it = findBreakpointByAddress(address);
    if (it == breakpoints_.end()) return false;
    breakpoints_.erase(it);
    return true;
}

void DebugBackend::removeBreakpoint(int id)
{
    breakpoints_.erase(id);
}

bool DebugBackend::setBreakpointEnabled(uint16_t address, bool enabled)
{
    auto it = findBreakpointByAddress(address);
    if (it == breakpoints_.end()) return false;
    it->second.enabled = enabled;
    return true;
}

bool DebugBackend::hasBreakpoint(uint16_t address) const
{
    auto it = findBreakpointByAddress(address);
    return it != breakpoints_.end() && it->second.enabled;
}

std::vector<DebuggerBreakpoint> DebugBackend::getBreakpoints() const
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    std::vector<DebuggerBreakpoint> result;
    result.reserve(breakpoints_.size());
    for (auto &kv : breakpoints_) {
        result.push_back(kv.second);
    }
    return result;
}

void DebugBackend::clearBreakpoints()
{
    breakpoints_.clear();
}

StopReason DebugBackend::getStopReason() const
{
    return stopReason_;
}

bool DebugBackend::checkBreakpoint()
{
    uint16_t pc = target_->getCpuState().pc;
    for (auto &kv : breakpoints_) {
        if (kv.second.enabled && kv.second.address == pc) {
            return true;
        }
    }
    return false;
}

void DebugBackend::syncBreakpointsToTarget()
{
    if (!target_) return;

    std::vector<DebuggerBreakpoint> bps;
    bps.reserve(breakpoints_.size());
    for (auto &kv : breakpoints_) {
        if (kv.second.enabled) {
            bps.push_back(kv.second);
        }
    }
    target_->syncBreakpoints(bps.data(), bps.size());
}

// ---------------------------------------------------------------------------
// Instrumentation hooks
// ---------------------------------------------------------------------------

void DebugBackend::onMemoryRead(uint32_t virt, uint32_t phys,
                                 bool stack, uint8_t value, uint16_t pc)
{
    if (!instrumentationEnabled_) return;

    // Stage 6.24: instruction-start detection from the memory read stream.
    //
    // In free-run mode (steppingInProgress_ == false), after all operand bytes
    // of the previous instruction have been consumed (fetchRemaining_ == 0),
    // a non-stack read from address `virt` where (uint16_t)(virt+1) == pc (the
    // PC value AFTER the RD_BYTE(PC++) increment was observed via i8080_pc())
    // is the opcode fetch of the NEXT instruction.
    //
    // `value` is the opcode byte at `virt`, so we can use it directly to
    // determine instruction length without a separate memory read.
    //
    // Known limitation: a data read (e.g. ADD M with HL pointing into the
    // instruction's own bytes) may satisfy this relation spuriously while
    // fetchRemaining_ == 0.  This is pathological self-referencing code; the
    // regression test suite documents the actual behaviour explicitly.
    if (!steppingInProgress_ && fetchRemaining_ == 0 && !stack &&
        static_cast<uint16_t>(virt + 1) == pc) {
        uint16_t instrAddr = static_cast<uint16_t>(virt & 0xffff);
        // Match the step path numbering: the first free-run instruction keeps
        // the current sequence value; subsequent ones advance it.
        if (freeRunSequenceStarted_) instructionSequence_++;
        freeRunSequenceStarted_ = true;
        executeCount_[instrAddr]++;
        fetchBasePc_    = instrAddr;
        fetchRemaining_ = opcode_info::get_length(value);
    }

    MemoryAccessEvent ev;
    ev.instructionSequence = instructionSequence_;

    bool isFetch = fetchRemaining_ > 0;
    if (isFetch) {
        ev.type = MemoryAccessType::Fetch;
        fetchRemaining_--;
    } else {
        ev.type = MemoryAccessType::Read;
    }

    ev.virt   = static_cast<uint16_t>(virt & 0xffff);
    ev.phys   = phys;
    ev.value  = value;
    ev.stack  = stack;

    impl_->memHistory.push(ev);

    uint16_t addr = ev.virt;
    impl_->memStats[addr].reads++;
    impl_->memStats[addr].lastReadSequence = instructionSequence_;
    impl_->memStats[addr].lastReadTime = MemoryStats::Clock::now();

    // Stage 6.20: Runtime Memory Access Map accumulation
    {
        int block = addr >> 8;
        if (isFetch) {
            runtimeAccessMap_[block].fetch = true;
            runtimeAccessMap_[block].fetch_count++;
        } else {
            runtimeAccessMap_[block].read = true;
            runtimeAccessMap_[block].read_count++;
        }

        // Stage 6.20: Runtime Access Log entry
        RuntimeAccessLogEntry logEntry;
        logEntry.address = addr;
        logEntry.type = isFetch ? RuntimeAccessLogEntry::Fetch
                                : RuntimeAccessLogEntry::Read;
        // Stage 6.22 §1: pc of the ACCESSING INSTRUCTION, not the CPU's
        // in-flight PC - otherwise a fetch of pc+1 is logged against a
        // different pc and the reader cannot tell fetch from data.
        logEntry.pc = fetchBasePc_;
        logEntry.value = value;
        // Stage 6.25: monotonic sequence, assigned at push time.
        logEntry.sequence = accessSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        runtimeAccessLog_->push(logEntry);
    }
}

void DebugBackend::onMemoryWrite(uint32_t virt, uint32_t phys,
                                  bool stack, uint8_t value)
{
    if (!instrumentationEnabled_) return;

    MemoryAccessEvent ev;
    ev.instructionSequence = instructionSequence_;
    ev.type   = MemoryAccessType::Write;
    ev.virt   = static_cast<uint16_t>(virt & 0xffff);
    ev.phys   = phys;
    ev.value  = value;
    ev.stack  = stack;

    impl_->memHistory.push(ev);

    uint16_t addr = ev.virt;
    impl_->memStats[addr].writes++;
    impl_->memStats[addr].lastWriteSequence = instructionSequence_;
    impl_->memStats[addr].lastWriteTime = MemoryStats::Clock::now();

    // Enhanced Vector Screen: track VRAM writes (0xC000..0xC0FF)
    if (addr >= 0xC000 && addr < 0xC100) {
        std::lock_guard<std::mutex> lock(vramWriteMutex_);
        int idx = addr - 0xC000;
        vramLastWrite_[idx].value    = value;
        vramLastWrite_[idx].pc       = target_->getCpuState().pc;
        vramLastWrite_[idx].sequence = instructionSequence_;
    }

    // Stage 6.20: Runtime Memory Access Map accumulation
    {
        int block = addr >> 8;
        runtimeAccessMap_[block].write = true;
        runtimeAccessMap_[block].write_count++;

        // Stage 6.20: Runtime Access Log entry
        RuntimeAccessLogEntry logEntry;
        logEntry.address = addr;
        logEntry.type = RuntimeAccessLogEntry::Write;
        logEntry.pc = fetchBasePc_;   // Stage 6.22 §1 — same rule as reads
        logEntry.value = value;
        // Stage 6.25: monotonic sequence, assigned at push time.
        logEntry.sequence = accessSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        runtimeAccessLog_->push(logEntry);
    }
}

void DebugBackend::onIoInput(uint8_t port, uint8_t value)
{
    if (!instrumentationEnabled_) return;

    IoAccessEvent ev;
    ev.instructionSequence = instructionSequence_;
    ev.type  = IoAccessType::In;
    ev.port  = port;
    ev.value = value;

    impl_->ioHistory.push(ev);
}

void DebugBackend::onIoOutput(uint8_t port, uint8_t value)
{
    if (!instrumentationEnabled_) return;

    IoAccessEvent ev;
    ev.instructionSequence = instructionSequence_;
    ev.type  = IoAccessType::Out;
    ev.port  = port;
    ev.value = value;

    impl_->ioHistory.push(ev);

    if (port == 0x03) {
        std::lock_guard<std::mutex> lock(ioRegMutex_);
        ioPA_ = value;
    } else if (port == 0x02) {
        std::lock_guard<std::mutex> lock(ioRegMutex_);
        ioPB_ = value;
    }
}

// ---------------------------------------------------------------------------
// Instrumentation enable/disable
// ---------------------------------------------------------------------------

void DebugBackend::setInstrumentationEnabled(bool enabled)
{
    instrumentationEnabled_ = enabled;
}

bool DebugBackend::isInstrumentationEnabled() const
{
    return instrumentationEnabled_;
}

// ---------------------------------------------------------------------------
// History access (Stage 2.1 — thread-safe snapshots)
// ---------------------------------------------------------------------------

uint64_t DebugBackend::instructionSequence() const
{
    return instructionSequence_;
}

size_t DebugBackend::instructionHistorySize() const
{
    return impl_->instrHistory.size();
}

std::vector<InstructionEvent> DebugBackend::instructionHistorySnapshot() const
{
    return impl_->instrHistory.snapshot();
}

size_t DebugBackend::memoryHistorySize() const
{
    return impl_->memHistory.size();
}

std::vector<MemoryAccessEvent> DebugBackend::memoryHistorySnapshot() const
{
    return impl_->memHistory.snapshot();
}

size_t DebugBackend::ioHistorySize() const
{
    return impl_->ioHistory.size();
}

std::vector<IoAccessEvent> DebugBackend::ioHistorySnapshot() const
{
    return impl_->ioHistory.snapshot();
}

void DebugBackend::clearHistory()
{
    impl_->instrHistory.clear();
    impl_->memHistory.clear();
    impl_->ioHistory.clear();
}

void DebugBackend::clearIoHistory()
{
    impl_->ioHistory.clear();
}

// ---------------------------------------------------------------------------
// Memory statistics (Stage 2.1 — thread-safe snapshot)
// ---------------------------------------------------------------------------

std::vector<MemoryStats> DebugBackend::memoryStatsSnapshot() const
{
    return std::vector<MemoryStats>(
        impl_->memStats, impl_->memStats + 65536);
}

// ---------------------------------------------------------------------------
// Stage 5.3.2: Symbol commands (through Command Queue)
// ---------------------------------------------------------------------------

CommandResult DebugBackend::requestCreateFunction(uint16_t addr, const std::string &name)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::CreateFunction;
    cmd->address = addr;
    cmd->name = name;
    return submitAndWait(std::move(cmd));
}

CommandResult DebugBackend::requestRenameSymbol(uint16_t addr, const std::string &name)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::RenameSymbol;
    cmd->address = addr;
    cmd->name = name;
    return submitAndWait(std::move(cmd));
}

CommandResult DebugBackend::requestSetComment(uint16_t addr, const std::string &comment)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::SetComment;
    cmd->address = addr;
    cmd->comment = comment;
    return submitAndWait(std::move(cmd));
}

CommandResult DebugBackend::requestRemoveSymbol(uint16_t addr)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::RemoveSymbol;
    cmd->address = addr;
    return submitAndWait(std::move(cmd));
}

CommandResult DebugBackend::requestAddLabel(uint16_t addr, const std::string &name)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::AddLabel;
    cmd->address = addr;
    cmd->name = name;
    return submitAndWait(std::move(cmd));
}

// ---------------------------------------------------------------------------
// Stage 5.3.2: Breakpoint commands (through Command Queue)
// ---------------------------------------------------------------------------

CommandResult DebugBackend::requestAddBreakpoint(uint16_t addr)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::AddBreakpoint;
    cmd->address = addr;
    return submitAndWait(std::move(cmd));
}

CommandResult DebugBackend::requestRemoveBreakpoint(uint16_t addr)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::RemoveBreakpoint;
    cmd->address = addr;
    return submitAndWait(std::move(cmd));
}

CommandResult DebugBackend::requestSetBreakpointEnabled(uint16_t addr, bool enabled)
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::SetBreakpointEnabled;
    cmd->address = addr;
    cmd->enabled = enabled;
    return submitAndWait(std::move(cmd));
}

CommandResult DebugBackend::requestClearBreakpoints()
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::ClearBreakpoints;
    return submitAndWait(std::move(cmd));
}

// ---------------------------------------------------------------------------
// Stage 5.3.2: Command Queue implementation
// ---------------------------------------------------------------------------

// -- CommandQueue methods --

void DebugBackend::CommandQueue::enqueue(std::unique_ptr<Command> cmd)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(cmd));
    }
    cv_.notify_one();
}

std::unique_ptr<DebugBackend::Command> DebugBackend::CommandQueue::tryDequeue()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return nullptr;
    auto cmd = std::move(queue_.front());
    queue_.pop();
    return cmd;
}

std::unique_ptr<DebugBackend::Command> DebugBackend::CommandQueue::waitAndDequeue()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]{ return !queue_.empty(); });
    auto cmd = std::move(queue_.front());
    queue_.pop();
    return cmd;
}

bool DebugBackend::CommandQueue::empty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

// -- submitAndWait: dual-mode command submission --

CommandResult DebugBackend::submitAndWait(std::unique_ptr<Command> cmd)
{
    // Get future before moving cmd — promise.get_future() can only be called once
    auto future = cmd->promise.get_future();

    // Stage 5.3.3.1: Test-only synchronous fallback.
    // Production code must NEVER use this path — all production goes through the queue.
    if (testSynchronous_ && !emulationLoopRunning_) {
        // Check cancellation before execution (matches production path in processCommand)
        if (cmd->cancelled.load(std::memory_order_acquire)) {
            CommandResult r;
            r.success = false;
            r.error = "cancelled";
            r.status = CommandResult::Cancelled;
            cmd->promise.set_value(r);
            return future.get();
        }
        cmd->state.store(CommandState::Executing, std::memory_order_release);
        executeCommand(*cmd);
        return future.get();
    }

    // Stage 5.3.3.1: Production path — always enqueue.
    Command *rawCmd = cmd.get();
    commandQueue_.enqueue(std::move(cmd));

    auto status = future.wait_for(std::chrono::seconds(5));
    if (status == std::future_status::timeout) {
        // Stage 5.3.3.2: Check command state before deciding on timeout.
        auto cmdState = rawCmd->state.load(std::memory_order_acquire);
        if (cmdState == CommandState::Executing) {
            // Command is already executing — must wait for completion.
            // Do NOT return Timeout for an executing state-changing command.
            return future.get();
        }
        // Stage 5.3.3.2: CAS Queued→Cancelled — mutually exclusive with Queued→Executing
        CommandState expected = CommandState::Queued;
        if (rawCmd->state.compare_exchange_strong(expected, CommandState::Cancelled,
                                                  std::memory_order_acq_rel)) {
            // Successfully cancelled — command won't execute
            CommandResult r;
            r.success = false;
            r.error = "command timed out";
            r.status = CommandResult::Timeout;
            return r;
        }
        // CAS failed — emulation thread already took the command. Wait.
        return future.get();
    }
    return future.get();
}

// -- executeCommand: runs on Emulation Thread --

void DebugBackend::executeCommand(Command &cmd)
{
    CommandResult result;
    result.success = true;
    result.status = CommandResult::Completed;

    // Stage 5.3.3: Check cancellation before state-changing operations
    if (cmd.cancelled.load(std::memory_order_acquire)) {
        result.success = false;
        result.error = "cancelled";
        result.status = CommandResult::Cancelled;
        cmd.promise.set_value(result);
        return;
    }

    switch (cmd.type) {
    case CommandType::AddBreakpoint: {
        int id = addBreakpoint(cmd.address);
        if (id >= 0) {
            result.success = true;
        } else {
            result.success = false;
            result.error = "breakpoint already exists at this address";
            result.status = CommandResult::Failed;
        }
        syncBreakpointsToTarget();
        break;
    }
    case CommandType::RemoveBreakpoint: {
        bool ok = removeBreakpoint(cmd.address);
        result.success = ok;
        if (!ok) {
            result.error = "no breakpoint at this address";
            result.status = CommandResult::Failed;
        }
        syncBreakpointsToTarget();
        break;
    }
    case CommandType::SetBreakpointEnabled: {
        bool ok = setBreakpointEnabled(cmd.address, cmd.enabled);
        result.success = ok;
        if (!ok) {
            result.error = "no breakpoint at this address";
            result.status = CommandResult::Failed;
        }
        syncBreakpointsToTarget();
        break;
    }
    case CommandType::ClearBreakpoints: {
        clearBreakpoints();
        syncBreakpointsToTarget();
        break;
    }
    case CommandType::CreateFunction: {
        bool ok = symbols_.addSymbol(cmd.address, cmd.name, SymbolType::Function);
        result.success = ok;
        if (!ok) {
            result.error = "symbol already exists at this address";
            result.status = CommandResult::Failed;
        } else {
            // Stage 6.15 Iter2: Also create RDB object so the function
            // appears in RDB queries (get_rdb_info, list_rdb_objects, save).
            RdbObject rdbObj;
            rdbObj.address = cmd.address;
            rdbObj.type    = RdbObjectType::Function;
            rdbObj.name    = cmd.name;
            rdbObj.hasSize = false;
            rdb_->addObject(rdbObj);  // ignore duplicate — symbol was created
        }
        break;
    }
    case CommandType::RenameSymbol: {
        bool ok = symbols_.renameSymbol(cmd.address, cmd.name);
        result.success = ok;
        if (!ok) {
            result.error = "symbol not found";
            result.status = CommandResult::Failed;
        } else {
            // Stage 6.20.2: Also update RDB object name so debug_save_rdb
            // persists the rename. Mirrors CreateFunction dual-update pattern.
            const RdbObject *existing = rdb_->getObject(cmd.address);
            if (existing) {
                RdbObject updated = *existing;
                updated.name = cmd.name;
                rdb_->updateObject(updated);
            }
        }
        break;
    }
    case CommandType::SetComment: {
        bool ok = symbols_.setComment(cmd.address, cmd.comment);
        result.success = ok;
        if (!ok) {
            result.error = "symbol not found";
            result.status = CommandResult::Failed;
        } else {
            // Stage 6.20.2: Also update RDB object comment so debug_save_rdb
            // persists the comment. Mirrors CreateFunction dual-update pattern.
            rdb_->setComment(cmd.address, cmd.comment);
        }
        break;
    }
    case CommandType::RemoveSymbol: {
        bool ok = symbols_.removeSymbol(cmd.address);
        result.success = ok;
        if (!ok) {
            result.error = "symbol not found";
            result.status = CommandResult::Failed;
        }
        break;
    }
    case CommandType::AddLabel: {
        bool ok = symbols_.addSymbol(cmd.address, cmd.name, SymbolType::Label);
        result.success = ok;
        if (!ok) {
            result.error = "symbol already exists at this address";
            result.status = CommandResult::Failed;
        } else {
            // Stage 6.15 Iter2: Also create RDB object for the label.
            RdbObject rdbObj;
            rdbObj.address = cmd.address;
            rdbObj.type    = RdbObjectType::Label;
            rdbObj.name    = cmd.name;
            rdbObj.hasSize = false;
            rdb_->addObject(rdbObj);  // ignore duplicate — symbol was created
        }
        break;
    }
    case CommandType::Step: {
        stepInstruction();
        break;
    }
    case CommandType::Reset: {
        running_.store(false, std::memory_order_release);
        reset();
        break;
    }
    case CommandType::Restart: {
        running_.store(false, std::memory_order_release);
        restart();
        break;
    }
    case CommandType::LoadRom: {
        // Runs on the emulation thread (see DebugBackend::loadRom). Stop the
        // loop first so no frame executes after the machine is replaced —
        // otherwise the new ROM would already have run a few frames by the
        // time the caller gets its result back and sets state_ to Paused.
        running_.store(false, std::memory_order_release);
        bool ok = target_->loadRom(cmd.name, cmd.org);
        result.success = ok;
        if (!ok) {
            result.error = "failed to load ROM: " + cmd.name;
            result.status = CommandResult::Failed;
        }
        break;
    }
    case CommandType::MemoryWrite: {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (state_ == DebuggerState::Paused) {
            for (size_t i = 0; i < cmd.writeData.size(); ++i) {
                uint16_t addr = static_cast<uint16_t>((cmd.address + i) & 0xFFFF);
                target_->writeMemory(addr, cmd.writeData[i]);
            }
            result.success = true;
        } else {
            result.success = false;
            result.error = "cannot write memory while running";
            result.status = CommandResult::Failed;
        }
        break;
    }
    case CommandType::RegisterWrite: {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (state_ == DebuggerState::Paused) {
            target_->writeCpuRegister(static_cast<int>(cmd.regId), cmd.regValue);
            result.success = true;
        } else {
            result.success = false;
            result.error = "cannot write register while running";
            result.status = CommandResult::Failed;
        }
        break;
    }
    case CommandType::ExecuteTrace: {
        if (traceBusy_.exchange(true)) {
            result.success = false;
            result.error = "trace busy";
            result.status = CommandResult::Failed;
            if (cmd.tracePromise) cmd.tracePromise->set_value(TraceExecutionResult{});
        } else {
            auto traceResult = executeTraceInternal(cmd.traceParams);
            result.success = true;
            traceBusy_.store(false);
            if (cmd.tracePromise) cmd.tracePromise->set_value(std::move(traceResult));
        }
        break;
    }
    case CommandType::Run:
        // Stage 5.3.3.3: Save pausePromise to pending list.
        // It will be fulfilled when the emulation actually transitions to Paused
        // (by run(), executeFramesTarget_(), or processCommand() post-check).
        if (cmd.pausePromise) {
            std::lock_guard<std::mutex> lk(pendingPauseMutex_);
            pendingPausePromises_.push_back(cmd.pausePromise);
        }
        // Stage 5.3.3.2: Use stateMutex_ for state_ protection.
        running_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = DebuggerState::Running;
            pauseRequestedAtomic_.store(false, std::memory_order_release);
        }
        skipBreakpoint_ = checkBreakpoint();
        break;
    case CommandType::Pause:
    case CommandType::Quit:
        // These are handled by the emulation loop directly, not through executeCommand.
        result.success = true;
        break;
    case CommandType::IoWrite: {
        // Stage 6.1 Iteration 3: Write to I/O port through Command Queue.
        // Must be executed on emulation thread.
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (state_ == DebuggerState::Paused) {
            target_->writeIoPort(static_cast<uint8_t>(cmd.address), cmd.ioValue);
            result.success = true;
        } else {
            result.success = false;
            result.error = "cannot write I/O port while running";
            result.status = CommandResult::Failed;
        }
        break;
    }
    default:
        result.success = false;
        result.error = "unknown command type";
        result.status = CommandResult::Failed;
        break;
    }

    cmd.promise.set_value(result);
}

// ---------------------------------------------------------------------------
// Stage 5.3.2: Trace execution (through Command Queue)
// ---------------------------------------------------------------------------

std::future<DebugBackend::TraceExecutionResult>
DebugBackend::requestExecuteTrace(const TraceExecutionParams &params)
{
    // Stage 5.3.3.1: Always through Command Queue.
    // No direct-execution fallback in production.
    auto tracePromise = std::make_shared<std::promise<TraceExecutionResult>>();
    auto traceResultFuture = tracePromise->get_future();

    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::ExecuteTrace;
    cmd->traceParams = params;
    cmd->tracePromise = tracePromise;

    if (testSynchronous_ && !emulationLoopRunning_) {
        // Test-only: execute synchronously
        cmd->state.store(CommandState::Executing, std::memory_order_release);
        executeCommand(*cmd);
    } else {
        // Production: enqueue and wait for command completion
        auto cmdFuture = cmd->promise.get_future();
        commandQueue_.enqueue(std::move(cmd));
        cmdFuture.wait();  // Wait until trace result is delivered
    }

    return traceResultFuture;
}

// ---------------------------------------------------------------------------
// executeTraceInternal — runs exclusively on Emulation Thread
// ---------------------------------------------------------------------------

DebugBackend::TraceExecutionResult
DebugBackend::executeTraceInternal(const TraceExecutionParams &params)
{
    TraceExecutionResult result;
    result.startSequence = instructionSequence_;
    result.entrySp = target_->getCpuState().sp;
    result.minSp = result.entrySp;
    result.maxSp = result.entrySp;
    result.exitReason = ExitReason::Unknown;

    for (uint32_t i = 0; i < params.maxInstructions; ++i) {
        // Stage 5.3.3: Check for break/quit requests during trace
        if (breakRequested_.load(std::memory_order_acquire) ||
            quitRequested_.load(std::memory_order_acquire)) {
            if (result.exitReason == ExitReason::Unknown) {
                result.exitReason = ExitReason::Timeout;
                result.exitPc = target_->getCpuState().pc;
            }
            break;
        }

        uint16_t pc = target_->getCpuState().pc;
        uint8_t opcode = target_->peekMemory(pc);

        // Execute one instruction
        stepInstruction();

        // Track SP bounds
        uint16_t newSp = target_->getCpuState().sp;
        if (newSp < result.minSp) result.minSp = newSp;
        if (newSp > result.maxSp) result.maxSp = newSp;

        // Check exit conditions
        if (params.stopOnRet && opcode == 0xC9) {
            result.exitReason = ExitReason::Ret;
            result.exitPc = pc;
            break;
        }

        if (opcode == 0x76) {
            result.exitReason = ExitReason::Halt;
            result.exitPc = pc;
            break;
        }

        if (params.stopOnCallerReturn && params.callerReturnAddress != 0
            && target_->getCpuState().pc == params.callerReturnAddress) {
            result.exitReason = ExitReason::CallerReturn;
            result.exitPc = pc;
            break;
        }
    }

    // If loop finished without exit
    if (result.exitReason == ExitReason::Unknown) {
        result.exitReason = ExitReason::Timeout;
        result.exitPc = target_->getCpuState().pc;
    }

    result.endSequence = instructionSequence_;
    result.instructionsExecuted = static_cast<uint32_t>(result.endSequence - result.startSequence);
    result.exitSp = target_->getCpuState().sp;

    return result;
}

// ---------------------------------------------------------------------------
// Stage 4.2: Screen snapshot
// ---------------------------------------------------------------------------

DebugBackend::ScreenSnapshot DebugBackend::screenSnapshot() const
{
    ScreenSnapshot snap;

    std::lock_guard<std::mutex> lock(screenMutex_);

    ScreenData data = target_->screenSnapshot();
    snap.pixels = std::move(data.pixels);
    snap.width  = data.width;
    snap.height = data.height;

    return snap;
}

// ---------------------------------------------------------------------------
// Stage 6.27: Beam / raster state facade
// ---------------------------------------------------------------------------

BeamState DebugBackend::beamState() const
{
    // Read-only: never pauses/steps/re-renders. The adapter reads state the
    // video path has already computed; we only stamp whether the emulator was
    // running at snapshot time (the backend owns the state machine).
    BeamState s = target_->getBeamState();
    s.running = (getState() == DebuggerState::Running);
    return s;
}

// ---------------------------------------------------------------------------
// Stage 6.27 P2: raster events facade (OUT-with-beam-position ring)
// ---------------------------------------------------------------------------

std::vector<RasterEvent> DebugBackend::rasterEvents(
    uint64_t frame, uint32_t vCycleStart, uint32_t vCycleEnd,
    int port, uint16_t pc, size_t maxResults) const
{
    return target_->getRasterEvents(frame, vCycleStart, vCycleEnd, port, pc, maxResults);
}

void DebugBackend::clearRasterEvents()
{
    target_->clearRasterEvents();
}

// ---------------------------------------------------------------------------
// Palette snapshot
// ---------------------------------------------------------------------------

PaletteSnapshot DebugBackend::paletteSnapshot() const
{
    return target_->paletteSnapshot();
}

// ---------------------------------------------------------------------------
// Sound snapshot
// ---------------------------------------------------------------------------

SoundSnapshot DebugBackend::soundSnapshot() const
{
    return target_->soundSnapshot();
}

void DebugBackend::setMuted(bool muted)
{
    target_->setMuted(muted);
}

// ---------------------------------------------------------------------------
// Audio grab (GRAB AUDIO / SAVE MID) — forwarded to the target
// ---------------------------------------------------------------------------

void DebugBackend::setAudioGrabEnabled(bool enabled)
{
    target_->setAudioGrabEnabled(enabled);
}

bool DebugBackend::isAudioGrabEnabled() const
{
    return target_->isAudioGrabEnabled();
}

size_t DebugBackend::audioGrabEventCount() const
{
    return target_->audioGrabEventCount();
}

std::vector<AudioPortEvent> DebugBackend::audioGrabEvents() const
{
    return target_->audioGrabEvents();
}

bool DebugBackend::audioGrabOverflowed() const
{
    return target_->audioGrabOverflowed();
}

// ---------------------------------------------------------------------------
// Stage 4.2: Activity snapshot
// ---------------------------------------------------------------------------

DebugBackend::ActivitySnapshot DebugBackend::activitySnapshot() const
{
    ActivitySnapshot snap;
    snap.executeCount.assign(executeCount_, executeCount_ + 65536);
    snap.readCount.resize(65536);
    snap.writeCount.resize(65536);
    for (int i = 0; i < 65536; ++i) {
        snap.readCount[i]  = impl_->memStats[i].reads;
        snap.writeCount[i] = impl_->memStats[i].writes;
    }
    return snap;
}

void DebugBackend::clearActivityCounters()
{
    std::memset(executeCount_, 0, sizeof(executeCount_));

    {
        std::lock_guard<std::mutex> lock(vramWriteMutex_);
        for (int i = 0; i < 256; ++i) {
            vramLastWrite_[i] = VramWriteInfo();
        }
    }

    {
        std::lock_guard<std::mutex> lock(ioRegMutex_);
        ioPA_ = 0xFF;
        ioPB_ = 0xFF;
    }
}

// ---------------------------------------------------------------------------
// Stage 5.2: Live Activity snapshot (for Memory Map)
// ---------------------------------------------------------------------------

DebugBackend::LiveActivitySnapshot DebugBackend::liveActivitySnapshot() const
{
    LiveActivitySnapshot snap;

    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint minTime = TimePoint::min();

    for (int i = 0; i < 256; ++i) {
        snap.blocks[i].lastReadTime  = minTime;
        snap.blocks[i].lastWriteTime = minTime;
    }

    for (int addr = 0; addr < 65536; ++addr) {
        int block = addr >> 8;  // addr / 256

        TimePoint rt = impl_->memStats[addr].lastReadTime;
        TimePoint wt = impl_->memStats[addr].lastWriteTime;

        if (rt > snap.blocks[block].lastReadTime)
            snap.blocks[block].lastReadTime = rt;
        if (wt > snap.blocks[block].lastWriteTime)
            snap.blocks[block].lastWriteTime = wt;
    }

    return snap;
}

// ---------------------------------------------------------------------------
// Enhanced Vector Screen: Video mode snapshot
// ---------------------------------------------------------------------------

DebugBackend::VideoModeSnapshot DebugBackend::videoModeSnapshot() const
{
    VideoModeSnapshot snap;

    // Framebuffer dimensions
    snap.screenWidth  = 576;   // DEFAULT_SCREEN_WIDTH
    snap.screenHeight = 288;   // DEFAULT_SCREEN_HEIGHT

    // Read video mode from tracked IO port values
    {
        std::lock_guard<std::mutex> lock(ioRegMutex_);
        snap.mode512     = (ioPB_ & 0x10) != 0;
        snap.scrollValue  = ioPA_;
    }

    snap.visibleWidth  = snap.mode512 ? 512 : 256;
    snap.visibleHeight = 256;
    snap.pixelsPerByte = snap.mode512 ? 4 : 8;

    snap.borderLeft = (snap.screenWidth - snap.visibleWidth) / 2;
    snap.borderTop  = (snap.screenHeight - snap.visibleHeight) / 2;

    return snap;
}

// ---------------------------------------------------------------------------
// Enhanced Vector Screen: VRAM write snapshot
// ---------------------------------------------------------------------------

DebugBackend::VramWriteSnapshot DebugBackend::vramWriteSnapshot() const
{
    VramWriteSnapshot snap;
    std::lock_guard<std::mutex> lock(vramWriteMutex_);
    snap.lastWrite.assign(vramLastWrite_, vramLastWrite_ + 256);
    return snap;
}

// ---------------------------------------------------------------------------
// Debug trace formatting
// ---------------------------------------------------------------------------

void DebugBackend::formatTraceLine(char *buf, size_t bufsize,
                                   uint16_t pc, uint8_t opcode,
                                   const uint8_t *operandBytes) const
{
    uint8_t len = opcode_info::get_length(opcode);

    if (len == 1) {
        snprintf(buf, bufsize, "PC=%04X  %02X", pc, opcode);
    } else if (len == 2) {
        snprintf(buf, bufsize, "PC=%04X  %02X %02X", pc, opcode, operandBytes[0]);
    } else { // len == 3
        snprintf(buf, bufsize, "PC=%04X  %02X %02X%02X",
                 pc, opcode, operandBytes[1], operandBytes[0]);
    }
}

// ---------------------------------------------------------------------------
// Thread-safe command API (Stage 3.1 — for GUI)
// ---------------------------------------------------------------------------

void DebugBackend::requestStep()
{
    // Stage 5.3.3: Always go through command queue / executeCommand.
    // No direct stepInstruction() from calling thread.
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::Step;
    submitAndWait(std::move(cmd));
}

void DebugBackend::requestRun()
{
    // Stage 5.3.3.2: Always through Command Queue.
    // skipBreakpoint_ is computed by emulation thread in executeFramesTarget_().
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::Run;
    submitAndWait(std::move(cmd));
}

// Stage 5.3.3.3: requestRun() variant that returns a future fulfilled
// when the Emulation Thread next transitions to Paused (breakpoint/pause hit).
// The pause promise belongs to this specific Run command — no global promise.
std::future<CommandResult> DebugBackend::requestRunFuture()
{
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::Run;

    // Stage 5.3.3.2: Test-only synchronous fallback
    if (testSynchronous_ && !emulationLoopRunning_) {
        // No emulation thread — return a ready future immediately.
        // No pausePromise needed (no emulation to wait for).
        auto cmdFuture = cmd->promise.get_future();
        cmd->state.store(CommandState::Executing, std::memory_order_release);
        executeCommand(*cmd);
        return cmdFuture;
    }

    // Stage 5.3.3.3: Per-command pause promise (production only)
    auto pausePromise = std::make_shared<std::promise<CommandResult>>();
    auto pauseFuture = pausePromise->get_future();
    cmd->pausePromise = pausePromise;

    // Production: enqueue and wait for command processing
    auto cmdFuture = cmd->promise.get_future();
    commandQueue_.enqueue(std::move(cmd));
    cmdFuture.wait();

    // Return future that completes when emulation pauses
    return pauseFuture;
}

void DebugBackend::requestPause()
{
    // Stage 5.3.3.2: Pure atomic signal — no state mutation from caller thread.
    // Emulation Thread performs the actual Running→Paused transition.
    pauseRequestedAtomic_.store(true, std::memory_order_release);
    breakRequested_.store(true, std::memory_order_release);
}

void DebugBackend::requestReset()
{
    // Stage 5.3.3.2: No running_ mutation — only enqueue Reset command.
    // Emulation Thread performs the actual state transition.
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::Reset;
    submitAndWait(std::move(cmd));
}

void DebugBackend::requestRestart()
{
    // BLK+ВВОД: attach boot ROM, reset CPU, pause.
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::Restart;
    submitAndWait(std::move(cmd));
}

void DebugBackend::waitForCompletion()
{
    // Legacy method — kept for backward compatibility.
    // With Command Queue, callers use submitAndWait() futures instead.
}

void DebugBackend::requestQuit()
{
    // Stage 5.3.3.1: Always enqueue Quit command + set atomic signal.
    quitRequested_.store(true, std::memory_order_release);
    auto cmd = std::make_unique<Command>();
    cmd->type = CommandType::Quit;
    commandQueue_.enqueue(std::move(cmd));
}

bool DebugBackend::isQuitRequested() const
{
    return quitRequested_.load(std::memory_order_acquire);
}

// Stage 5.3.3.3: fulfillPausePromises_ — fulfill all pending Run pause promises.
// Called when the Emulation Thread transitions to Paused.
void DebugBackend::fulfillPausePromises_()
{
    std::vector<std::shared_ptr<std::promise<CommandResult>>> toFulfill;
    {
        std::lock_guard<std::mutex> lk(pendingPauseMutex_);
        toFulfill.swap(pendingPausePromises_);
    }
    CommandResult r;
    r.success = true;
    r.status = CommandResult::Completed;
    for (auto &p : toFulfill) {
        p->set_value(r);
    }
}

// Stage 5.3.3.3: Unified command processing — single CAS-based path.
// Called from both runUntilPause() (main loop) and executeFramesTarget_() (frame loop).
// Returns true if the frame loop should continue, false if it should break.
bool DebugBackend::processCommand(std::unique_ptr<Command> &cmd)
{
    // Stage 5.3.3.2: CAS Queued→Executing — mutually exclusive with Queued→Cancelled
    CommandState expected = CommandState::Queued;
    if (!cmd->state.compare_exchange_strong(expected, CommandState::Executing,
                                            std::memory_order_acq_rel)) {
        // CAS failed — command was cancelled (Queued→Cancelled)
        cmd->promise.set_value(CommandResult{false, "cancelled", CommandResult::Cancelled});
        return true;
    }

    // Every command must receive a result, even on quit.
    if (quitRequested_.load(std::memory_order_acquire) &&
        cmd->type != CommandType::Quit) {
        cmd->state.store(CommandState::Completed, std::memory_order_release);
        cmd->promise.set_value(CommandResult{false, "quit requested", CommandResult::Cancelled});
        return false;
    }

    // Check cancellation flag (redundant with CAS above, but kept for safety)
    if (cmd->cancelled.load(std::memory_order_acquire)) {
        cmd->state.store(CommandState::Completed, std::memory_order_release);
        cmd->promise.set_value(CommandResult{false, "cancelled", CommandResult::Cancelled});
        return true;
    }

    // Pause before trace execution — trace needs CPU in Paused state
    if (cmd->type == CommandType::ExecuteTrace) {
        running_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = DebuggerState::Paused;
            pauseRequestedAtomic_.store(false, std::memory_order_release);
        }
    }

    // Handle Quit specially: set flags and transition to Paused
    if (cmd->type == CommandType::Quit) {
        quitRequested_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            state_ = DebuggerState::Paused;
        }
        cmd->state.store(CommandState::Completed, std::memory_order_release);
        cmd->promise.set_value(CommandResult{true, "", CommandResult::Completed});
        fulfillPausePromises_();
        return false;
    }

    // Handle Pause specially: transition to Paused
    if (cmd->type == CommandType::Pause) {
        running_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            state_ = DebuggerState::Paused;
            stopReason_ = StopReason::UserPause;
            pauseRequestedAtomic_.store(false, std::memory_order_release);
        }
        cmd->state.store(CommandState::Completed, std::memory_order_release);
        cmd->promise.set_value(CommandResult{true, "", CommandResult::Completed});
        fulfillPausePromises_();
        return false;
    }

    executeCommand(*cmd);
    // Note: executeCommand() sets cmd->promise internally

    // Mark command as completed after execution
    cmd->state.store(CommandState::Completed, std::memory_order_release);

    // After any command, check if state is now Paused — fulfill pending promises
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (state_ == DebuggerState::Paused) {
            fulfillPausePromises_();
        }
    }

    return true;
}

// Note: processOneCommand() was replaced by processCommand() in Stage 5.3.3.3.
// The emulation main loop now uses processCommand() directly.

// ---------------------------------------------------------------------------
// executeFramesTarget_ — run frames until pause/breakpoint via IDebugTarget
// ---------------------------------------------------------------------------

void DebugBackend::executeFramesTarget_()
{
    syncBreakpointsToTarget();

    if (skipBreakpoint_ && checkBreakpoint()) {
        skipBreakpoint_ = false;
        stepInstruction();
    }

    // Sound follows the frame loop: the audio device is opened here and shut
    // again on every exit path, including the early quit return. Without this
    // the SDL callback keeps draining the sample ring after a Pause and then
    // degenerates into replaying half-written buffers (crackle). Single steps
    // stay outside the scope on purpose — they would otherwise emit one 20 ms
    // burst of sound each.
    struct AudioScope
    {
        IDebugTarget *t;
        explicit AudioScope(IDebugTarget *target) : t(target) {
            t->setAudioEmulationActive(true);
        }
        ~AudioScope() { t->setAudioEmulationActive(false); }
    } audioScope(target_);

    // Reset frame pacing timer — avoids a huge delta after pause/resume
    bool paceFrames = target_->framePacingEnabled();
    if (paceFrames) {
        lastFrameTime_ = Clock::now();
    }

    while (running_.load(std::memory_order_acquire)) {
        if (quitRequested_.load(std::memory_order_acquire)) {
            target_->debuggerBreak();
            target_->debuggerDetached();
            return;
        }

        target_->debuggerContinue();
        uint16_t pcBeforeFrame = target_->getCpuState().pc;
        target_->executeFrame();
        uint16_t pcAfterFrame = target_->getCpuState().pc;

        // -- Frame pacing: sleep to maintain 50 Hz (20 ms per frame) --------
        if (paceFrames) {
            auto now = Clock::now();
            auto elapsed = now - lastFrameTime_;
            if (elapsed < kFrameDuration) {
                std::this_thread::sleep_for(kFrameDuration - elapsed);
            }
            lastFrameTime_ = Clock::now();
        }

        // Stage 5.3.3.3: Process commands through unified path
        while (auto cmd = commandQueue_.tryDequeue()) {
            if (!processCommand(cmd)) break;
        }

        // If PC didn't change, target stopped (breakpoint or debugger_interrupt)
        if (pcAfterFrame == pcBeforeFrame && !quitRequested_.load(std::memory_order_acquire)) {
            if (checkBreakpoint()) {
                stopReason_ = StopReason::Breakpoint;
                running_.store(false, std::memory_order_release);
                break;
            }
        }

        if (pauseRequestedAtomic_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire)) {
            running_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(stateMutex_);
                stopReason_ = StopReason::UserPause;
                state_ = DebuggerState::Paused;
                pauseRequestedAtomic_.store(false, std::memory_order_release);
            }
            break;
        }

        if (checkBreakpoint()) {
            stopReason_ = StopReason::Breakpoint;
            running_.store(false, std::memory_order_release);
            break;
        }
    }

    // Stage 5.3.3.3: Frame loop ended — fulfill any pending Run pause promises
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (state_ != DebuggerState::Paused) {
            state_ = DebuggerState::Paused;
        }
    }
    fulfillPausePromises_();
}

// ---------------------------------------------------------------------------
// executeFramesNoTarget_ — fallback (should not normally be called)
// ---------------------------------------------------------------------------

void DebugBackend::executeFramesNoTarget_()
{
    while (running_.load(std::memory_order_acquire)) {
        if (quitRequested_.load(std::memory_order_acquire)) return;
        if (checkBreakpoint()) {
            if (skipBreakpoint_) {
                skipBreakpoint_ = false;
                stepInstruction();
                continue;
            }
            stopReason_ = StopReason::Breakpoint;
            running_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(stateMutex_);
                state_ = DebuggerState::Paused;
            }
            break;
        }
        if (pauseRequestedAtomic_.load(std::memory_order_acquire) || !running_.load(std::memory_order_acquire)) {
            running_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(stateMutex_);
                stopReason_ = StopReason::UserPause;
                state_ = DebuggerState::Paused;
                pauseRequestedAtomic_.store(false, std::memory_order_release);
            }
            break;
        }
        stepInstruction();
    }
}

void DebugBackend::runUntilPause()
{
    if (!target_) return;

    emulationLoopRunning_ = true;

    // Set up poll_debugger callback — checks flags set by other threads
    target_->setPollCallback([this]() {
        if (quitRequested_.load(std::memory_order_acquire)) {
            target_->debuggerBreak();
            return;
        }

        if (breakRequested_.load(std::memory_order_acquire)) {
            breakRequested_.store(false, std::memory_order_release);
            target_->debuggerBreak();
            return;
        }

        if (pauseRequestedAtomic_.load(std::memory_order_acquire) || !running_.load(std::memory_order_relaxed)) {
            pauseRequestedAtomic_.store(true, std::memory_order_release);
            target_->debuggerBreak();
            return;
        }
    });

    // Enable debugging mode
    target_->debuggerAttached();

    // Main emulation loop
    while (true) {
        // Stage 5.3.3.3: Check running_ BEFORE blocking on waitAndDequeue().
        // This handles test-mode where requestRun() executed synchronously
        // and already set running_=true before runUntilPause() was called.
        if (running_.load(std::memory_order_acquire)) {
            executeFramesTarget_();
            // executeFramesTarget_() already set Paused and
            // fulfilled pending pause promises before returning.
        }

        if (quitRequested_.load(std::memory_order_acquire)) {
            // Stage 5.3.3: Drain remaining commands — fulfill all promises
            while (auto pending = commandQueue_.tryDequeue()) {
                // Stage 5.3.3.2: CAS to Cancelled — only if still Queued
                CommandState expected = CommandState::Queued;
                if (pending->state.compare_exchange_strong(expected, CommandState::Cancelled,
                                                          std::memory_order_acq_rel)) {
                    // Stage 5.3.3.3: Also fulfill per-command pause promise if present
                    if (pending->pausePromise) {
                        CommandResult pr;
                        pr.success = false;
                        pr.error = "quit";
                        pr.status = CommandResult::Cancelled;
                        pending->pausePromise->set_value(pr);
                    }
                    pending->promise.set_value(
                        CommandResult{false, "quit", CommandResult::Cancelled});
                }
            }
            // Stage 5.3.3.3: processCommand(Quit) already set Paused and
            // fulfilled pending promises.  Ensure state is consistent.
            {
                std::lock_guard<std::mutex> lk(stateMutex_);
                state_ = DebuggerState::Paused;
            }
            fulfillPausePromises_();
            target_->debuggerDetached();
            emulationLoopRunning_ = false;
            return;
        }

        // Wait for a command from the queue (blocks until available)
        auto cmd = commandQueue_.waitAndDequeue();
        processCommand(cmd);
        // Loop back — if processCommand set running_ (Run command),
        // executeFramesTarget_() will be called at the top of the loop.
    }
}

// ---------------------------------------------------------------------------
// Runtime Memory Access Map (Stage 6.20)
// ---------------------------------------------------------------------------

void DebugBackend::clearRuntimeAccessMap()
{
    std::lock_guard<std::mutex> lock(runtimeAccessMutex_);
    for (int i = 0; i < 256; ++i) {
        runtimeAccessMap_[i].address = static_cast<uint16_t>(i * 256);
        runtimeAccessMap_[i].read   = false;
        runtimeAccessMap_[i].write  = false;
        runtimeAccessMap_[i].fetch  = false;
        runtimeAccessMap_[i].read_count  = 0;
        runtimeAccessMap_[i].write_count = 0;
        runtimeAccessMap_[i].fetch_count = 0;
    }
    runtimeAccessLog_->clear();
    // Stage 6.25: reset sequence so numbering restarts at 1 for a fresh
    // debug session (ROM load, explicit clear via MCP).
    accessSequence_.store(0, std::memory_order_relaxed);
}

// Stage 6.25: clear ONLY the detailed access log; leave the aggregated
// runtime map untouched so the Memory Map window keeps its history.
void DebugBackend::clearMemoryAccessLog()
{
    std::lock_guard<std::mutex> lock(runtimeAccessMutex_);
    runtimeAccessLog_->clear();
    // accessSequence_ is deliberately NOT reset: entries already rendered in
    // the Memory Access window keep their numbering until they scroll out.
}

std::vector<RuntimeAccessBlock> DebugBackend::getRuntimeAccessMap() const
{
    std::lock_guard<std::mutex> lock(runtimeAccessMutex_);
    return std::vector<RuntimeAccessBlock>(
        runtimeAccessMap_, runtimeAccessMap_ + 256);
}

std::vector<RuntimeAccessLogEntry> DebugBackend::getRuntimeAccessLog(size_t maxEntries) const
{
    auto all = runtimeAccessLog_->snapshot();
    if (maxEntries > 0 && all.size() > maxEntries) {
        // Return the most recent maxEntries entries (they are at the end)
        return std::vector<RuntimeAccessLogEntry>(
            all.end() - static_cast<long>(maxEntries), all.end());
    }
    return all;
}

// ---------------------------------------------------------------------------
// Memory Snapshots (Stage 6.20)
// ---------------------------------------------------------------------------

uint32_t DebugBackend::createMemorySnapshot(uint16_t start, size_t size)
{
    if (!target_) return 0;
    if (size == 0 || static_cast<uint32_t>(start) + size > 0x10000) return 0;

    MemorySnapshotData snap;
    snap.start_address = start;
    snap.data.resize(size);
    for (size_t i = 0; i < size; ++i) {
        snap.data[i] = target_->readMemoryRaw(static_cast<uint16_t>(start + i));
    }

    std::lock_guard<std::mutex> lock(snapshotMutex_);
    uint32_t id = nextSnapshotId_++;
    snap.snapshot_id = id;
    snapshots_[id] = std::move(snap);
    return id;
}

MemorySnapshotData DebugBackend::getMemorySnapshot(uint32_t id) const
{
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    auto it = snapshots_.find(id);
    if (it == snapshots_.end()) {
        return {};  // empty/invalid
    }
    return it->second;
}

MemorySnapshotDiff DebugBackend::compareMemorySnapshots(uint32_t idA, uint32_t idB) const
{
    MemorySnapshotDiff diff;

    std::lock_guard<std::mutex> lock(snapshotMutex_);
    auto itA = snapshots_.find(idA);
    auto itB = snapshots_.find(idB);
    if (itA == snapshots_.end() || itB == snapshots_.end()) {
        return diff;  // empty diff for invalid snapshots
    }

    const auto &a = itA->second;
    const auto &b = itB->second;

    // Both snapshots must cover the same range
    if (a.start_address != b.start_address || a.data.size() != b.data.size()) {
        return diff;
    }

    // Find changed bytes and merge into contiguous ranges
    size_t len = a.data.size();
    bool inRange = false;
    uint16_t rangeStart = 0;
    size_t rangeSize = 0;

    for (size_t i = 0; i < len; ++i) {
        if (a.data[i] != b.data[i]) {
            if (!inRange) {
                rangeStart = static_cast<uint16_t>(a.start_address + i);
                rangeSize = 1;
                inRange = true;
            } else {
                ++rangeSize;
            }
        } else {
            if (inRange) {
                diff.changed_ranges.push_back({rangeStart, rangeSize});
                inRange = false;
            }
        }
    }
    // Close any open range at the end
    if (inRange) {
        diff.changed_ranges.push_back({rangeStart, rangeSize});
    }

    return diff;
}

bool DebugBackend::deleteMemorySnapshot(uint32_t id)
{
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshots_.erase(id) > 0;
}

void DebugBackend::invalidateAllSnapshots()
{
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    snapshots_.clear();
}
