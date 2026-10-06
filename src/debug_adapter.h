#pragma once

#include "debug_target.h"
#include "ring_buffer.h"
#include "memory.h"
#include "vio.h"
#include "tv.h"
#include "board.h"
#include "keyboard.h"
#include "8253.h"
#include "sound.h"
#include "ay.h"
#include "wav.h"
#include "fd1793.h"

#include <string>
#include <set>
#include <atomic>
#include <chrono>

// ---------------------------------------------------------------------------
// DebugAdapter
//
// Adapter layer between Debugger Core and Vector Emulator.
// Implements IDebugTarget — the ONLY component with direct access to
// emulator internals (Board, Memory, CPU, IO, TV, etc.).
//
// Also owns the HAL function definitions (i8080_hal_*) that connect
// the CPU core to emulator components.
// ---------------------------------------------------------------------------

class DebugAdapter : public IDebugTarget
{
public:
    DebugAdapter();
    ~DebugAdapter() override;

    void init();
    void shutdown();

    // -- IDebugTarget implementation ----------------------------------------

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
    void reset(bool attachBoot) override;

    void debuggerBreak() override;
    void debuggerContinue() override;
    void debuggerAttached() override;
    void debuggerDetached() override;
    void setPollCallback(std::function<void()> cb) override;

    void syncBreakpoints(const DebuggerBreakpoint *bps, size_t count) override;

    ScreenData screenSnapshot() override;
    BeamState  getBeamState() override;
    std::vector<RasterEvent> getRasterEvents(
        uint64_t frame, uint32_t vCycleStart, uint32_t vCycleEnd,
        int port, uint16_t pc, size_t maxResults) override;
    void clearRasterEvents() override;
    PaletteSnapshot paletteSnapshot() const override;
    SoundSnapshot soundSnapshot() const override;
    void setMuted(bool muted) override;
    void setAudioEmulationActive(bool active) override;

    // True when the audio device must stay silent: either the user muted the
    // machine or the emulation loop is not producing frames. Diagnostic and
    // test hook — the real effect happens in updateAudioPause().
    bool isAudioOutputPaused() const { return audioMuted_ || !audioEmulationActive_; }

    void pressKey(int scancode) override;
    void releaseKey(int scancode) override;
    bool isRuslatMode() const override { return ruslatState_; }

    bool loadRom(const std::string &path, uint32_t org) override;
    bool loadWav(const std::string &path) override;
    void initCpu(uint16_t pc, uint16_t sp) override;

    // -- I/O ports (Stage 6.1 Iteration 3) ----------------------------------

    uint8_t readIoPort(uint8_t port) override;
    void    writeIoPort(uint8_t port, uint8_t value) override;

    // -- HAL binding --------------------------------------------------------

    // Bind HAL callbacks to this adapter's components.
    void bindHal();

    // Set static HAL pointers from external code.
    static void setHalPointers(Memory *mem, IO *io, Board *board);

    // Static accessors for HAL functions.
    static Memory* halMemory() { return s_memory; }
    static IO*     halIo()     { return s_io; }
    static Board*  halBoard()  { return s_board; }

private:
    // -- Video timing constants (Stage 6.27) --------------------------------
    // Declared ONCE here, in the adapter layer — the only Vector-specific
    // access point. Agent API / MCP read them from the BeamState JSON and
    // never compute timing themselves.
    //
    // Source of truth: src/filler.cpp — one pixel-time per iteration, line
    // ends at raster_pixel == 768, frame wraps at raster_line == 312
    // (22 vsync + 18 border + 256 picture + 16 border lines).
    struct VideoTiming
    {
        static constexpr uint32_t lineVCycles  = 768;
        static constexpr uint32_t frameLines   = 312;
        static constexpr uint32_t frameVCycles = lineVCycles * frameLines; // 239616
    };

    // -- Emulator components (hidden from outside) ----------------------------

    Memory memory;
    FD1793 fdc;
    Wav wav;
    WavPlayer tape_player;
    Keyboard keyboard;
    I8253 timer;
    TimerWrapper tw;
    AY ay;
    AYWrapper aw;
    Soundnik soundnik;
    IO io;
    TV tv;
    PixelFiller filler;
    Board board;

    bool initialized_ = false;

    // РУС/LAT LED state — updated by io.onruslat callback
    bool ruslatState_ = false;

    // Audio output gating (see setAudioEmulationActive()). Both flags are
    // written from the emulation/GUI threads and only ever funnel into
    // Soundnik::pause(), which is the SDL-sanctioned way to stop the callback
    // thread — it waits for the running callback to finish.
    bool audioMuted_ = false;             // user's Mute toggle
    bool audioEmulationActive_ = false;   // frame loop is running
    void updateAudioPause();

    // Timer write tracking (i8253 counter ports 0x09-0x0B, control port 0x08)
    // Interprets the i8253 write protocol to extract counter load values.
    // Core mapping (vio.h): timer.write(~port & 3) — port 0x08 selects the
    // control register, 0x0B/0x0A/0x09 select counters 0/1/2.
    int  timerLatchModes_[3] = {3, 3, 3};  // latch mode per counter (default: LSB+MSB)
    int  timerWriteStates_[3] = {};  // write state machine per counter
    uint16_t timerLoadValues_[3] = {}; // last load value per counter
    int  timerModes_[3] = {};        // mode per counter (0-5)
    uint8_t timerWriteLsb_[3] = {};  // temp LSB storage per counter
    bool timerDirty_[3] = {};        // true if counter was written since last snapshot
    // Persistent tone-output state per counter, mirroring the emulator's
    // CounterUnit::enabled for mode 3 (8253.h): set when a divider load
    // completes in square-wave mode, cleared when a mode-set control word
    // reprograms the counter. Drives the sustained note bar in the Sound
    // window; `timerDirty_` only marks the single frame a write happened.
    bool timerSounding_[3] = {};     // true while counter generates a tone

    // AY write tracking (ports 0x14/0x15)
    bool ayDirty_ = false;           // true if AY was written since last snapshot

    // Standard Vector noise tracking — PIA1 Port C bit 0 (tape-out beeper).
    // Counted in the io.onwrite hook (emulation thread) as actual PC0 state
    // transitions only (old != new), including BSR semantics on port 0x00.
    // This is a diagnostic activity measurement, not a volume level.
    std::atomic<uint64_t> pc0TogglesTotal_{0}; // monotonic, emulation thread writes
    int pc0Mirror_ = -1;                       // last observed PC0 (-1 = unknown)

    // Snapshot interval bookkeeping — accessed from the snapshot (GUI) thread
    // only, together with the read side of pc0TogglesTotal_.
    uint64_t pc0TogglesLastSnapshot_ = 0;
    std::chrono::steady_clock::time_point pc0SnapshotTime_{};
    bool pc0SnapshotPrimed_ = false;

    MemoryReadCallback  memReadCb_;
    MemoryWriteCallback memWriteCb_;

    // Stage 6.27 P2: OUT-with-beam-position events, recorded on the emulation
    // thread (io.onwrite) and read from the query thread via RingBuffer.
    RingBuffer<RasterEvent> rasterEvents_;
    std::function<void(uint32_t,uint32_t,bool,uint8_t)> prevMemOnRead_;
    std::function<void(uint32_t,uint32_t,bool,uint8_t)> prevMemOnWrite_;

    // Track breakpoints synced to Board (addresses only)
    std::set<uint16_t> syncedBreakpoints_;

    static Memory       *s_memory;
    static IO           *s_io;
    static Board        *s_board;
};
