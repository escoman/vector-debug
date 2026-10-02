#pragma once

// ---------------------------------------------------------------------------
// MockAgentBackend — Stage 5.3.1
//
// Minimal IDebugBackend implementation for Agent API unit tests.
// No Board, SDL, or emulator dependency.
// Provides controllable test data for all Agent API operations.
// ---------------------------------------------------------------------------

#include "idebug_backend.h"
#include "events.h"
#include "symbol_database.h"
#include "opcode_info.h"
#include "rdb_controller.h"

#include <cstdint>
#include <cstring>
#include <future>
#include <map>
#include <vector>

class MockAgentBackend : public IDebugBackend
{
public:
    MockAgentBackend() {
        // Default: paused state
        cpu_.pc = 0x0100;
        cpu_.sp = 0xF800;
        cpu_.a = 0x42;
        cpu_.flags = 0x00;

        // Initialize memory to zero
        memory_.resize(65536, 0x00);

        // Place a simple program at 0x0100:
        // 0x0100: LXI SP, 0xF800   (31 00 F8)
        // 0x0103: MVI A, 0x55      (3E 55)
        // 0x0105: CALL 0x0200      (CD 00 02)
        // 0x0108: HLT              (76)
        memory_[0x0100] = 0x31; memory_[0x0101] = 0x00; memory_[0x0102] = 0xF8;
        memory_[0x0103] = 0x3E; memory_[0x0104] = 0x55;
        memory_[0x0105] = 0xCD; memory_[0x0106] = 0x00; memory_[0x0107] = 0x02;
        memory_[0x0108] = 0x76;

        // Place a subroutine at 0x0200:
        // 0x0200: PUSH H           (E5)
        // 0x0202: MVI H, 0xC0      (26 C0)
        // 0x0204: MVI L, 0x00      (2E 00)
        // 0x0206: MOV M, A         (77)
        // 0x0207: POP H            (E1)
        // 0x0208: RET              (C9)
        memory_[0x0200] = 0xE5;
        memory_[0x0202] = 0x26; memory_[0x0203] = 0xC0;
        memory_[0x0204] = 0x2E; memory_[0x0205] = 0x00;
        memory_[0x0206] = 0x77;
        memory_[0x0207] = 0xE1;
        memory_[0x0208] = 0xC9;

        // Pre-populate some activity counters
        executeCount_.resize(65536, 0);
        readCount_.resize(65536, 0);
        writeCount_.resize(65536, 0);

        executeCount_[0x0100] = 1;
        executeCount_[0x0103] = 1;
        executeCount_[0x0105] = 1;
        executeCount_[0x0200] = 1;
        executeCount_[0x0202] = 1;
        executeCount_[0x0204] = 1;
        executeCount_[0x0206] = 1;
        executeCount_[0x0207] = 1;
        executeCount_[0x0208] = 1;

        writeCount_[0xC000] = 1;  // VRAM write

        // Stage 6.20: Initialize runtime access map block addresses
        for (int i = 0; i < 256; ++i) {
            runtimeMap_[i].address = static_cast<uint16_t>(i * 256);
        }

        // Initialize video mode to consistent 256-mode defaults
        setVideoMode(false);
    }

    // -- State queries ------------------------------------------------------

    CpuState getCpuState() const override { return cpu_; }
    DebuggerState getState() const override { return state_; }
    bool isPaused() const override { return state_ == DebuggerState::Paused; }
    StopReason getStopReason() const override { return stopReason_; }

    // -- Execution control --------------------------------------------------

    void requestRun() override {
        // Simulate running until breakpoint or HLT (synchronous for tests)
        state_ = DebuggerState::Running;
        while (state_ == DebuggerState::Running) {
            uint16_t pc = cpu_.pc;

            // Check breakpoint
            if (hasBreakpoint(pc)) {
                state_ = DebuggerState::Paused;
                stopReason_ = StopReason::Breakpoint;
                return;
            }

            // Execute one instruction (simplified 8080)
            simulateStep();

            // Check HLT
            if (halted_) {
                state_ = DebuggerState::Paused;
                stopReason_ = StopReason::Step;
                return;
            }

            // Safety limit
            if (++runStepCount_ > 100000) {
                state_ = DebuggerState::Paused;
                stopReason_ = StopReason::UserPause;
                return;
            }
        }
    }

    // Stage 5.3.3.2: Mock implementation — runs synchronously, returns fulfilled future
    std::future<CommandResult> requestRunFuture() override {
        requestRun();
        std::promise<CommandResult> p;
        CommandResult r;
        r.success = true;
        r.status = CommandResult::Completed;
        p.set_value(r);
        return p.get_future();
    }

    void requestPause() override { state_ = DebuggerState::Paused; }
    void requestStep() override { simulateStep(); }
    void requestReset() override {
        cpu_.pc = 0x0100;
        cpu_.sp = 0xF800;
        state_ = DebuggerState::Paused;
        halted_ = false;
        runStepCount_ = 0;
    }
    void requestQuit() override { state_ = DebuggerState::Stopped; }
    void requestRestart() override {
        cpu_.pc = 0x0100;
        cpu_.sp = 0xF800;
        cpu_.a = 0x42;
        cpu_.flags = 0x00;
        state_ = DebuggerState::Paused;
        halted_ = false;
        runStepCount_ = 0;
    }

    void stepInstruction() override {
        simulateStep();
    }

    void requestSkipInstruction() override {
        simulateStep();
    }

    // -- Memory access ------------------------------------------------------

    uint8_t readMemory(uint16_t address) override {
        return memory_[address];
    }

    MemorySnapshot readMemorySnapshot(uint16_t start, size_t size) override {
        MemorySnapshot snap;
        snap.start = start;
        snap.data.resize(size);
        for (size_t i = 0; i < size; ++i) {
            snap.data[i] = memory_[static_cast<uint16_t>(start + i)];
        }
        return snap;
    }

    bool writeMemoryByte(uint16_t address, uint8_t value) override {
        memory_[address] = value;
        return true;
    }

    bool writeMemory(uint16_t address, const uint8_t* data, size_t size) override {
        for (size_t i = 0; i < size; ++i) {
            memory_[static_cast<uint16_t>(address + i)] = data[i];
        }
        return true;
    }

    // -- Register write -----------------------------------------------------

    bool writeRegister(RegisterId id, uint16_t value) override {
        switch (id) {
            case RegisterId::AF: cpu_.a = value >> 8; cpu_.flags = value & 0xFF; break;
            case RegisterId::BC: cpu_.b = value >> 8; cpu_.c = value & 0xFF; break;
            case RegisterId::DE: cpu_.d = value >> 8; cpu_.e = value & 0xFF; break;
            case RegisterId::HL: cpu_.h = value >> 8; cpu_.l = value & 0xFF; break;
            case RegisterId::SP: cpu_.sp = value; break;
            case RegisterId::PC: cpu_.pc = value; break;
        }
        return true;
    }

    // -- Breakpoints (direct — for backward compat) -------------------------

    int addBreakpoint(uint16_t address) override {
        for (auto &kv : breakpoints_) {
            if (kv.second.address == address) return -1;
        }
        int id = nextBpId_++;
        breakpoints_[id] = {address, true};
        return id;
    }

    bool removeBreakpoint(uint16_t address) override {
        for (auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
            if (it->second.address == address) {
                breakpoints_.erase(it);
                return true;
            }
        }
        return false;
    }

    bool setBreakpointEnabled(uint16_t address, bool enabled) override {
        for (auto &kv : breakpoints_) {
            if (kv.second.address == address) {
                kv.second.enabled = enabled;
                return true;
            }
        }
        return false;
    }

    bool hasBreakpoint(uint16_t address) const override {
        for (auto &kv : breakpoints_) {
            if (kv.second.address == address && kv.second.enabled) return true;
        }
        return false;
    }

    std::vector<DebuggerBreakpoint> getBreakpoints() const override {
        std::vector<DebuggerBreakpoint> result;
        for (auto &kv : breakpoints_) result.push_back(kv.second);
        return result;
    }

    void clearBreakpoints() override { breakpoints_.clear(); }

    // -- Breakpoint commands (Stage 5.3.1 — direct in mock) -----------------

    CommandResult requestAddBreakpoint(uint16_t addr) override {
        int id = addBreakpoint(addr);
        CommandResult r;
        r.success = (id >= 0);
        if (!r.success) { r.error = "duplicate"; r.status = CommandResult::Failed; }
        return r;
    }

    CommandResult requestRemoveBreakpoint(uint16_t addr) override {
        bool ok = removeBreakpoint(addr);
        CommandResult r;
        r.success = ok;
        if (!ok) { r.error = "not found"; r.status = CommandResult::Failed; }
        return r;
    }

    CommandResult requestSetBreakpointEnabled(uint16_t addr, bool enabled) override {
        bool ok = setBreakpointEnabled(addr, enabled);
        CommandResult r;
        r.success = ok;
        if (!ok) { r.error = "not found"; r.status = CommandResult::Failed; }
        return r;
    }

    CommandResult requestClearBreakpoints() override {
        breakpoints_.clear();
        CommandResult r;
        r.success = true;
        return r;
    }

    // -- History ------------------------------------------------------------

    uint64_t instructionSequence() const override { return seq_; }
    size_t instructionHistorySize() const override { return instrHistory_.size(); }
    std::vector<InstructionEvent> instructionHistorySnapshot() const override {
        return instrHistory_;
    }

    size_t memoryHistorySize() const override { return memHistory_.size(); }
    std::vector<MemoryAccessEvent> memoryHistorySnapshot() const override {
        return memHistory_;
    }

    size_t ioHistorySize() const override { return ioHistory_.size(); }
    std::vector<IoAccessEvent> ioHistorySnapshot() const override {
        return ioHistory_;
    }

    void clearHistory() override {
        instrHistory_.clear();
        memHistory_.clear();
    }
    void clearIoHistory() override { ioHistory_.clear(); }

    // -- Screen -------------------------------------------------------------

    ScreenSnapshot screenSnapshot() const override { return screenSnap_; }
    VideoModeSnapshot videoModeSnapshot() const override { return videoSnap_; }
    VramWriteSnapshot vramWriteSnapshot() const override { return {}; }

    // -- Beam / raster (Stage 6.27) -----------------------------------------
    BeamState beamState() const override { return beamState_; }

    std::vector<RasterEvent> rasterEvents(
        uint64_t frame, uint32_t vCycleStart, uint32_t vCycleEnd,
        int port, uint16_t pc, size_t maxResults) const override {
        std::vector<RasterEvent> out;
        for (const auto &ev : rasterEvents_) {
            if (frame != 0 && ev.frame != frame) continue;
            if (ev.vCycle < vCycleStart || ev.vCycle > vCycleEnd) continue;
            if (port >= 0 && ev.port != static_cast<uint8_t>(port)) continue;
            if (pc != 0xFFFF && ev.pc != pc) continue;
            out.push_back(ev);
        }
        if (maxResults != static_cast<size_t>(-1) && out.size() > maxResults)
            out.erase(out.begin(), out.begin() + (out.size() - maxResults));
        return out;
    }
    void clearRasterEvents() override { rasterEvents_.clear(); }
    void setRasterEvents(const std::vector<RasterEvent> &evs) { rasterEvents_ = evs; }

    // -- Palette ------------------------------------------------------------

    PaletteSnapshot paletteSnapshot() const override { return {}; }

    // -- Sound --------------------------------------------------------------

    SoundSnapshot soundSnapshot() const override { return {}; }
    void setMuted(bool) override {}

    // -- Activity -----------------------------------------------------------

    ActivitySnapshot activitySnapshot() const override {
        return {executeCount_, readCount_, writeCount_};
    }

    void clearActivityCounters() override {
        std::fill(executeCount_.begin(), executeCount_.end(), 0);
        std::fill(readCount_.begin(), readCount_.end(), 0);
        std::fill(writeCount_.begin(), writeCount_.end(), 0);
    }

    // -- Live Activity ------------------------------------------------------
    LiveActivitySnapshot liveActivitySnapshot() const override { return {}; }

    // -- Symbols (read-only) ------------------------------------------------
    // -- Symbols (read-only) ------------------------------------------------
    SymbolDatabase &symbolDatabase() override { return symbols_; }
    const SymbolDatabase &symbolDatabase() const override { return symbols_; }

    // -- Symbol commands (Stage 5.3.1 — direct in mock) ---------------------

    CommandResult requestCreateFunction(uint16_t addr, const std::string &name) override {
        bool ok = symbols_.addSymbol(addr, name, SymbolType::Function);
        CommandResult r; r.success = ok;
        if (!ok) { r.error = "exists"; r.status = CommandResult::Failed; }
        return r;
    }

    CommandResult requestRenameSymbol(uint16_t addr, const std::string &name) override {
        bool ok = symbols_.renameSymbol(addr, name);
        CommandResult r; r.success = ok;
        if (!ok) { r.error = "not found"; r.status = CommandResult::Failed; }
        return r;
    }

    CommandResult requestSetComment(uint16_t addr, const std::string &comment) override {
        bool ok = symbols_.setComment(addr, comment);
        CommandResult r; r.success = ok;
        if (!ok) { r.error = "not found"; r.status = CommandResult::Failed; }
        return r;
    }

    CommandResult requestRemoveSymbol(uint16_t addr) override {
        bool ok = symbols_.removeSymbol(addr);
        CommandResult r; r.success = ok;
        if (!ok) { r.error = "not found"; r.status = CommandResult::Failed; }
        return r;
    }

    CommandResult requestAddLabel(uint16_t addr, const std::string &name) override {
        bool ok = symbols_.addSymbol(addr, name, SymbolType::Label);
        CommandResult r; r.success = ok;
        if (!ok) { r.error = "exists"; r.status = CommandResult::Failed; }
        return r;
    }

    // -- Trace execution (Stage 5.3.2 — through queue) -----------------------

    std::future<TraceExecutionResult>
    requestExecuteTrace(const TraceExecutionParams &params) override {
        // Mock: execute synchronously, deliver via promise/future
        auto promise = std::make_shared<std::promise<TraceExecutionResult>>();
        auto future = promise->get_future();

        TraceExecutionResult result;
        result.startSequence = seq_;
        result.entrySp = cpu_.sp;
        result.minSp = cpu_.sp;
        result.maxSp = cpu_.sp;
        result.exitReason = ExitReason::Unknown;

        for (uint32_t i = 0; i < params.maxInstructions; ++i) {
            uint16_t pc = cpu_.pc;
            uint8_t opcode = memory_[pc];

            simulateStep();

            // Track SP bounds
            if (cpu_.sp < result.minSp) result.minSp = cpu_.sp;
            if (cpu_.sp > result.maxSp) result.maxSp = cpu_.sp;

            // Check RET
            if (params.stopOnRet && opcode == 0xC9) {
                result.exitReason = ExitReason::Ret;
                result.exitPc = pc;
                break;
            }

            // Check HLT
            if (opcode == 0x76) {
                result.exitReason = ExitReason::Halt;
                result.exitPc = pc;
                break;
            }

            // Check caller return
            if (params.stopOnCallerReturn && params.callerReturnAddress != 0
                && cpu_.pc == params.callerReturnAddress) {
                result.exitReason = ExitReason::CallerReturn;
                result.exitPc = pc;
                break;
            }
        }

        if (result.exitReason == ExitReason::Unknown) {
            result.exitReason = ExitReason::Timeout;
            result.exitPc = cpu_.pc;
        }

        result.endSequence = seq_;
        result.instructionsExecuted = static_cast<uint32_t>(result.endSequence - result.startSequence);
        result.exitSp = cpu_.sp;

        promise->set_value(result);
        return future;
    }

    // -- ROM ----------------------------------------------------------------
    bool loadRom(const std::string &, uint32_t) override { return true; }

    // -- WAV (tape input) ---------------------------------------------------
    bool loadWav(const std::string &) override { return true; }

    // -- Keyboard -----------------------------------------------------------
    void pressKey(int) override {}
    void releaseKey(int) override {}

    // -- Ruslat -------------------------------------------------------------
    bool isRuslatMode() const override { return false; }

    // -- I/O ports (Stage 6.1 Iteration 3) ----------------------------------

    uint8_t readIoPort(uint8_t port) override {
        return ioPorts_[port];
    }

    CommandResult writeIoPort(uint8_t port, uint8_t value) override {
        ioPorts_[port] = value;
        CommandResult r;
        r.success = true;
        r.status = CommandResult::Completed;
        return r;
    }

    // -- ROM Database (Stage 6.11) ------------------------------------------

    RdbController       &rdbController() override { return rdb_; }
    const RdbController &rdbController() const override { return rdb_; }

    // -- Runtime Memory Access Map (Stage 6.20) ------------------------------

    void clearRuntimeAccessMap() override {
        for (int i = 0; i < 256; ++i) {
            runtimeMap_[i] = RuntimeAccessBlock{};
            runtimeMap_[i].address = static_cast<uint16_t>(i * 256);
        }
        runtimeLog_.clear();
    }

    // Stage 6.25: clear ONLY the detailed log; aggregated map is preserved.
    void clearMemoryAccessLog() override {
        runtimeLog_.clear();
    }

    std::vector<RuntimeAccessBlock> getRuntimeAccessMap() const override {
        return std::vector<RuntimeAccessBlock>(runtimeMap_, runtimeMap_ + 256);
    }

    std::vector<RuntimeAccessLogEntry> getRuntimeAccessLog(size_t maxEntries) const override {
        if (maxEntries >= runtimeLog_.size()) return runtimeLog_;
        return std::vector<RuntimeAccessLogEntry>(
            runtimeLog_.end() - static_cast<long>(maxEntries), runtimeLog_.end());
    }

    // -- Memory Snapshots (Stage 6.20) ---------------------------------------

    uint32_t createMemorySnapshot(uint16_t start, size_t size) override {
        if (size == 0 || static_cast<uint32_t>(start) + size > 0x10000) return 0;
        MemorySnapshotData snap;
        snap.start_address = start;
        snap.data.resize(size);
        for (size_t i = 0; i < size; ++i)
            snap.data[i] = memory_[static_cast<uint16_t>(start + i)];
        uint32_t id = nextSnapId_++;
        snap.snapshot_id = id;
        snapshots_[id] = std::move(snap);
        return id;
    }

    MemorySnapshotData getMemorySnapshot(uint32_t id) const override {
        auto it = snapshots_.find(id);
        if (it == snapshots_.end()) return {};
        return it->second;
    }

    MemorySnapshotDiff compareMemorySnapshots(uint32_t idA, uint32_t idB) const override {
        MemorySnapshotDiff diff;
        auto itA = snapshots_.find(idA);
        auto itB = snapshots_.find(idB);
        if (itA == snapshots_.end() || itB == snapshots_.end()) return diff;
        const auto &a = itA->second;
        const auto &b = itB->second;
        if (a.start_address != b.start_address || a.data.size() != b.data.size()) return diff;
        bool inRange = false;
        uint16_t rangeStart = 0;
        size_t rangeSize = 0;
        for (size_t i = 0; i < a.data.size(); ++i) {
            if (a.data[i] != b.data[i]) {
                if (!inRange) { rangeStart = static_cast<uint16_t>(a.start_address + i); rangeSize = 1; inRange = true; }
                else ++rangeSize;
            } else {
                if (inRange) { diff.changed_ranges.push_back({rangeStart, rangeSize}); inRange = false; }
            }
        }
        if (inRange) diff.changed_ranges.push_back({rangeStart, rangeSize});
        return diff;
    }

    bool deleteMemorySnapshot(uint32_t id) override {
        return snapshots_.erase(id) > 0;
    }

    void invalidateAllSnapshots() override {
        snapshots_.clear();
    }

    // -- Test helpers for Stage 6.20 -----------------------------------------

    // Simulate a runtime memory access (for testing the mock's map/log)
    void simulateRuntimeAccess(uint16_t addr, RuntimeAccessLogEntry::Type type, uint16_t pc = 0, uint8_t value = 0) {
        int block = addr >> 8;
        switch (type) {
            case RuntimeAccessLogEntry::Read:
                runtimeMap_[block].read = true;
                runtimeMap_[block].read_count++;
                break;
            case RuntimeAccessLogEntry::Write:
                runtimeMap_[block].write = true;
                runtimeMap_[block].write_count++;
                break;
            case RuntimeAccessLogEntry::Fetch:
                runtimeMap_[block].fetch = true;
                runtimeMap_[block].fetch_count++;
                break;
        }
        RuntimeAccessLogEntry entry;
        entry.address = addr;
        entry.type = type;
        entry.pc = pc;
        entry.value = value;
        runtimeLog_.push_back(entry);
    }

    // -- Test data setters --------------------------------------------------

    void setCpuState(const CpuState &cpu) { cpu_ = cpu; }
    void setState(DebuggerState s) { state_ = s; }

    void setVideoMode(bool mode512, uint16_t vramBase = 0xC000) {
        videoSnap_.mode512 = mode512;
        videoSnap_.vramBase = vramBase;
        videoSnap_.visibleWidth = mode512 ? 512 : 256;
        videoSnap_.visibleHeight = 256;
        videoSnap_.pixelsPerByte = mode512 ? 4 : 8;
        videoSnap_.screenWidth = 576;
        videoSnap_.screenHeight = 288;
        videoSnap_.borderLeft = (videoSnap_.screenWidth - videoSnap_.visibleWidth) / 2;
        videoSnap_.borderTop = (videoSnap_.screenHeight - videoSnap_.visibleHeight) / 2;
    }

    // Stage 6.27: inject a deterministic beam state for agent/API tests.
    void setBeamState(const BeamState &s) { beamState_ = s; }

    // Stage 6.27: inject a synthetic TV framebuffer for screen-snapshot tests.
    void setScreenSnapshot(int w, int h, std::vector<uint32_t> px) {
        screenSnap_.width = w; screenSnap_.height = h;
        screenSnap_.pixels = std::move(px);
    }

    void setMemory(uint16_t addr, const std::vector<uint8_t> &data) {
        for (size_t i = 0; i < data.size(); ++i) {
            memory_[static_cast<uint16_t>(addr + i)] = data[i];
        }
    }

    void addInstructionEvent(const InstructionEvent &ev) {
        instrHistory_.push_back(ev);
    }

    void addMemoryEvent(const MemoryAccessEvent &ev) {
        memHistory_.push_back(ev);
    }

    void addIoEvent(const IoAccessEvent &ev) {
        ioHistory_.push_back(ev);
    }

    // Set I/O port value for testing
    void setIoPort(uint8_t port, uint8_t value) {
        ioPorts_[port] = value;
    }

    // Get I/O port value (to verify writes)
    uint8_t getIoPort(uint8_t port) const {
        return ioPorts_[port];
    }

private:
    CpuState cpu_{};
    DebuggerState state_ = DebuggerState::Paused;
    StopReason stopReason_ = StopReason::UserPause;

    std::vector<uint8_t> memory_;

    std::map<int, DebuggerBreakpoint> breakpoints_;
    int nextBpId_ = 1;

    uint64_t seq_ = 100;
    std::vector<InstructionEvent> instrHistory_;
    std::vector<MemoryAccessEvent> memHistory_;
    std::vector<IoAccessEvent> ioHistory_;

    ScreenSnapshot screenSnap_;
    VideoModeSnapshot videoSnap_;
    BeamState beamState_;
    std::vector<RasterEvent> rasterEvents_;

    std::vector<uint64_t> executeCount_;
    std::vector<uint64_t> readCount_;
    std::vector<uint64_t> writeCount_;

    SymbolDatabase symbols_;

    RdbController rdb_;

    // Stage 6.20: Runtime memory access tracking
    RuntimeAccessBlock runtimeMap_[256] = {};
    std::vector<RuntimeAccessLogEntry> runtimeLog_;

    // Stage 6.20: Memory snapshots
    std::map<uint32_t, MemorySnapshotData> snapshots_;
    uint32_t nextSnapId_ = 1;

    // I/O port state (256 ports)
    uint8_t ioPorts_[256] = {};

    bool halted_ = false;
    uint32_t runStepCount_ = 0;

    // Simplified 8080 step for mock — handles key instructions
    void simulateStep() {
        uint16_t pc = cpu_.pc;
        uint8_t opcode = memory_[pc];
        uint8_t len = opcode_info::get_length(opcode);

        // Record instruction event
        InstructionEvent ie;
        ie.sequence = seq_;
        ie.pcBefore = pc;
        ie.opcode = opcode;
        ie.length = len;
        ie.before = cpu_;
        if (len >= 2) ie.operandBytes[0] = memory_[pc + 1];
        if (len >= 3) ie.operandBytes[1] = memory_[pc + 2];

        // Simulate key instructions
        switch (opcode) {
        case 0xC9: // RET
            // Pop PC from stack
            cpu_.pc = memory_[cpu_.sp] | (memory_[cpu_.sp + 1] << 8);
            cpu_.sp += 2;
            break;

        case 0xCD: // CALL addr16
        {
            uint16_t target = memory_[pc + 1] | (memory_[pc + 2] << 8);
            cpu_.sp -= 2;
            memory_[cpu_.sp] = (pc + 3) & 0xFF;
            memory_[cpu_.sp + 1] = ((pc + 3) >> 8) & 0xFF;
            cpu_.pc = target;
            break;
        }

        case 0xE5: // PUSH HL
            cpu_.sp -= 2;
            memory_[cpu_.sp] = cpu_.l;
            memory_[cpu_.sp + 1] = cpu_.h;
            cpu_.pc = pc + len;
            break;

        case 0xE1: // POP HL
            cpu_.l = memory_[cpu_.sp];
            cpu_.h = memory_[cpu_.sp + 1];
            cpu_.sp += 2;
            cpu_.pc = pc + len;
            break;

        case 0x76: // HLT
            halted_ = true;
            cpu_.pc = pc;  // PC stays at HLT
            break;

        case 0x77: // MOV M, A — write A to (HL)
        {
            uint16_t addr = (static_cast<uint16_t>(cpu_.h) << 8) | cpu_.l;
            memory_[addr] = cpu_.a;
            // Record memory write event
            MemoryAccessEvent mev;
            mev.instructionSequence = seq_;
            mev.type = MemoryAccessType::Write;
            mev.virt = addr;
            mev.value = cpu_.a;
            mev.stack = false;
            memHistory_.push_back(mev);
            cpu_.pc = pc + len;
            break;
        }

        case 0xC3: // JMP addr16
        {
            uint16_t target = memory_[pc + 1] | (memory_[pc + 2] << 8);
            cpu_.pc = target;
            break;
        }

        default:
            // Default: advance PC by instruction length
            cpu_.pc = pc + len;
            break;
        }

        ie.pcAfter = cpu_.pc;
        ie.after = cpu_;
        ie.cycles = 4;

        instrHistory_.push_back(ie);
        seq_++;
    }
};
