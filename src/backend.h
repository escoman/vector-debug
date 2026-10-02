#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <memory>
#include <future>

#include "idebug_backend.h"
#include "debug_target.h"
#include "rdb_controller.h"
#include "ring_buffer.h"

// ---------------------------------------------------------------------------
// Result of a single-instruction step (kept for backward compatibility)
// ---------------------------------------------------------------------------

struct StepResult
{
    uint16_t pcBefore;
    uint16_t pcAfter;

    uint8_t opcode;
    uint8_t length;      // instruction length in bytes (1, 2 or 3)
    uint32_t cycles;     // CPU cycles consumed

    CpuState before;
    CpuState after;
};

// ---------------------------------------------------------------------------
// DebugBackend
//
// Implements IDebugBackend (GUI facade) and works through IDebugTarget
// (emulator adapter). No direct access to Board, Memory, CPU, or IO.
// ---------------------------------------------------------------------------

class DebugBackend : public IDebugBackend
{
public:
    explicit DebugBackend(IDebugTarget &target);

    ~DebugBackend();

    // Stage 5.3.3.1: Test-only flag — allows synchronous command execution
    // when no emulation thread is running. Production code must NEVER set this.
    bool testSynchronous_ = false;

    // -- Target integration (Stage 3.13a) -----------------------------------

    void attachTarget(IDebugTarget *target);
    IDebugTarget *getTarget() const { return target_; }

    // -- IDebugBackend: ROM -------------------------------------------------

    bool loadRom(const std::string &path, uint32_t org = 0) override;

    // -- IDebugBackend: WAV (tape input) ------------------------------------

    bool loadWav(const std::string &path) override;

    // -- IDebugBackend: keyboard injection ----------------------------------

    void pressKey(int scancode) override;
    void releaseKey(int scancode) override;
    bool isRuslatMode() const override;

    // -- IDebugBackend: I/O ports (Stage 6.1 Iteration 3) -------------------

    uint8_t readIoPort(uint8_t port) override;
    CommandResult writeIoPort(uint8_t port, uint8_t value) override;

    // -- IDebugBackend: memory access ---------------------------------------

    uint8_t readMemory(uint16_t address) override;
    MemorySnapshot readMemorySnapshot(uint16_t start, size_t size) override;
    bool writeMemoryByte(uint16_t address, uint8_t value) override;
    bool writeMemory(uint16_t address, const uint8_t* data, size_t size) override;

    // -- IDebugBackend: register write --------------------------------------

    bool writeRegister(RegisterId id, uint16_t value) override;

    // -- IDebugBackend: state queries ---------------------------------------

    CpuState       getCpuState()    const override;
    DebuggerState  getState()       const override;
    bool           isPaused()       const override;
    StopReason     getStopReason()  const override;

    // -- IDebugBackend: execution control -----------------------------------

    void requestRun()      override;
    std::future<CommandResult> requestRunFuture();  // Stage 5.3.3.2: returns pause future
    void requestPause()    override;
    void requestStep()     override;
    void requestReset()    override;
    void requestRestart()  override;  // BLK+ВВОД: attach boot ROM, PC=0
    void requestQuit()     override;
    void stepInstruction() override;  // IDebugBackend: execute 1 instruction (void)
    void requestSkipInstruction() override;  // run until PC == next instruction

    // Extended step that returns detailed result (for tests/tracing)
    StepResult stepInstructionDetailed();

    // -- IDebugBackend: breakpoints -----------------------------------------

    int  addBreakpoint(uint16_t address) override;
    bool removeBreakpoint(uint16_t address) override;
    bool setBreakpointEnabled(uint16_t address, bool enabled) override;
    bool hasBreakpoint(uint16_t address) const override;
    std::vector<DebuggerBreakpoint> getBreakpoints() const override;
    void clearBreakpoints() override;

    // Legacy: remove by id (backward compat with tests)
    void removeBreakpoint(int id);

    // -- IDebugBackend: breakpoint commands (Stage 5.3.1) -------------------

    CommandResult requestAddBreakpoint(uint16_t addr) override;
    CommandResult requestRemoveBreakpoint(uint16_t addr) override;
    CommandResult requestSetBreakpointEnabled(uint16_t addr, bool enabled) override;
    CommandResult requestClearBreakpoints() override;

    // -- IDebugBackend: history ---------------------------------------------

    uint64_t instructionSequence() const override;
    size_t   instructionHistorySize() const override;
    std::vector<InstructionEvent> instructionHistorySnapshot() const override;

    size_t   memoryHistorySize() const override;
    std::vector<struct MemoryAccessEvent> memoryHistorySnapshot() const override;

    size_t   ioHistorySize() const override;
    std::vector<IoAccessEvent> ioHistorySnapshot() const override;

    void clearHistory() override;
    void clearIoHistory() override;

    // -- IDebugBackend: screen ----------------------------------------------

    ScreenSnapshot     screenSnapshot() const override;
    VideoModeSnapshot  videoModeSnapshot() const override;
    VramWriteSnapshot  vramWriteSnapshot() const override;

    BeamState beamState() const override;

    std::vector<RasterEvent> rasterEvents(
        uint64_t frame, uint32_t vCycleStart, uint32_t vCycleEnd,
        int port, uint16_t pc, size_t maxResults) const override;
    void clearRasterEvents() override;

    // -- IDebugBackend: palette ---------------------------------------------

    PaletteSnapshot paletteSnapshot() const override;

    // -- IDebugBackend: sound -----------------------------------------------

    SoundSnapshot soundSnapshot() const override;
    void setMuted(bool muted) override;

    // -- IDebugBackend: activity --------------------------------------------

    ActivitySnapshot activitySnapshot() const override;
    void clearActivityCounters() override;

    // -- IDebugBackend: live activity (Stage 5.2) ----------------------------

    LiveActivitySnapshot liveActivitySnapshot() const override;

    // -- IDebugBackend: symbols ---------------------------------------------

    SymbolDatabase       &symbolDatabase() override { return symbols_; }
    const SymbolDatabase &symbolDatabase() const override { return symbols_; }

    // -- IDebugBackend: ROM Database (Stage 6.11) ----------------------------

    RdbController       &rdbController() override { return *rdb_; }
    const RdbController &rdbController() const override { return *rdb_; }

    // -- IDebugBackend: Runtime Memory Access Map (Stage 6.20) ---------------

    void clearRuntimeAccessMap() override;
    void clearMemoryAccessLog() override;   // Stage 6.25
    std::vector<RuntimeAccessBlock> getRuntimeAccessMap() const override;
    std::vector<RuntimeAccessLogEntry> getRuntimeAccessLog(size_t maxEntries) const override;

    // -- IDebugBackend: Memory Snapshots (Stage 6.20) ------------------------

    uint32_t createMemorySnapshot(uint16_t start, size_t size) override;
    MemorySnapshotData getMemorySnapshot(uint32_t id) const override;
    MemorySnapshotDiff compareMemorySnapshots(uint32_t idA, uint32_t idB) const override;
    bool deleteMemorySnapshot(uint32_t id) override;
    void invalidateAllSnapshots() override;

    // -- IDebugBackend: symbol commands (Stage 5.3.1) -----------------------

    CommandResult requestCreateFunction(uint16_t addr, const std::string &name) override;
    CommandResult requestRenameSymbol(uint16_t addr, const std::string &name) override;
    CommandResult requestSetComment(uint16_t addr, const std::string &comment) override;
    CommandResult requestRemoveSymbol(uint16_t addr) override;
    CommandResult requestAddLabel(uint16_t addr, const std::string &name) override;

    // -- IDebugBackend: trace execution (Stage 5.3.2 — through queue) -------

    std::future<TraceExecutionResult>
        requestExecuteTrace(const TraceExecutionParams &params) override;

    // -- debug logging (backward compatibility) -----------------------------

    std::function<void(const char *line)> onTrace;

    // -- event callbacks ----------------------------------------------------

    std::function<void(const InstructionEvent &)> onInstruction;

    // -- instrumentation hooks ----------------------------------------------

    void onMemoryRead(uint32_t virt, uint32_t phys, bool stack, uint8_t value,
                      uint16_t pc);
    void onMemoryWrite(uint32_t virt, uint32_t phys, bool stack, uint8_t value);
    void onIoInput(uint8_t port, uint8_t value);
    void onIoOutput(uint8_t port, uint8_t value);

    void setInstrumentationEnabled(bool enabled);
    bool isInstrumentationEnabled() const;

    // -- memory statistics --------------------------------------------------

    std::vector<struct MemoryStats> memoryStatsSnapshot() const;

    // -- Command Queue (Stage 5.3.2) ----------------------------------------

    enum class CommandType {
        // Execution state
        Run, Pause, Step, Reset, Restart, Quit,
        // Machine replacement (ROM load) — runs on the emulation thread
        LoadRom,
        // Memory/Register
        MemoryWrite, RegisterWrite,
        // Breakpoints
        AddBreakpoint, RemoveBreakpoint, SetBreakpointEnabled, ClearBreakpoints,
        // Symbols
        CreateFunction, RenameSymbol, SetComment, RemoveSymbol, AddLabel,
        // Trace
        ExecuteTrace,
        // I/O (Stage 6.1 Iteration 3)
        IoWrite
    };

    // Stage 5.3.3.2: Explicit command lifecycle state machine
    // Transitions: Queued→Executing, Queued→Cancelled, Executing→Completed
    enum class CommandState { Queued, Executing, Cancelled, Completed };

    struct Command {
        CommandType type = CommandType::Quit;
        // Parameters (only relevant fields used per command type)
        uint16_t address = 0;
        std::string name;
        std::string comment;
        bool enabled = false;
        // Memory write
        std::vector<uint8_t> writeData;
        // Register write
        RegisterId regId = RegisterId::AF;
        uint16_t regValue = 0;
        // I/O write (Stage 6.1 Iteration 3)
        uint8_t ioValue = 0;
        // LoadRom: ROM load address (name carries the path)
        uint32_t org = 0;
        // Trace
        TraceExecutionParams traceParams;
        std::shared_ptr<std::promise<TraceExecutionResult>> tracePromise;
        // Stage 5.3.3.1: Command lifecycle state
        std::atomic<CommandState> state{CommandState::Queued};
        // Cancellation (Stage 5.3.3) — only checked while Queued
        std::atomic<bool> cancelled{false};
        // Result delivery
        std::promise<CommandResult> promise;
        // Stage 5.3.3.3: Per-Run pause promise — fulfilled when emulation
        // transitions to Paused after this specific Run command.
        // Uses CommandResult so the future can be returned directly from requestRunFuture().
        std::shared_ptr<std::promise<CommandResult>> pausePromise;
    };

    class CommandQueue {
    public:
        void enqueue(std::unique_ptr<Command> cmd);
        std::unique_ptr<Command> tryDequeue();
        std::unique_ptr<Command> waitAndDequeue();
        bool empty() const;
    private:
        std::queue<std::unique_ptr<Command>> queue_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
    };

    // -- emulation thread API (not part of IDebugBackend) --------------------

    void runUntilPause();
    // Stage 5.3.3.3: Unified command processing — replaces processOneCommand().
    // Returns true if the frame loop should continue, false if it should break.
    bool processCommand(std::unique_ptr<Command> &cmd);
    void fulfillPausePromises_();
    void waitForCompletion();
    bool isQuitRequested() const;

    // Legacy execution (used internally, not via IDebugBackend)
    void run();
    void pause();
    void reset();
    void restart();  // BLK+ВВОД: attach boot ROM, PC=0

    // Helper: dual-mode command submission.
    // If emulation loop is running — enqueue and wait for result.
    // If not (test scenario) — execute directly on calling thread.
    CommandResult submitAndWait(std::unique_ptr<Command> cmd);

    // Stage 5.3.3.2: test-only accessor for emulation thread detection
    bool isEmulationLoopRunning() const { return emulationLoopRunning_.load(std::memory_order_acquire); }

private:
    IDebugTarget *target_;

    DebuggerState state_;

    std::map<int, DebuggerBreakpoint> breakpoints_;
    int nextId_;

    // Stage 5.3.3.1: pauseRequested_ removed — only atomic version remains.
    std::atomic<bool> pauseRequestedAtomic_{false};

    StopReason stopReason_ = StopReason::None;
    bool       skipBreakpoint_ = false;

    // Skip: temporary breakpoint tracking
    uint16_t skipTempBreakpoint_ = 0;
    bool     skipTempBreakpointAdded_ = false;

    bool checkBreakpoint();
    void syncBreakpointsToTarget();

    void formatTraceLine(char *buf, size_t bufsize,
                         uint16_t pc, uint8_t opcode,
                         const uint8_t *operandBytes) const;

    // -- Stage 2 internals --------------------------------------------------

    uint64_t instructionSequence_;
    bool     instrumentationEnabled_;
    int      fetchRemaining_;
    uint16_t fetchBasePc_ = 0;           // pc of the instruction being fetched
    bool     steppingInProgress_ = false;   // step path owns sequence/execute
    bool     freeRunSequenceStarted_ = false;

    struct Impl;
    Impl *impl_;

    void installMemoryCallbacks();

    // -- Execution loops ----------------------------------------------------

    void executeFramesTarget_();
    void executeFramesNoTarget_();

    // -- Frame pacing (50 Hz, matching real Vector-06C refresh rate) --------

    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    TimePoint lastFrameTime_{};

    static constexpr int kTargetFps = 50;
    static constexpr auto kFrameDuration = std::chrono::microseconds(1000000 / kTargetFps);  // 20 ms

    // -- Thread-safe command protocol (Stage 5.3.2 — Command Queue) ---------

    mutable std::mutex      commandMutex_;
    std::condition_variable commandCv_;
    std::atomic<bool>       quitRequested_{false};   // Stage 5.3.3: atomic
    mutable std::mutex      stateMutex_;   // Stage 5.3.3.2: sole protector of state_

    std::atomic<bool>       running_{false};
    std::atomic<bool>       breakRequested_{false};

    // Stage 5.3.3.3: Pending pause promises from Run commands.
    // Each Run command contributes one promise; all are fulfilled when
    // the Emulation Thread transitions to Paused.
    std::mutex pendingPauseMutex_;
    std::vector<std::shared_ptr<std::promise<CommandResult>>> pendingPausePromises_;

    uint16_t    lastPc_        = 0;

    std::map<int, DebuggerBreakpoint>::iterator findBreakpointByAddress(uint16_t address);
    std::map<int, DebuggerBreakpoint>::const_iterator findBreakpointByAddress(uint16_t address) const;

    // -- Symbol database ----------------------------------------------------

    SymbolDatabase symbols_;

    // -- ROM Database (Stage 6.11) ------------------------------------------

    RdbController *rdb_;
    std::string rdbPath_;  // derived path: <rom_path>.rdb

    // Derive .rdb path from ROM path
    static std::string rdbPathFromRom(const std::string &romPath);

    // Load .rdb file if it exists; create empty in-memory RDB otherwise
    void loadRdb(const std::string &romPath);

    // -- Runtime Memory Access Map (Stage 6.20) ------------------------------

    // 256 blocks × 256 bytes covering 64K address space.
    // Accumulated in onMemoryRead/onMemoryWrite (emulation thread only).
    RuntimeAccessBlock runtimeAccessMap_[256];

    // Bounded log of individual memory accesses.
    RingBuffer<RuntimeAccessLogEntry> *runtimeAccessLog_;

    mutable std::mutex runtimeAccessMutex_;

    // Stage 6.25: monotonically increasing sequence number assigned at push
    // time.  Only ever incremented by the emulation thread (onMemoryRead /
    // onMemoryWrite); reset by clearRuntimeAccessMap() and by loadRom().
    // Kept atomic so external readers see a consistent value.
    std::atomic<uint64_t> accessSequence_{0};

    // -- Memory Snapshots (Stage 6.20) ---------------------------------------

    std::map<uint32_t, MemorySnapshotData> snapshots_;
    uint32_t nextSnapshotId_ = 1;
    mutable std::mutex snapshotMutex_;

private:

    // -- Activity counters --------------------------------------------------

    uint64_t executeCount_[65536];

    // -- VRAM write tracking ------------------------------------------------

    VramWriteInfo vramLastWrite_[256];
    mutable std::mutex vramWriteMutex_;

    // -- IO port tracking ---------------------------------------------------

    uint8_t ioPA_ = 0xFF;
    uint8_t ioPB_ = 0xFF;
    mutable std::mutex ioRegMutex_;

    // -- Screen mutex -------------------------------------------------------

    mutable std::mutex screenMutex_;

    // -- Command Queue (Stage 5.3.2) ----------------------------------------

    std::atomic<bool> emulationLoopRunning_{false};
    CommandQueue commandQueue_;
    std::atomic<bool> traceBusy_{false};

    void executeCommand(Command &cmd);
    TraceExecutionResult executeTraceInternal(const TraceExecutionParams &params);

};
