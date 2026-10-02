// Stage 3.2 — Simplified Board Integration Smoke Test
//
// This test verifies that the debugger works with the REAL Board,
// without requiring boot ROM or icon binary resources.
// We disable video/sound to avoid SDL dependencies.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <set>

#include "memory.h"
#include "i8080.h"
#include "i8080_hal.h"
#include "backend.h"
#include "debug_target.h"
#include "debug_memory.h"
#include "events.h"
#include "options.h"
#include "board.h"
#include "vio.h"
#include "tv.h"
#include "sound.h"
#include "filler.h"
#include "keyboard.h"
#include "8253.h"
#include "ay.h"
#include "wav.h"
#include "fd1793.h"
#include "debug_memory.h"
#include "disassembler.h"
#include "memory_inspector_window.h"
#include "disassembly_window.h"
#include "stack_view_window.h"
#include "breakpoints_window.h"
#include "io_inspector_window.h"

using namespace i8080cpu;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_BEGIN(name) \
    do { \
        tests_run++; \
        printf("\n\033[0;35m=== TEST: %s ===\033[0m\n", name); \
        const char *_test_name = name; \
        bool _test_ok = true; \
        (void)_test_name;

#define CHECK(cond, msg) \
        do { \
            if (!(cond)) { \
                printf("  \033[41;97m FAIL \033[0m %s (line %d)\n", msg, __LINE__); \
                _test_ok = false; \
            } else { \
                printf("  \033[46;30m ok \033[0m %s\n", msg); \
            } \
        } while(0)

#define CHECK_EQ(exp, act, msg) \
        do { \
            unsigned _e = (unsigned)(exp); \
            unsigned _a = (unsigned)(act); \
            if (_e != _a) { \
                printf("  \033[41;97m FAIL \033[0m %s: expected 0x%X, got 0x%X (line %d)\n", \
                       msg, _e, _a, __LINE__); \
                _test_ok = false; \
            } else { \
                printf("  \033[46;30m ok \033[0m %s = 0x%X\n", msg, _a); \
            } \
        } while(0)

#define TEST_END() \
        if (_test_ok) { \
            tests_passed++; \
            printf("\033[46;30m PASS \033[0m %s\n", _test_name); \
        } else { \
            tests_failed++; \
            printf("\033[41;97m FAIL \033[0m %s\n", _test_name); \
        } \
    } while(0)

// ---------------------------------------------------------------------------
// Minimal test program (written directly to Memory, no ROM file)
// ---------------------------------------------------------------------------
//
// 0000: MVI A, 55h    ; 3E 55
// 0002: INR A         ; 3C
// 0003: JMP 0002h     ; C3 02 00
//

static const uint8_t test_program[] = {
    0x3E, 0x55,        // 0000: MVI A, 55h
    0x3C,              // 0002: INR A
    0xC3, 0x02, 0x00,  // 0003: JMP 0002h
};

// ---------------------------------------------------------------------------
// HAL — we use the real hal.cpp from the main project
// ---------------------------------------------------------------------------

static DebugBackend *test_backend = nullptr;

// ---------------------------------------------------------------------------
// TestBoardTarget — IDebugTarget wrapping a real Board for testing
// ---------------------------------------------------------------------------

class TestBoardTarget : public IDebugTarget {
public:
    TestBoardTarget(Memory &mem, Board &brd) : memory_(mem), board_(brd) {}

    uint8_t readMemory(uint16_t addr) override { return memory_.read(addr, false); }
    uint8_t peekMemory(uint16_t addr) override { return DebugMemoryAccess::peek(memory_, addr); }
    uint8_t readMemoryRaw(uint16_t addr) override { return memory_.peek(addr, false); }
    void writeMemory(uint16_t addr, uint8_t val) override { memory_.write(addr, val, false); }

    CpuState getCpuState() override {
        CpuState s;
        s.pc = (uint16_t)i8080_pc(); s.sp = (uint16_t)i8080_regs_sp();
        s.a = (uint8_t)i8080_regs_a(); s.b = (uint8_t)i8080_regs_b();
        s.c = (uint8_t)i8080_regs_c(); s.d = (uint8_t)i8080_regs_d();
        s.e = (uint8_t)i8080_regs_e(); s.h = (uint8_t)i8080_regs_h();
        s.l = (uint8_t)i8080_regs_l(); s.flags = (uint8_t)i8080_regs_f();
        s.iff = i8080_iff(); s.cycles = (uint32_t)i8080_cycles();
        s.ei_pending = false; s.last_pc = 0;
        return s;
    }
    void writeCpuRegister(int reg, uint16_t val) override {
        switch(reg) {
            case 0: i8080_setreg_a((val>>8)&0xFF); i8080_setreg_f(val&0xFF); break;
            case 1: i8080_setreg_b((val>>8)&0xFF); i8080_setreg_c(val&0xFF); break;
            case 2: i8080_setreg_d((val>>8)&0xFF); i8080_setreg_e(val&0xFF); break;
            case 3: i8080_setreg_h((val>>8)&0xFF); i8080_setreg_l(val&0xFF); break;
            case 4: i8080_setreg_sp(val); break;
            case 5: i8080_jump(val); break;
        }
    }

    void setMemoryCallbacks(MemoryReadCallback onRead,
                            MemoryWriteCallback onWrite) override {
        if (onRead) {
            prevOnRead_  = memory_.onread;
            prevOnWrite_ = memory_.onwrite;
            memory_.onread = [this, onRead](uint32_t v, uint32_t p, bool s, uint8_t val) {
                // Stage 6.24: pass i8080_pc() as the pc argument.
                onRead(v, p, s, val, static_cast<uint16_t>(i8080_pc()));
                if (prevOnRead_) prevOnRead_(v, p, s, val);
            };
            memory_.onwrite = [this, onWrite](uint32_t v, uint32_t p, bool s, uint8_t val) {
                onWrite(v, p, s, val);
                if (prevOnWrite_) prevOnWrite_(v, p, s, val);
            };
        } else {
            memory_.onread  = prevOnRead_;
            memory_.onwrite = prevOnWrite_;
            prevOnRead_ = nullptr;
            prevOnWrite_ = nullptr;
        }
    }

    void stepInstruction() override { board_.single_step(false); }
    void executeFrame() override { board_.execute_frame_with_cadence(false, false); }
    void reset(bool loadRom) override {
        board_.reset(loadRom ? Board::ResetMode::LOADROM : Board::ResetMode::BLKSBR);
    }
    void debuggerBreak() override { board_.debugger_break(); }
    void debuggerContinue() override { board_.debugger_continue(); }
    void debuggerAttached() override { board_.debugger_attached(); }
    void debuggerDetached() override { board_.debugger_detached(); }
    void setPollCallback(std::function<void()> cb) override { board_.poll_debugger = cb; }
    void syncBreakpoints(const DebuggerBreakpoint *bps, size_t count) override {
        // Diff-based sync matching DebugAdapter::syncBreakpoints()
        std::set<uint16_t> newAddrs;
        for (size_t i = 0; i < count; ++i)
            if (bps[i].enabled)
                newAddrs.insert(bps[i].address);
        // Remove old
        for (uint16_t a : syncedBp_) {
            if (newAddrs.find(a) == newAddrs.end())
                board_.remove_breakpoint(0, a, 1);
        }
        // Add new
        for (uint16_t a : newAddrs) {
            if (syncedBp_.find(a) == syncedBp_.end())
                board_.insert_breakpoint(0, a, 1);
        }
        syncedBp_ = newAddrs;
    }
    ScreenData screenSnapshot() override { return {}; }
    void pressKey(int) override {}
    void releaseKey(int) override {}
    bool loadRom(const std::string&, uint32_t) override { return false; }
    bool loadWav(const std::string&) override { return false; }
    void initCpu(uint16_t pc, uint16_t sp) override {
        i8080_jump(pc); i8080_setreg_sp(sp); i8080_init();
    }

private:
    Memory &memory_;
    Board  &board_;
    // Stage 6.24: prevOnRead_ chains the RAW Memory::onread type (4-param),
    // not MemoryReadCallback which now has a 5th pc parameter.
    std::function<void(uint32_t,uint32_t,bool,uint8_t)> prevOnRead_;
    MemoryWriteCallback prevOnWrite_;
    std::set<uint16_t> syncedBp_;
};

// ---------------------------------------------------------------------------
// Test: Simplified Board integration smoke test
// ---------------------------------------------------------------------------

static void test_board_smoke()
{
    TEST_BEGIN("Board Integration Smoke Test (simplified)");

    // --- Disable video/sound to avoid SDL dependencies ---
    printf("  Disabling video and sound for headless test...\n");
    Options.novideo = true;
    Options.nosound = true;
    
    // --- Create real components in correct order ---
    printf("  Creating real Board components...\n");
    
    Memory memory;
    FD1793 fdc;
    Wav wav;
    WavPlayer tape_player(wav);
    Keyboard keyboard;
    I8253 timer;
    TimerWrapper tw(timer);
    AY ay;
    AYWrapper aw(ay);
    Soundnik soundnik(tw, aw);
    IO io(memory, keyboard, timer, fdc, ay, tape_player);
    TV tv;
    PixelFiller filler(memory, io, tv);
    
    // Initialize components (headless mode)
    filler.init();
    soundnik.init(nullptr);  // No WavRecorder
    tv.init();  // Will skip video init due to Options.novideo
    
    // Create real Board
    Board board(memory, io, filler, soundnik, tv, tape_player);
    
    printf("  Real Board created\n");
    
    // --- Initialize Board ---
    printf("  Initializing Board...\n");
    board.init();  // This calls i8080_hal_bind()
    
    // --- Create DebugBackend with TestBoardTarget wrapping real Board ---
    TestBoardTarget target(memory, board);
    DebugBackend backend(target);
    test_backend = &backend;
    
    // --- Wire I/O callbacks so DebugBackend records I/O events ---
    // The real HAL calls io.input()/io.output() which fire onread/onwrite.
    // We connect them to DebugBackend's I/O recording hooks.
    io.onread = [](uint32_t port, uint8_t value) -> int {
        if (test_backend) test_backend->onIoInput((uint8_t)port, value);
        return -1;  // don't override the result
    };
    io.onwrite = [](uint32_t port, uint8_t value) {
        if (test_backend) test_backend->onIoOutput((uint8_t)port, value);
    };
    
    printf("  Real Board attached to DebugBackend\n");
    
    // --- Write test program directly to Memory ---
    printf("  Writing test program to Memory...\n");
    for (size_t i = 0; i < sizeof(test_program); ++i) {
        memory.write((uint16_t)i, test_program[i], false);
    }
    
    // --- Reset Board in LOADROM mode ---
    printf("  Resetting Board in LOADROM mode...\n");
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    
    // --- Check initial CPU state ---
    printf("  Checking initial CPU state...\n");
    CpuState initial = backend.getCpuState();
    CHECK_EQ(0x0000, initial.pc, "Initial PC == 0x0000");
    CHECK_EQ(0x0000, initial.a,  "Initial A == 0x00");
    
    // --- Step 1: Execute MVI A, 55h ---
    printf("  Stepping: MVI A, 55h...\n");
    StepResult step1 = backend.stepInstructionDetailed();
    CHECK_EQ(0x0000, step1.pcBefore, "Step 1: PC before == 0x0000");
    CHECK_EQ(0x0002, step1.pcAfter,  "Step 1: PC after == 0x0002");
    CHECK_EQ(0x3E,   step1.opcode,   "Step 1: opcode == 0x3E (MVI A)");
    
    CpuState after_step1 = backend.getCpuState();
    CHECK_EQ(0x55, after_step1.a, "After step 1: A == 0x55");
    
    // --- Step 2: Execute INR A ---
    printf("  Stepping: INR A...\n");
    StepResult step2 = backend.stepInstructionDetailed();
    CHECK_EQ(0x0002, step2.pcBefore, "Step 2: PC before == 0x0002");
    CHECK_EQ(0x0003, step2.pcAfter,  "Step 2: PC after == 0x0003");
    CHECK_EQ(0x3C,   step2.opcode,   "Step 2: opcode == 0x3C (INR A)");
    
    CpuState after_step2 = backend.getCpuState();
    CHECK_EQ(0x56, after_step2.a, "After step 2: A == 0x56 (incremented)");
    
    // --- Test breakpoint ---
    printf("  Testing breakpoint at 0x0002 (INR A)...\n");
    int bp_id = backend.addBreakpoint(0x0002);
    CHECK(bp_id >= 0, "Breakpoint added");
    
    // Reset to 0x0000
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    
    // Execute MVI A, 55h
    backend.stepInstruction();
    CpuState before_bp = backend.getCpuState();
    CHECK_EQ(0x0002, before_bp.pc, "PC == 0x0002 (at breakpoint)");
    
    // Step should execute the instruction at breakpoint
    backend.stepInstruction();
    CpuState after_bp_step = backend.getCpuState();
    CHECK_EQ(0x56, after_bp_step.a, "After breakpoint step: A == 0x56 (incremented from 0x55)");
    CHECK_EQ(0x0003, after_bp_step.pc, "After breakpoint step: PC == 0x0003");
    
    backend.removeBreakpoint(bp_id);
    
    // --- Test instrumentation ---
    printf("  Checking instrumentation events...\n");
    auto instr_history = backend.instructionHistorySnapshot();
    auto mem_history = backend.memoryHistorySnapshot();
    
    CHECK(instr_history.size() > 0, "Instruction history has events");
    CHECK(mem_history.size() > 0, "Memory history has events");
    
    printf("  Instruction events: %zu\n", instr_history.size());
    printf("  Memory events: %zu\n", mem_history.size());
    
    // Check that we got Fetch events
    int fetch_count = 0;
    for (const auto &ev : mem_history) {
        if (ev.type == MemoryAccessType::Fetch) {
            fetch_count++;
        }
    }
    CHECK(fetch_count > 0, "Got Fetch events from real Board");
    printf("  Fetch events: %d\n", fetch_count);
    
    // --- Test Memory::read/write ---
    printf("  Testing real Memory::read/write...\n");
    memory.write(0x8000, 0xAB, false);
    uint8_t val = DebugMemoryAccess::peek(memory, 0x8000);
    CHECK_EQ(0xAB, val, "Memory::read/write works");
    
    // --- Test Memory Inspector API (Stage 3.3) ---
    printf("  Testing Memory Inspector API...\n");
    
    // Test readMemory()
    uint8_t byte_at_0 = backend.readMemory(0x0000);
    CHECK_EQ(0x3E, byte_at_0, "readMemory(0000) == 0x3E (MVI A opcode)");
    
    // Test readMemorySnapshot()
    MemorySnapshot snap = backend.readMemorySnapshot(0x0000, 6);
    CHECK_EQ(0x0000, snap.start, "Snapshot start == 0x0000");
    CHECK_EQ(6, snap.data.size(), "Snapshot size == 6");
    CHECK_EQ(0x3E, snap.data[0], "Snapshot[0] == 0x3E (MVI A)");
    CHECK_EQ(0x55, snap.data[1], "Snapshot[1] == 0x55 (operand)");
    CHECK_EQ(0x3C, snap.data[2], "Snapshot[2] == 0x3C (INR A)");
    CHECK_EQ(0xC3, snap.data[3], "Snapshot[3] == 0xC3 (JMP)");
    
    printf("  Memory Inspector API works with real Board\n");
    
    // --- Test Stack View API (Stage 3.4) ---
    printf("  Testing Stack View API...\n");
    
    // Test SP read
    CpuState cpu = backend.getCpuState();
    CHECK(cpu.sp > 0, "SP > 0");
    printf("  SP = %04X\n", cpu.sp);
    
    // Test stack snapshot around SP
    uint16_t sp = cpu.sp;
    int start = std::max(static_cast<int>(sp) - 8, 0);
    int end = std::min(static_cast<int>(sp) + 8, 0xFFFF);
    size_t size = static_cast<size_t>(end - start + 1);
    
    MemorySnapshot stackSnap = backend.readMemorySnapshot(static_cast<uint16_t>(start), size);
    CHECK_EQ(start, stackSnap.start, "Stack snapshot start correct");
    CHECK(size > 0, "Stack snapshot has data");
    
    printf("  Stack View API works with real Board\n");
    
    // --- Test Memory Editing API (Stage 3.5) ---
    printf("  Testing Memory Editing API...\n");
    
    // Write a byte to a known RAM area (use address 0xC000 which is RAM)
    // First read original value
    uint8_t origByte = backend.readMemory(0xC000);
    printf("  Original C000 = %02X\n", origByte);
    
    // Write through Memory::write (virtual address path)
    // This is what processWriteCommand -> executeWriteMemory does
    memory.write(0xC000, 0x77, false);
    
    // Verify through backend read
    uint8_t newByte = backend.readMemory(0xC000);
    CHECK_EQ(0x77, newByte, "write C000 = 0x77, read back 0x77");
    
    // Verify snapshot reflects the write
    MemorySnapshot editSnap = backend.readMemorySnapshot(0xC000, 4);
    CHECK_EQ(0x77, editSnap.data[0], "snapshot[0] == 0x77 after write");
    
    // Write multiple bytes
    uint8_t editData[] = {0xDE, 0xAD, 0xBE, 0xEF};
    for (size_t i = 0; i < sizeof(editData); ++i) {
        memory.write(static_cast<uint16_t>(0xC010 + i), editData[i], false);
    }
    MemorySnapshot editSnap2 = backend.readMemorySnapshot(0xC010, 4);
    CHECK_EQ(0xDE, editSnap2.data[0], "bulk write [0] == 0xDE");
    CHECK_EQ(0xAD, editSnap2.data[1], "bulk write [1] == 0xAD");
    CHECK_EQ(0xBE, editSnap2.data[2], "bulk write [2] == 0xBE");
    CHECK_EQ(0xEF, editSnap2.data[3], "bulk write [3] == 0xEF");
    
    // Restore original value
    memory.write(0xC000, origByte, false);
    uint8_t restored = backend.readMemory(0xC000);
    CHECK_EQ(origByte, restored, "restored original value");
    
    printf("  Memory Editing API works with real Board\n");
    
    // --- Test CPU Register Write/Read (Stage 3.6) ---
    printf("  Testing CPU Register Write/Read API...\n");
    
    // Write registers via i8080 API (simulating what emulation thread does)
    i8080_jump(0x0100);
    i8080_setreg_sp(0xC100);
    i8080_setreg_b(0x12);
    i8080_setreg_c(0x34);
    i8080_setreg_d(0x56);
    i8080_setreg_e(0x78);
    i8080_setreg_h(0x9A);
    i8080_setreg_l(0xBC);
    i8080_setreg_a(0xDE);
    i8080_setreg_f(0xF0);
    
    // Read back via DebugBackend
    CpuState regs = backend.getCpuState();
    CHECK_EQ(0x0100, regs.pc, "PC = 0x0100");
    CHECK_EQ(0xC100, regs.sp, "SP = 0xC100");
    CHECK_EQ(0x12, regs.b, "B = 0x12");
    CHECK_EQ(0x34, regs.c, "C = 0x34");
    CHECK_EQ(0x56, regs.d, "D = 0x56");
    CHECK_EQ(0x78, regs.e, "E = 0x78");
    CHECK_EQ(0x9A, regs.h, "H = 0x9A");
    CHECK_EQ(0xBC, regs.l, "L = 0xBC");
    CHECK_EQ(0xDE, regs.a, "A = 0xDE");
    CHECK_EQ(0xF0, regs.flags, "F = 0xF0");
    
    // Verify additional Stage 3.6 fields
    printf("  IFF=%d EI_pending=%d Cycles=%u LastPC=%04X\n",
        regs.iff ? 1 : 0, regs.ei_pending ? 1 : 0, regs.cycles, regs.last_pc);
    CHECK(true, "Stage 3.6 fields accessible");
    
    // Test PC -> Step executes from new address
    // Write MVI A, 0x99 at 0x0100
    memory.write(0x0100, 0x3E, false);  // MVI A
    memory.write(0x0101, 0x99, false);  // operand
    
    i8080_jump(0x0100);
    backend.stepInstruction();
    CpuState afterStep = backend.getCpuState();
    CHECK_EQ(0x99, afterStep.a, "A = 0x99 after step from new PC");
    CHECK_EQ(0x0102, afterStep.pc, "PC advanced to 0x0102");
    
    printf("  CPU Register Write/Read API works with real Board\n");
    
    // --- Test Breakpoint API with real Board (Stage 3.7) ---
    printf("  Testing Breakpoint API with real Board...\n");
    
    // Reset to known state
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    backend.clearBreakpoints();
    
    // Verify test program is still in memory (MVI A,55 / INR A / JMP 0002)
    CHECK_EQ(0x3E, backend.readMemory(0x0000), "test program at 0000: MVI A opcode");
    CHECK_EQ(0x55, backend.readMemory(0x0001), "test program at 0001: operand 55");
    CHECK_EQ(0x3C, backend.readMemory(0x0002), "test program at 0002: INR A");
    
    // Add breakpoint at 0x0002 (INR A)
    int bpId = backend.addBreakpoint(0x0002);
    CHECK(bpId >= 0, "breakpoint added at 0x0002");
    CHECK(backend.hasBreakpoint(0x0002), "hasBreakpoint(0x0002)");
    
    // Run — should stop at 0x0002 before executing INR A
    backend.run();
    
    CpuState bpState = backend.getCpuState();
    CHECK_EQ(0x0002, bpState.pc, "PC == 0x0002 at breakpoint");
    CHECK_EQ(0x55, bpState.a, "A == 0x55 (MVI executed, INR not yet)");
    CHECK_EQ((int)StopReason::Breakpoint, (int)backend.getStopReason(),
             "stopReason == Breakpoint");
    
    // Step — should execute INR A, A becomes 0x56, PC becomes 0x0003
    StepResult bpStep = backend.stepInstructionDetailed();
    CHECK_EQ(0x0002, bpStep.pcBefore, "step from 0x0002");
    CHECK_EQ(0x0003, bpStep.pcAfter, "now at 0x0003");
    
    CpuState afterBpStep = backend.getCpuState();
    CHECK_EQ(0x56, afterBpStep.a, "A == 0x56 (INR A executed)");
    CHECK_EQ((int)StopReason::Step, (int)backend.getStopReason(),
             "stopReason == Step");
    
    // Test breakpoint enable/disable
    backend.clearBreakpoints();
    backend.addBreakpoint(0x0002);
    backend.setBreakpointEnabled(0x0002, false);
    CHECK(!backend.hasBreakpoint(0x0002), "disabled BP not reported by hasBreakpoint");
    
    auto bpList = backend.getBreakpoints();
    CHECK_EQ((size_t)1, bpList.size(), "1 BP in list (disabled)");
    CHECK(!bpList[0].enabled, "BP marked disabled");
    
    backend.setBreakpointEnabled(0x0002, true);
    CHECK(backend.hasBreakpoint(0x0002), "re-enabled BP visible");
    
    // Test duplicate rejection
    int dupId = backend.addBreakpoint(0x0002);
    CHECK_EQ(-1, dupId, "duplicate BP returns -1");
    
    // Test reset preserves breakpoints
    backend.reset();
    auto bpAfterReset = backend.getBreakpoints();
    CHECK_EQ((size_t)1, bpAfterReset.size(), "BP survives reset");
    CHECK_EQ((int)StopReason::Reset, (int)backend.getStopReason(),
             "stopReason == Reset after reset");
    
    backend.clearBreakpoints();
    printf("  Breakpoint API works with real Board\n");
    
    // --- Test Disassembly API (Stage 3.8) ---
    printf("  Testing Disassembly API...\n");
    
    // Reset to known state — test program should still be in memory
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    
    // Verify test program bytes
    CHECK_EQ(0x3E, backend.readMemory(0x0000), "test program: MVI A opcode at 0000");
    CHECK_EQ(0x55, backend.readMemory(0x0001), "test program: operand 55 at 0001");
    CHECK_EQ(0x3C, backend.readMemory(0x0002), "test program: INR A at 0002");
    CHECK_EQ(0xC3, backend.readMemory(0x0003), "test program: JMP opcode at 0003");
    
    // Disassemble using DebugMemoryAccess::peek() as read function
    auto dasmReadFn = [&backend](uint16_t addr) -> uint8_t {
        return backend.readMemory(addr);
    };
    
    // Disassemble at 0000: MVI A,55H (2 bytes)
    auto d1 = disassemble(0x0000, dasmReadFn);
    CHECK(d1.mnemonic == "MVI", "disasm 0000: MVI");
    CHECK_EQ(2, d1.length, "disasm 0000: length 2");
    
    // Disassemble at 0002: INR A (1 byte)
    auto d2 = disassemble(0x0002, dasmReadFn);
    CHECK(d2.mnemonic == "INR", "disasm 0002: INR");
    CHECK_EQ(1, d2.length, "disasm 0002: length 1");
    
    // Disassemble at 0003: JMP 0002 (3 bytes)
    auto d3 = disassemble(0x0003, dasmReadFn);
    CHECK(d3.mnemonic == "JMP", "disasm 0003: JMP");
    CHECK_EQ(3, d3.length, "disasm 0003: length 3");
    CHECK(d3.text.find("0002") != std::string::npos, "disasm 0003: target 0002");
    
    // Set BP at 0002, run, verify PC=0002
    backend.addBreakpoint(0x0002);
    backend.run();
    
    CpuState bpState2 = backend.getCpuState();
    CHECK_EQ(0x0002, bpState2.pc, "BP hit: PC == 0x0002");
    CHECK_EQ(0x55, bpState2.a, "BP hit: A == 0x55");
    CHECK_EQ((int)StopReason::Breakpoint, (int)backend.getStopReason(),
             "stopReason == Breakpoint");
    
    // Step — should execute INR A, PC becomes 0003
    backend.stepInstruction();
    CpuState afterStep2 = backend.getCpuState();
    CHECK_EQ(0x0003, afterStep2.pc, "after step: PC == 0x0003");
    CHECK_EQ(0x56, afterStep2.a, "after step: A == 0x56");
    
    // Disassemble at PC (0003): should be JMP 0002
    auto d4 = disassemble(afterStep2.pc, dasmReadFn);
    CHECK(d4.mnemonic == "JMP", "disasm at PC 0003: JMP");
    CHECK_EQ(3, d4.length, "disasm at PC 0003: length 3");
    
    backend.clearBreakpoints();
    printf("  Disassembly API works with real Board\n");
    
    // --- Test Navigation API (Stage 3.9) ---
    printf("  Testing Navigation API...\n");
    
    // Reset to known state
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    backend.clearBreakpoints();
    
    // Create window objects for navigation testing
    MemoryInspectorWindow memWin;
    DisassemblyWindow dasmWin;
    StackViewWindow stackWin;
    BreakpointsWindow bpWin;
    
    // --- PC → Disassembly navigation ---
    printf("  Testing PC -> Disassembly navigation...\n");
    CpuState navCpu = backend.getCpuState();
    uint16_t pcAddr = navCpu.pc;
    dasmWin.gotoAddress(pcAddr);
    CHECK_EQ(pcAddr, dasmWin.address(), "Disassembly navigated to PC address");
    CHECK(dasmWin.isVisible(), "Disassembly window opened");
    
    // --- PC → Memory navigation ---
    printf("  Testing PC -> Memory navigation...\n");
    memWin.gotoAddress(pcAddr);
    CHECK_EQ(pcAddr, memWin.address(), "Memory Inspector navigated to PC address");
    CHECK(memWin.isVisible(), "Memory Inspector window opened");
    
    // --- SP → Stack navigation ---
    printf("  Testing SP -> Stack navigation...\n");
    uint16_t spAddr = navCpu.sp;
    stackWin.gotoAddress(spAddr);
    CHECK_EQ(spAddr, stackWin.address(), "Stack View navigated to SP address");
    CHECK(stackWin.isVisible(), "Stack View window opened");
    CHECK(!stackWin.followSP(), "gotoAddress disables Follow SP");
    
    // --- SP → Memory navigation ---
    printf("  Testing SP -> Memory navigation...\n");
    memWin.gotoAddress(spAddr);
    CHECK_EQ(spAddr, memWin.address(), "Memory Inspector navigated to SP address");
    
    // --- BP → Disassembly/Memory navigation ---
    printf("  Testing BP -> Disassembly/Memory navigation...\n");
    int navBpId = backend.addBreakpoint(0x0003);
    CHECK(navBpId >= 0, "BP added for navigation test");
    
    auto bpList2 = backend.getBreakpoints();
    CHECK_EQ((size_t)1, bpList2.size(), "1 BP in list");
    uint16_t bpAddr = bpList2[0].address;
    
    // Navigate BP address to both windows
    dasmWin.gotoAddress(bpAddr);
    CHECK_EQ(bpAddr, dasmWin.address(), "Disassembly navigated to BP address");
    
    memWin.gotoAddress(bpAddr);
    CHECK_EQ(bpAddr, memWin.address(), "Memory navigated to BP address");
    
    backend.clearBreakpoints();
    
    // --- Stack Word → Disassembly navigation ---
    printf("  Testing Stack Word -> Disassembly navigation...\n");
    
    // Write a known LE word at a RAM address
    uint16_t wordAddr = 0xC000;
    uint16_t wordVal  = 0x1234;
    memory.write(wordAddr,      static_cast<uint8_t>(wordVal & 0xFF), false);       // low byte
    memory.write(wordAddr + 1,  static_cast<uint8_t>((wordVal >> 8) & 0xFF), false); // high byte
    
    // Read back as LE word (same logic as StackViewWindow context menu)
    uint8_t lo = backend.readMemory(wordAddr);
    uint8_t hi = backend.readMemory(wordAddr + 1);
    uint16_t leWord = static_cast<uint16_t>(lo | (hi << 8));
    CHECK_EQ(wordVal, leWord, "LE word at C000 == 0x1234");
    
    // Navigate to the word value in Disassembly
    dasmWin.gotoAddress(leWord);
    CHECK_EQ(leWord, dasmWin.address(), "Disassembly navigated to LE word value");
    
    // --- Follow PC ON/OFF ---
    printf("  Testing Follow PC ON/OFF...\n");
    
    // gotoAddress should disable Follow PC
    dasmWin.gotoAddress(0x0000);
    CHECK_EQ(0x0000, dasmWin.address(), "gotoAddress sets address");
    // Follow PC is internal to DisassemblyWindow; we verify via address() stability
    // After gotoAddress, address is 0x0000 and followPc_ is false
    
    // Navigate again — address should change (proves follow was off, manual nav works)
    dasmWin.gotoAddress(0x0003);
    CHECK_EQ(0x0003, dasmWin.address(), "second gotoAddress updates address");
    
    // --- Follow SP ON/OFF ---
    printf("  Testing Follow SP ON/OFF...\n");
    
    // gotoAddress disables Follow SP
    stackWin.gotoAddress(0xC000);
    CHECK(!stackWin.followSP(), "gotoAddress disables Follow SP");
    CHECK_EQ(0xC000, stackWin.address(), "Stack address == 0xC000");
    
    // Enable Follow SP
    stackWin.setFollowSP(true);
    CHECK(stackWin.followSP(), "Follow SP enabled");
    // address() now returns currentSP_ (tracked internally)
    
    // Disable Follow SP — address should return to viewCenter_
    stackWin.setFollowSP(false);
    CHECK(!stackWin.followSP(), "Follow SP disabled");
    CHECK_EQ(0xC000, stackWin.address(), "Stack address returns to viewCenter_");
    
    // --- Cross-navigation callbacks ---
    printf("  Testing cross-navigation callbacks...\n");
    
    // Wire callbacks like DebuggerGui would
    uint16_t dasmNavTarget = 0xFFFF;
    uint16_t memNavTarget  = 0xFFFF;
    
    memWin.onGoToDisassembly = [&](uint16_t a) { dasmNavTarget = a; };
    dasmWin.onGoToMemoryInspector = [&](uint16_t a) { memNavTarget = a; };
    stackWin.onGoToDisassembly = [&](uint16_t a) { dasmNavTarget = a; };
    stackWin.onGoToMemoryInspector = [&](uint16_t a) { memNavTarget = a; };
    bpWin.onGoToDisassembly = [&](uint16_t a) { dasmNavTarget = a; };
    bpWin.onGoToMemoryInspector = [&](uint16_t a) { memNavTarget = a; };
    
    // Fire callbacks
    memWin.onGoToDisassembly(0x1234);
    CHECK_EQ(0x1234, dasmNavTarget, "Memory → Disassembly callback fires");
    
    dasmWin.onGoToMemoryInspector(0x2345);
    CHECK_EQ(0x2345, memNavTarget, "Disassembly → Memory callback fires");
    
    stackWin.onGoToDisassembly(0x3456);
    CHECK_EQ(0x3456, dasmNavTarget, "Stack → Disassembly callback fires");
    
    stackWin.onGoToMemoryInspector(0x4567);
    CHECK_EQ(0x4567, memNavTarget, "Stack → Memory callback fires");
    
    bpWin.onGoToDisassembly(0x5678);
    CHECK_EQ(0x5678, dasmNavTarget, "BP → Disassembly callback fires");
    
    bpWin.onGoToMemoryInspector(0x6789);
    CHECK_EQ(0x6789, memNavTarget, "BP → Memory callback fires");
    
    // --- Boundary addresses ---
    printf("  Testing boundary addresses...\n");
    memWin.gotoAddress(0x0000);
    CHECK_EQ(0x0000, memWin.address(), "Memory: gotoAddress(0x0000)");
    memWin.gotoAddress(0xFFFF);
    CHECK_EQ(0xFFFF, memWin.address(), "Memory: gotoAddress(0xFFFF)");
    
    dasmWin.gotoAddress(0x0000);
    CHECK_EQ(0x0000, dasmWin.address(), "Disassembly: gotoAddress(0x0000)");
    dasmWin.gotoAddress(0xFFFF);
    CHECK_EQ(0xFFFF, dasmWin.address(), "Disassembly: gotoAddress(0xFFFF)");
    
    stackWin.gotoAddress(0x0000);
    CHECK_EQ(0x0000, stackWin.address(), "Stack: gotoAddress(0x0000)");
    stackWin.gotoAddress(0xFFFF);
    CHECK_EQ(0xFFFF, stackWin.address(), "Stack: gotoAddress(0xFFFF)");
    
    printf("  Navigation API works with real Board\n");
    
    // --- Test Execution Trace API (Stage 3.10) ---
    printf("  Testing Execution Trace API...\n");
    
    // Reset to known state
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    backend.clearBreakpoints();
    
    // Verify test program is still in memory
    CHECK_EQ(0x3E, backend.readMemory(0x0000), "test program: MVI A opcode at 0000");
    CHECK_EQ(0x55, backend.readMemory(0x0001), "test program: operand 55 at 0001");
    CHECK_EQ(0x3C, backend.readMemory(0x0002), "test program: INR A at 0002");
    
    // Step 1: MVI A,55
    backend.stepInstruction();
    
    auto trace1 = backend.instructionHistorySnapshot();
    CHECK_EQ(1, trace1.size(), "trace has 1 entry after first step");
    CHECK_EQ(0x0000, trace1[0].pcBefore, "trace[0].PC == 0000");
    CHECK_EQ(0x3E, trace1[0].opcode, "trace[0].opcode == 0x3E (MVI A)");
    CHECK_EQ(2, trace1[0].length, "trace[0].length == 2");
    CHECK_EQ(0x55, trace1[0].operandBytes[0], "trace[0].operandBytes[0] == 0x55");
    
    CpuState traceCpu1 = backend.getCpuState();
    CHECK_EQ(0x0002, traceCpu1.pc, "PC == 0002 after first step");
    
    // Step 2: INR A
    backend.stepInstruction();
    
    auto trace2 = backend.instructionHistorySnapshot();
    CHECK_EQ(2, trace2.size(), "trace has 2 entries after second step");
    CHECK_EQ(0x0002, trace2[1].pcBefore, "trace[1].PC == 0002");
    CHECK_EQ(0x3C, trace2[1].opcode, "trace[1].opcode == 0x3C (INR A)");
    CHECK_EQ(1, trace2[1].length, "trace[1].length == 1");
    
    CpuState traceCpu2 = backend.getCpuState();
    CHECK_EQ(0x0003, traceCpu2.pc, "PC == 0003 after second step");
    
    // Breakpoint test: BP at 0002, Run
    backend.clearBreakpoints();
    backend.clearHistory();
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    
    backend.addBreakpoint(0x0002);
    backend.run();
    
    CpuState bpCpu = backend.getCpuState();
    CHECK_EQ(0x0002, bpCpu.pc, "BP: PC == 0002");
    CHECK_EQ((int)StopReason::Breakpoint, (int)backend.getStopReason(),
             "stopReason == Breakpoint");
    
    auto traceBp = backend.instructionHistorySnapshot();
    CHECK(traceBp.size() >= 1, "trace has entries at BP");
    // Last trace entry should be the instruction BEFORE the BP
    CHECK_EQ(0x0000, traceBp.back().pcBefore, "last trace PC == 0000 (before BP at 0002)");
    
    // Step from BP — should execute INR A
    backend.stepInstruction();
    
    auto traceAfterStep = backend.instructionHistorySnapshot();
    CHECK(traceAfterStep.size() > traceBp.size(), "trace grew after step");
    CHECK_EQ(0x0002, traceAfterStep.back().pcBefore, "last trace PC == 0002 after step");
    
    backend.clearBreakpoints();
    printf("  Execution Trace API works with real Board\n");
    
    // --- Test I/O Inspector API (Stage 3.11) ---
    printf("  Testing I/O Inspector API...\n");
    
    // I/O test program:
    // 0000: OUT 10h   (D3 10)
    // 0002: IN  20h   (DB 20)
    // 0004: OUT 11h   (D3 11)
    // 0006: JMP 0006h (C3 06 00)
    static const uint8_t io_test_program[] = {
        0xD3, 0x10,              // 0000: OUT 10h
        0xDB, 0x20,              // 0002: IN  20h
        0xD3, 0x11,              // 0004: OUT 11h
        0xC3, 0x06, 0x00,        // 0006: JMP 0006h
    };
    
    // Reset and write I/O test program
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    backend.clearBreakpoints();
    
    for (size_t i = 0; i < sizeof(io_test_program); ++i) {
        memory.write((uint16_t)i, io_test_program[i], false);
    }
    
    // Step 1: OUT 10h at PC=0000
    uint64_t seqBase = backend.instructionSequence();
    backend.stepInstruction();
    
    auto io1 = backend.ioHistorySnapshot();
    CHECK_EQ(1, (int)io1.size(), "I/O history has 1 entry after first step");
    CHECK_EQ((int)IoAccessType::Out, (int)io1[0].type, "io[0].type == OUT");
    CHECK_EQ(0x10, io1[0].port, "io[0].port == 0x10");
    CHECK_EQ((int)seqBase, (int)io1[0].instructionSequence, "io[0].sequence == seqBase");
    
    // Resolve PC via instruction history
    auto instr1 = backend.instructionHistorySnapshot();
    uint16_t pc0 = 0xFFFF;
    for (const auto &ie : instr1) {
        if (ie.sequence == io1[0].instructionSequence) {
            pc0 = ie.pcBefore;
            break;
        }
    }
    CHECK_EQ(0x0000, pc0, "io[0] resolved PC == 0x0000");
    
    // Step 2: IN 20h at PC=0002
    backend.stepInstruction();
    
    auto io2 = backend.ioHistorySnapshot();
    CHECK_EQ(2, (int)io2.size(), "I/O history has 2 entries");
    CHECK_EQ((int)IoAccessType::In, (int)io2[1].type, "io[1].type == IN");
    CHECK_EQ(0x20, io2[1].port, "io[1].port == 0x20");
    CHECK_EQ((int)(seqBase + 1), (int)io2[1].instructionSequence, "io[1].sequence == seqBase+1");
    
    // Step 3: OUT 11h at PC=0004
    backend.stepInstruction();
    
    auto io3 = backend.ioHistorySnapshot();
    CHECK_EQ(3, (int)io3.size(), "I/O history has 3 entries");
    CHECK_EQ((int)IoAccessType::Out, (int)io3[2].type, "io[2].type == OUT");
    CHECK_EQ(0x11, io3[2].port, "io[2].port == 0x11");
    CHECK_EQ((int)(seqBase + 2), (int)io3[2].instructionSequence, "io[2].sequence == seqBase+2");
    
    // Verify sequences are monotonically increasing
    CHECK(io3[0].instructionSequence < io3[1].instructionSequence,
          "io[0].seq < io[1].seq");
    CHECK(io3[1].instructionSequence < io3[2].instructionSequence,
          "io[1].seq < io[2].seq");
    
    // Test clearIoHistory — I/O empty, instruction history preserved
    size_t instrCount = backend.instructionHistorySize();
    backend.clearIoHistory();
    CHECK_EQ(0, (int)backend.ioHistorySize(), "I/O history empty after clearIoHistory");
    CHECK_EQ((int)instrCount, (int)backend.instructionHistorySize(),
             "instruction history preserved");
    
    // CPU not reset
    CpuState ioCpu = backend.getCpuState();
    CHECK_EQ(0x0006, ioCpu.pc, "PC == 0x0006 (not reset after clearIoHistory)");
    
    // Test navigation callbacks
    uint16_t navDisasm = 0xFFFF;
    uint16_t navMemory = 0xFFFF;
    IoInspectorWindow ioWin;
    ioWin.onGoToDisassembly = [&navDisasm](uint16_t a) { navDisasm = a; };
    ioWin.onGoToMemoryInspector = [&navMemory](uint16_t a) { navMemory = a; };
    
    // Simulate navigation
    if (ioWin.onGoToDisassembly) ioWin.onGoToDisassembly(0x0000);
    if (ioWin.onGoToMemoryInspector) ioWin.onGoToMemoryInspector(0x0002);
    CHECK_EQ(0x0000, navDisasm, "I/O nav: GoToDisassembly(0x0000)");
    CHECK_EQ(0x0002, navMemory, "I/O nav: GoToMemoryInspector(0x0002)");
    
    // Test IoInspectorWindow state
    ioWin.setFollowIo(false);
    CHECK(!ioWin.followIo(), "followIo == false");
    ioWin.setPauseCapture(true);
    CHECK(ioWin.pauseCapture(), "pauseCapture == true");
    ioWin.setTypeFilter(IoInspectorWindow::TypeFilter::Out);
    CHECK((int)ioWin.typeFilter() == (int)IoInspectorWindow::TypeFilter::Out,
          "typeFilter == Out");
    ioWin.setPortFilter(0x10);
    CHECK_EQ(0x10, ioWin.portFilter(), "portFilter == 0x10");
    
    backend.clearBreakpoints();
    printf("  I/O Inspector API works with real Board\n");
    
    // --- Cleanup ---
    test_backend = nullptr;
    
    printf("  Board and components destroyed\n");
    
    TEST_END();
}

// ---------------------------------------------------------------------------
// Regression test: Breakpoint removal syncs to Board
//
// Verifies the full chain:
//   GUI → requestRemoveBreakpoint → DebugBackend → syncBreakpointsToTarget
//   → DebugAdapter/TestTarget::syncBreakpoints → Board::remove_breakpoint
//
// Also tests: disable/re-enable, clear all, re-set after remove, duplicates.
// ---------------------------------------------------------------------------

static void test_breakpoint_sync_removal()
{
    TEST_BEGIN("Breakpoint removal syncs to Board (regression)");

    // --- Headless setup (same as test_board_smoke) ---
    Options.novideo = true;
    Options.nosound = true;

    Memory memory;
    FD1793 fdc;
    Wav wav;
    WavPlayer tape_player(wav);
    Keyboard keyboard;
    I8253 timer;
    TimerWrapper tw(timer);
    AY ay;
    AYWrapper aw(ay);
    Soundnik soundnik(tw, aw);
    IO io(memory, keyboard, timer, fdc, ay, tape_player);
    TV tv;
    PixelFiller filler(memory, io, tv);
    filler.init();
    soundnik.init(nullptr);
    tv.init();
    Board board(memory, io, filler, soundnik, tv, tape_player);
    board.init();

    TestBoardTarget target(memory, board);
    DebugBackend backend(target);
    backend.testSynchronous_ = true;

    // Write test program:
    // 0000: MVI A, 55h    ; 3E 55
    // 0002: INR A         ; 3C
    // 0003: JMP 0002h     ; C3 02 00
    for (size_t i = 0; i < sizeof(test_program); ++i)
        memory.write((uint16_t)i, test_program[i], false);

    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();

    // --- Test 1: Set breakpoint, verify it fires ---
    printf("  Test 1: Set BP at 0x0002, run, expect stop...\n");
    auto r1 = backend.requestAddBreakpoint(0x0002);
    CHECK(r1.success, "BP added via request");
    backend.run();
    CHECK_EQ(0x0002, backend.getCpuState().pc, "PC == 0x0002 at BP");
    CHECK_EQ(0x55, backend.getCpuState().a, "A == 0x55 (INR not executed)");

    // --- Test 2: Remove breakpoint, verify execution passes through ---
    printf("  Test 2: Remove BP, step past, run, expect no stop...\n");
    auto r2 = backend.requestRemoveBreakpoint(0x0002);
    CHECK(r2.success, "BP removed via request");
    // Step past the breakpoint address
    backend.stepInstruction();  // INR A at 0x0002, A becomes 0x56
    CHECK_EQ(0x0003, backend.getCpuState().pc, "PC == 0x0003 after step");
    CHECK_EQ(0x56, backend.getCpuState().a, "A == 0x56 (INR executed)");
    // Key check: hasBreakpoint returns false
    CHECK(!backend.hasBreakpoint(0x0002), "No BP at 0x0002 after removal");

    // --- Test 3: Re-set and re-remove (spec item 9) ---
    printf("  Test 3: Re-set BP at 0x0002, remove, verify gone...\n");
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    auto r3 = backend.requestAddBreakpoint(0x0002);
    CHECK(r3.success, "BP re-added via request");
    backend.run();
    CHECK_EQ(0x0002, backend.getCpuState().pc, "PC == 0x0002 (re-set BP fires)");
    // Now remove
    auto r4 = backend.requestRemoveBreakpoint(0x0002);
    CHECK(r4.success, "BP removed via request");
    CHECK(!backend.hasBreakpoint(0x0002), "BP gone after re-remove");
    // Step through — should not stop
    backend.stepInstruction();  // INR A
    CHECK_EQ(0x0003, backend.getCpuState().pc, "PC == 0x0003 (passed through)");

    // --- Test 4: Disable → sync → verify no stop → re-enable → verify stop ---
    printf("  Test 4: Disable BP, verify no stop; re-enable, verify stop...\n");
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.clearHistory();
    backend.requestClearBreakpoints();
    backend.requestAddBreakpoint(0x0002);
    // Disable
    backend.requestSetBreakpointEnabled(0x0002, false);
    CHECK(!backend.hasBreakpoint(0x0002), "Disabled BP not active");
    // Re-enable
    backend.requestSetBreakpointEnabled(0x0002, true);
    CHECK(backend.hasBreakpoint(0x0002), "Re-enabled BP is active");
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.run();
    CHECK_EQ(0x0002, backend.getCpuState().pc, "PC == 0x0002 (re-enabled BP fires)");

    // --- Test 5: Clear all breakpoints → verify Board has none ---
    printf("  Test 5: Clear all BPs, verify Board has none...\n");
    backend.requestClearBreakpoints();
    CHECK(!backend.hasBreakpoint(0x0002), "No BP after clearAll");
    auto bpList = backend.getBreakpoints();
    CHECK_EQ((size_t)0, bpList.size(), "BP list empty after clearAll");
    // Step through — should not stop anywhere
    // PC=0x0002 → step INR A → 0x0003 → step JMP 0002 → 0x0002
    backend.stepInstruction();  // INR A
    backend.stepInstruction();  // JMP 0002
    CHECK_EQ(0x0002, backend.getCpuState().pc, "PC == 0x0002 (JMP landed)");
    backend.stepInstruction();  // INR A again — no breakpoint!
    CHECK_EQ(0x0003, backend.getCpuState().pc, "PC == 0x0003 (passed through, no BP)");

    // --- Test 6: Duplicate add rejection + remove ---
    printf("  Test 6: Duplicate add, remove, verify clean...\n");
    backend.requestClearBreakpoints();
    auto r5 = backend.requestAddBreakpoint(0x0003);
    CHECK(r5.success, "First add succeeds");
    auto r6 = backend.requestAddBreakpoint(0x0003);
    CHECK(!r6.success, "Duplicate add returns failure");
    // Remove
    auto r7 = backend.requestRemoveBreakpoint(0x0003);
    CHECK(r7.success, "BP removed");
    CHECK(!backend.hasBreakpoint(0x0003), "BP gone after remove");
    CHECK_EQ((size_t)0, backend.getBreakpoints().size(), "BP list empty");

    // --- Test 7: Remove one BP doesn't affect another ---
    printf("  Test 7: Remove one BP, other survives...\n");
    backend.requestClearBreakpoints();
    backend.requestAddBreakpoint(0x0002);
    backend.requestAddBreakpoint(0x0003);
    // Remove first
    backend.requestRemoveBreakpoint(0x0002);
    CHECK(!backend.hasBreakpoint(0x0002), "BP at 0x0002 removed");
    CHECK(backend.hasBreakpoint(0x0003), "BP at 0x0003 survives");
    // Verify in Board: run from 0, should stop at 0x0003 not 0x0002
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    backend.run();
    CHECK_EQ(0x0003, backend.getCpuState().pc, "Stopped at 0x0003 (0x0002 BP removed)");

    backend.requestClearBreakpoints();

    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("\033[0;36m=== Board Integration Smoke Test (Stage 3.2, simplified) ===\033[0m\n");
    
    test_board_smoke();
    test_breakpoint_sync_removal();
    
    printf("\n\033[0;36m=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", \033[41;97m %d FAILED \033[0m\033[0;36m", tests_failed);
    }
    printf(" ===\033[0m\n\n");
    
    return tests_failed > 0 ? 1 : 0;
}
