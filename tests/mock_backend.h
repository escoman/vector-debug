#pragma once

// ---------------------------------------------------------------------------
// MockDebugBackend — Stage 3.13
//
// Provides test data for GUI testing without a real emulator.
// Implements the same public interface as DebugBackend for the methods
// used by GUI windows.
//
// This mock is single-threaded and does NOT require Board, Memory, or
// any emulator component. It returns fixed test data for all queries.
// ---------------------------------------------------------------------------

#include "backend.h"
#include "events.h"
#include "symbol_database.h"

#include <cstdint>
#include <vector>
#include <map>

class MockDebugBackend
{
public:
    MockDebugBackend() = default;

    // -- State ---------------------------------------------------------------

    DebuggerState getState() const { return mockState_; }
    bool isPaused() const { return mockState_ == DebuggerState::Paused; }
    StopReason getStopReason() const { return mockStopReason_; }

    void setMockState(DebuggerState s) { mockState_ = s; }
    void setMockStopReason(StopReason r) { mockStopReason_ = r; }

    // -- CPU state -----------------------------------------------------------

    CpuState getCpuState() const { return mockCpu_; }

    void setMockCpuState(const CpuState &s) { mockCpu_ = s; }

    // -- Memory access -------------------------------------------------------

    uint8_t readMemory(uint16_t address) {
        size_t offset = address - mockMemoryBase_;
        if (offset < mockMemory_.size()) return mockMemory_[offset];
        return 0x00;
    }

    MemorySnapshot readMemorySnapshot(uint16_t start, size_t size) {
        MemorySnapshot snap;
        snap.start = start;
        snap.data.resize(size, 0x00);
        for (size_t i = 0; i < size; ++i) {
            snap.data[i] = readMemory(static_cast<uint16_t>(start + i));
        }
        return snap;
    }

    // Set mock memory contents (base address + data)
    void setMockMemory(uint16_t base, const std::vector<uint8_t> &data) {
        mockMemoryBase_ = base;
        mockMemory_ = data;
    }

    // -- Memory write (returns success) --------------------------------------

    bool writeMemoryByte(uint16_t address, uint8_t value) {
        size_t offset = address - mockMemoryBase_;
        if (offset < mockMemory_.size()) {
            mockMemory_[offset] = value;
            return true;
        }
        return false;
    }

    bool writeMemory(uint16_t address, const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            if (!writeMemoryByte(static_cast<uint16_t>(address + i), data[i]))
                return false;
        }
        return true;
    }

    // -- Register write ------------------------------------------------------

    using RegisterId = DebugBackend::RegisterId;

    bool writeRegister(RegisterId id, uint16_t value) {
        switch (id) {
            case RegisterId::AF: mockCpu_.a = value >> 8; mockCpu_.flags = value & 0xFF; break;
            case RegisterId::BC: mockCpu_.b = value >> 8; mockCpu_.c = value & 0xFF; break;
            case RegisterId::DE: mockCpu_.d = value >> 8; mockCpu_.e = value & 0xFF; break;
            case RegisterId::HL: mockCpu_.h = value >> 8; mockCpu_.l = value & 0xFF; break;
            case RegisterId::SP: mockCpu_.sp = value; break;
            case RegisterId::PC: mockCpu_.pc = value; break;
        }
        return true;
    }

    // -- Execution control (no-ops in mock) ----------------------------------

    void requestStep() {}
    void requestRun() { mockState_ = DebuggerState::Running; }
    void requestPause() { mockState_ = DebuggerState::Paused; }
    void requestReset() {}
    void requestRestart() {}
    void requestQuit() {}
    bool isQuitRequested() const { return false; }

    // -- Breakpoints ---------------------------------------------------------

    int addBreakpoint(uint16_t address) {
        for (auto &kv : mockBreakpoints_) {
            if (kv.second.address == address) return -1;
        }
        int id = mockNextBpId_++;
        mockBreakpoints_[id] = {address, true};
        return id;
    }

    bool removeBreakpoint(uint16_t address) {
        for (auto it = mockBreakpoints_.begin(); it != mockBreakpoints_.end(); ++it) {
            if (it->second.address == address) {
                mockBreakpoints_.erase(it);
                return true;
            }
        }
        return false;
    }

    bool hasBreakpoint(uint16_t address) const {
        for (auto &kv : mockBreakpoints_) {
            if (kv.second.address == address && kv.second.enabled) return true;
        }
        return false;
    }

    std::vector<DebuggerBreakpoint> getBreakpoints() const {
        std::vector<DebuggerBreakpoint> result;
        for (auto &kv : mockBreakpoints_) {
            result.push_back(kv.second);
        }
        return result;
    }

    void clearBreakpoints() { mockBreakpoints_.clear(); }

    // request* variants (matching IDebugBackend interface)
    struct CommandResult { bool success = true; };
    CommandResult requestAddBreakpoint(uint16_t) { return {}; }
    CommandResult requestRemoveBreakpoint(uint16_t) { return {}; }
    CommandResult requestSetBreakpointEnabled(uint16_t, bool) { return {}; }
    CommandResult requestClearBreakpoints() { mockBreakpoints_.clear(); return {}; }

    // -- History snapshots ---------------------------------------------------

    std::vector<InstructionEvent> instructionHistorySnapshot() const {
        return mockInstructions_;
    }

    std::vector<IoAccessEvent> ioHistorySnapshot() const {
        return mockIoEvents_;
    }

    void setMockInstructionHistory(const std::vector<InstructionEvent> &h) {
        mockInstructions_ = h;
    }

    void setMock_IoHistory(const std::vector<IoAccessEvent> &h) {
        mockIoEvents_ = h;
    }

    // -- Screen snapshot -----------------------------------------------------

    using ScreenSnapshot = DebugBackend::ScreenSnapshot;

    ScreenSnapshot screenSnapshot() const { return mockScreen_; }

    void setMockScreen(const ScreenSnapshot &s) { mockScreen_ = s; }

    // -- Video mode snapshot -------------------------------------------------

    using VideoModeSnapshot = DebugBackend::VideoModeSnapshot;

    VideoModeSnapshot videoModeSnapshot() const { return mockVideoMode_; }

    void setMockVideoMode(const VideoModeSnapshot &v) { mockVideoMode_ = v; }

    // -- Activity snapshot ---------------------------------------------------

    using ActivitySnapshot = DebugBackend::ActivitySnapshot;

    ActivitySnapshot activitySnapshot() const { return mockActivity_; }

    void setMockActivity(const ActivitySnapshot &a) { mockActivity_ = a; }

    // -- Symbol database (minimal) -------------------------------------------

    SymbolDatabase &symbolDatabase() { return mockSymbols_; }
    const SymbolDatabase &symbolDatabase() const { return mockSymbols_; }

    // -- Instrumentation (no-ops) --------------------------------------------

    void setInstrumentationEnabled(bool) {}
    bool isInstrumentationEnabled() const { return true; }

    // -- Create default test data --------------------------------------------

    void setupDefaultTestData() {
        // CPU state: typical paused state
        mockCpu_ = {};
        mockCpu_.pc = 0x0100;
        mockCpu_.sp = 0xFF00;
        mockCpu_.a = 0x42;
        mockCpu_.flags = 0x00;

        // Memory: simple program at 0x0100
        mockMemoryBase_ = 0x0000;
        mockMemory_.resize(256, 0x00);
        // NOP NOP NOP HLT at 0x0100
        mockMemory_[0x0100] = 0x00; // NOP
        mockMemory_[0x0101] = 0x00; // NOP
        mockMemory_[0x0102] = 0x00; // NOP
        mockMemory_[0x0103] = 0x76; // HLT

        // State: paused
        mockState_ = DebuggerState::Paused;
        mockStopReason_ = StopReason::UserPause;

        // Screen: small test pattern
        mockScreen_.width = 256;
        mockScreen_.height = 256;
        mockScreen_.pixels.resize(256 * 256, 0xFF000000);

        // Video mode
        mockVideoMode_ = {};
        mockVideoMode_.mode512 = false;
        mockVideoMode_.scrollValue = 0;
    }

private:
    DebuggerState mockState_ = DebuggerState::Paused;
    StopReason mockStopReason_ = StopReason::None;
    CpuState mockCpu_ = {};

    uint16_t mockMemoryBase_ = 0;
    std::vector<uint8_t> mockMemory_;

    std::map<int, DebuggerBreakpoint> mockBreakpoints_;
    int mockNextBpId_ = 1;

    std::vector<InstructionEvent> mockInstructions_;
    std::vector<IoAccessEvent> mockIoEvents_;

    ScreenSnapshot mockScreen_;
    VideoModeSnapshot mockVideoMode_;
    ActivitySnapshot mockActivity_;
    SymbolDatabase mockSymbols_;
};
