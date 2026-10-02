#pragma once

#include "debugger_types.h"

#include <cstdint>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// IDebugTarget — interface between Debugger Core and the emulator Adapter.
//
// DebugBackend communicates ONLY through this interface.
// The real implementation is DebugAdapter (full Vector emulator).
// Test implementations (NoBoardTarget) provide CPU + Memory only.
// ---------------------------------------------------------------------------

class IDebugTarget
{
public:
    virtual ~IDebugTarget() = default;

    // -- Memory access ------------------------------------------------------

    virtual uint8_t readMemory(uint16_t addr) = 0;
    virtual uint8_t peekMemory(uint16_t addr) = 0;  // read without callbacks (paused only)
    virtual uint8_t readMemoryRaw(uint16_t addr) = 0;  // read without callbacks (thread-safe)
    virtual void    writeMemory(uint16_t addr, uint8_t val) = 0;

    // Memory instrumentation callbacks.
    // DebugBackend provides lambdas that record memory access events.
    // The target installs them on the underlying Memory (if any).
    // Pass nullptr to clear (called by DebugBackend destructor).
    //
    // Stage 6.24: onRead also receives the CPU's PC at the moment of the
    // callback (i8080_pc() evaluated inside the target's onread lambda).
    // For an opcode/operand fetch (RD_BYTE(PC++)), PC has already been
    // incremented, so isFetchStart ⇔ fetchRemaining==0 && !stack &&
    // (virt+1 == pc).  For a data read (RD_BYTE(HL) etc.) this relation
    // does not hold.  No new src/-side callback is required.
    using MemoryReadCallback  = std::function<void(uint32_t virt, uint32_t phys, bool stack, uint8_t value, uint16_t pc)>;
    using MemoryWriteCallback = std::function<void(uint32_t virt, uint32_t phys, bool stack, uint8_t value)>;

    virtual void setMemoryCallbacks(MemoryReadCallback onRead,
                                    MemoryWriteCallback onWrite) {}

    // -- CPU state ----------------------------------------------------------

    virtual CpuState getCpuState() = 0;
    virtual void     writeCpuRegister(int reg, uint16_t val) = 0;

    // -- Execution control --------------------------------------------------

    virtual void stepInstruction() = 0;
    virtual void executeFrame() = 0;
    virtual void reset(bool attachBoot) = 0;  // true=BLK+ВВОД (attach boot ROM), false=BLK+СБР (detach boot ROM)

    // -- Debugger control ---------------------------------------------------

    virtual void debuggerBreak() = 0;
    virtual void debuggerContinue() = 0;
    virtual void debuggerAttached() = 0;
    virtual void debuggerDetached() = 0;
    virtual void setPollCallback(std::function<void()> cb) = 0;

    // -- Breakpoints --------------------------------------------------------

    virtual void syncBreakpoints(const struct DebuggerBreakpoint *bps, size_t count) = 0;

    // -- Screen -------------------------------------------------------------

    virtual ScreenData screenSnapshot() = 0;

    // -- Beam / raster state (Stage 6.27) ----------------------------------
    // Read-only snapshot of the current beam/raster position and the video
    // state associated with it. Must NOT pause/step/reset the emulation.
    // Default: unavailable (test targets without a real video path).

    virtual BeamState getBeamState() { BeamState s; return s; }

    // Ring of OUT-with-beam-position events recorded on the emulation thread.
    // Filters: frame (0 = any), v_cycle range [start,end] (end UINT32_MAX = any),
    // port (-1 = any), pc (UINT16_MAX = any). maxResults caps the tail.
    virtual std::vector<RasterEvent> getRasterEvents(
        uint64_t frame, uint32_t vCycleStart, uint32_t vCycleEnd,
        int port, uint16_t pc, size_t maxResults)
    {
        (void)frame; (void)vCycleStart; (void)vCycleEnd;
        (void)port; (void)pc; (void)maxResults;
        return {};
    }
    virtual void clearRasterEvents() {}

    // -- Palette ------------------------------------------------------------

    virtual PaletteSnapshot paletteSnapshot() const { return {}; }

    // -- Sound --------------------------------------------------------------

    virtual SoundSnapshot soundSnapshot() const { return {}; }
    virtual void setMuted(bool muted) { (void)muted; }

    // Audio output gate: true only while the emulation loop is executing
    // frames. Sound samples are produced by the frame loop, but the SDL
    // audio callback drains the sample ring on its own thread, so without
    // this the speaker keeps playing after the CPU has been paused.
    // Called from the emulation thread only.
    virtual void setAudioEmulationActive(bool active) { (void)active; }

    // -- Keyboard injection -------------------------------------------------

    virtual void pressKey(int scancode) {}
    virtual void releaseKey(int scancode) {}
    virtual bool isRuslatMode() const { return false; }

    // -- I/O ports (Stage 6.1 Iteration 3) ---------------------------------
    // readIoPort: returns port value (0-255). Default returns 0xFF.
    // writeIoPort: writes value to port. Default is no-op.
    // These are called from emulation thread only (via Command Queue for write).

    virtual uint8_t readIoPort(uint8_t port) { (void)port; return 0xFF; }
    virtual void    writeIoPort(uint8_t port, uint8_t value) { (void)port; (void)value; }

    // -- ROM / init ---------------------------------------------------------

    virtual bool loadRom(const std::string &path, uint32_t org) = 0;
    virtual bool loadWav(const std::string &path) = 0;
    virtual void initCpu(uint16_t pc, uint16_t sp) = 0;

    // -- Frame pacing -------------------------------------------------------
    // Real targets (DebugAdapter) execute full frames and need 50 Hz pacing.
    // Test targets (NoBoardTarget) execute one instruction per "frame" and
    // should run at full speed.

    virtual bool framePacingEnabled() const { return true; }
};
