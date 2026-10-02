#pragma once

#include "debug_target.h"

#include <thread>

class Memory;

// ---------------------------------------------------------------------------
// NoBoardTarget — minimal IDebugTarget for tests.
//
// Provides CPU (i8080 global state) + Memory without requiring Board.
// Replaces the old DEBUGGER_NO_BOARD conditional compilation in DebugBackend.
// ---------------------------------------------------------------------------

class NoBoardTarget : public IDebugTarget
{
public:
    NoBoardTarget(Memory &memory);
    ~NoBoardTarget() override;

    uint8_t readMemory(uint16_t addr) override;
    uint8_t peekMemory(uint16_t addr) override;
    uint8_t readMemoryRaw(uint16_t addr) override;
    void    writeMemory(uint16_t addr, uint8_t val) override;
    void setMemoryCallbacks(MemoryReadCallback onRead,
                            MemoryWriteCallback onWrite) override;

    CpuState getCpuState() override;
    void     writeCpuRegister(int reg, uint16_t val) override;

    void stepInstruction() override;
    void executeFrame() override;
    void reset(bool loadRom) override;

    void debuggerBreak() override {}
    void debuggerContinue() override {}
    void debuggerAttached() override {}
    void debuggerDetached() override {}
    void setPollCallback(std::function<void()> cb) override {}

    void syncBreakpoints(const struct DebuggerBreakpoint *bps, size_t count) override {}

    ScreenData screenSnapshot() override { return {}; }

    void pressKey(int) override {}
    void releaseKey(int) override {}

    bool loadRom(const std::string &path, uint32_t org) override;
    bool loadWav(const std::string &path) override { return false; }
    void initCpu(uint16_t pc, uint16_t sp) override;

    bool framePacingEnabled() const override { return false; }

    // Audio gate — recorded so tests can assert that the backend opens and
    // closes the output around the frame loop.
    void setAudioEmulationActive(bool active) override {
        audioGateLog_.push_back(active);
        audioActive_ = active;
    }
    bool audioEmulationActive() const { return audioActive_; }
    const std::vector<bool> &audioGateLog() const { return audioGateLog_; }
    void clearAudioGateLog() { audioGateLog_.clear(); }

    // Thread that last executed loadRom() — a ROM load mutates the whole
    // machine, so it must happen on the emulation thread, never on the
    // thread that asked for it.
    std::thread::id loadRomThread() const { return loadRomThread_; }

private:
    Memory &memory_;
    bool    cpuInitialized_ = false;
    bool    audioActive_ = false;
    std::vector<bool> audioGateLog_;
    std::thread::id loadRomThread_{};

    // prevOnRead_/prevOnWrite_ chain the RAW Memory::onread/onwrite type
    // (4-parameter). Stage 6.24: MemoryReadCallback grew a 5th pc param, but
    // the underlying Memory::onread is unchanged, so we must use the raw type.
    std::function<void(uint32_t,uint32_t,bool,uint8_t)> prevOnRead_;
    MemoryWriteCallback prevOnWrite_;
};
