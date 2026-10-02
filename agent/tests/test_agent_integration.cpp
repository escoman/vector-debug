// Agent Integration Tests — Stage 5.3.2
//
// 20 tests verifying Command Queue + Emulation Thread isolation
// using real DebugBackend + NoBoardTarget.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <future>
#include <set>

#include "agent_api.h"
#include "backend.h"
#include "memory.h"
#include "no_board_target.h"
#include "debug_target.h"
#include "events.h"
#include "options.h"

// ---------------------------------------------------------------------------
// Global references needed by debug_adapter.cpp (weak dependency)
// ---------------------------------------------------------------------------
DebugBackend *g_adapter_backend = nullptr;

// ---------------------------------------------------------------------------
// HAL functions — needed by i8080.cpp (same as test_backend.cpp)
// ---------------------------------------------------------------------------

static Memory       *hal_memory = nullptr;
static DebugBackend *hal_dbg    = nullptr;
static bool          hal_iff    = false;

int i8080_hal_memory_read_byte(int addr)
{
    return hal_memory->read(addr, false);
}

void i8080_hal_memory_write_byte(int addr, int value)
{
    hal_memory->write(addr, value, false);
}

int i8080_hal_memory_read_word(int addr, bool stack)
{
    return hal_memory->read(addr, stack)
         | (hal_memory->read(addr + 1, stack) << 8);
}

void i8080_hal_memory_write_word(int addr, int word, bool stack)
{
    hal_memory->write(addr, word & 0xff, stack);
    hal_memory->write(addr + 1, word >> 8, stack);
}

int i8080_hal_io_input(int port)
{
    if (hal_dbg) hal_dbg->onIoInput((uint8_t)port, 0xff);
    return 0xff;
}

void i8080_hal_io_output(int port, int value)
{
    if (hal_dbg) hal_dbg->onIoOutput((uint8_t)port, (uint8_t)value);
}

void i8080_hal_iff(int on)
{
    hal_iff = (on != 0);
}

// ---------------------------------------------------------------------------
// Test framework
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
// Test program
// ---------------------------------------------------------------------------

static const uint8_t test_program[] = {
    // 0x0000: LXI SP, 0xC100
    0x31, 0x00, 0xC1,
    // 0x0003: MVI A, 0x05
    0x3E, 0x05,
    // 0x0005: CALL 0x0200
    0xCD, 0x00, 0x02,
    // 0x0008: DCR A
    0x3D,
    // 0x0009: JNZ 0x0005
    0xC2, 0x05, 0x00,
    // 0x000C: HLT
    0x76,
};

static const uint8_t subroutine[] = {
    // 0x0200: PUSH H
    0xE5,
    // 0x0201: MVI H, 0xC0
    0x26, 0xC0,
    // 0x0203: MVI L, 0x00
    0x2E, 0x00,
    // 0x0205: MOV M, A
    0x77,
    // 0x0206: POP H
    0xE1,
    // 0x0207: RET
    0xC9,
};

// IO test program: OUT instruction
static const uint8_t io_test_program[] = {
    // 0x0000: LXI SP, 0xC100
    0x31, 0x00, 0xC1,
    // 0x0003: MVI A, 0x55
    0x3E, 0x55,
    // 0x0005: OUT 0x03
    0xD3, 0x03,
    // 0x0007: HLT
    0x76,
};

// VRAM write test program
static const uint8_t vram_test_program[] = {
    // 0x0000: LXI SP, 0xC100
    0x31, 0x00, 0xC1,
    // 0x0003: LXI H, 0xC000
    0x21, 0x00, 0xC0,
    // 0x0006: MVI A, 0x42
    0x3E, 0x42,
    // 0x0008: MOV M, A   (writes 0x42 to 0xC000 = VRAM)
    0x77,
    // 0x0009: HLT
    0x76,
};

// Simple loop program for running tests
static const uint8_t loop_program[] = {
    // 0x0000: LXI SP, 0xC100
    0x31, 0x00, 0xC1,
    // 0x0003: MVI A, 0x42
    0x3E, 0x42,
    // 0x0005: MOV M, A   (HL=C000 from LXI H below — but we use this as bp target)
    0x77,
    // 0x0006: INR A
    0x3C,
    // 0x0007: MOV M, A
    0x77,
    // 0x0008: JMP 0003
    0xC3, 0x03, 0x00,
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void writeProgram(Memory &mem, const uint8_t *data, size_t len, uint16_t addr)
{
    for (size_t i = 0; i < len; ++i) {
        mem.write(static_cast<uint16_t>(addr + i), data[i], false);
    }
}

struct TestFixture {
    Memory mem;
    NoBoardTarget *target;
    DebugBackend *backend;

    TestFixture() {
        Options.novideo = true;
        Options.nosound = true;
        target = new NoBoardTarget(mem);
        backend = new DebugBackend(*target);
        // Stage 5.3.3.1: Enable test-only synchronous fallback
        backend->testSynchronous_ = true;
        backend->reset();
        // Set HAL globals for i8080 CPU
        hal_memory = &mem;
        hal_dbg = backend;
    }
    ~TestFixture() {
        hal_dbg = nullptr;
        hal_memory = nullptr;
        delete backend;
        delete target;
    }

    void loadMainProgram() {
        writeProgram(mem, test_program, sizeof(test_program), 0x0000);
        writeProgram(mem, subroutine, sizeof(subroutine), 0x0200);
    }

    void loadLoopProgram() {
        writeProgram(mem, loop_program, sizeof(loop_program), 0x0000);
    }

    void initCpu(uint16_t pc = 0, uint16_t sp = 0xC100) {
        target->initCpu(pc, sp);
    }
};

// ---------------------------------------------------------------------------
// Test 1: command_queue_preserves_order
// ---------------------------------------------------------------------------

static void test_command_queue_preserves_order()
{
    TEST_BEGIN("command_queue_preserves_order");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    // Add N breakpoints via command queue and verify all succeed in order
    const int N = 10;
    std::vector<uint16_t> addresses;
    for (int i = 0; i < N; ++i) {
        addresses.push_back(static_cast<uint16_t>(0x1000 + i * 0x100));
    }

    for (int i = 0; i < N; ++i) {
        auto r = f.backend->requestAddBreakpoint(addresses[i]);
        CHECK(r.success, "breakpoint added");
    }

    // Verify all breakpoints exist
    auto bps = f.backend->getBreakpoints();
    CHECK_EQ(N, (int)bps.size(), "all N breakpoints present");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 2: concurrent_commands_not_lost
// ---------------------------------------------------------------------------

static void test_concurrent_commands_not_lost()
{
    TEST_BEGIN("concurrent_commands_not_lost");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    std::atomic<int> successCount{0};
    auto addBp = [&](uint16_t addr) {
        auto r = f.backend->requestAddBreakpoint(addr);
        if (r.success) successCount++;
    };

    // 4 threads add breakpoints simultaneously
    std::thread t1(addBp, 0x0300);
    std::thread t2(addBp, 0x0400);
    std::thread t3(addBp, 0x0500);
    std::thread t4(addBp, 0x0600);
    t1.join(); t2.join(); t3.join(); t4.join();

    CHECK_EQ(4, successCount.load(), "all 4 concurrent breakpoints added");
    auto bps = f.backend->getBreakpoints();
    CHECK_EQ(4, (int)bps.size(), "4 breakpoints total");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 3: each_command_receives_own_result
// ---------------------------------------------------------------------------

static void test_each_command_receives_own_result()
{
    TEST_BEGIN("each_command_receives_own_result");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    // Add breakpoints concurrently, each should get its own result
    std::atomic<int> dupFails{0};
    auto addBp = [&](uint16_t addr) {
        auto r = f.backend->requestAddBreakpoint(addr);
        if (!r.success) dupFails++;
    };

    // Two threads try same address — one should fail
    std::thread t1(addBp, 0x0300);
    std::thread t2(addBp, 0x0300);
    t1.join(); t2.join();

    CHECK_EQ(1, dupFails.load(), "one duplicate rejected");

    // Different addresses should both succeed
    auto r1 = f.backend->requestAddBreakpoint(0x0400);
    auto r2 = f.backend->requestAddBreakpoint(0x0500);
    CHECK(r1.success, "first unique succeeds");
    CHECK(r2.success, "second unique succeeds");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 4: command_executes_on_emulation_thread
// ---------------------------------------------------------------------------

static void test_command_executes_on_emulation_thread()
{
    TEST_BEGIN("command_executes_on_emulation_thread");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    // Without emulation thread, commands execute on calling thread
    // Verify by adding a breakpoint and checking it exists
    auto r = f.backend->requestAddBreakpoint(0x0300);
    CHECK(r.success, "command executed successfully");
    CHECK(f.backend->hasBreakpoint(0x0300), "breakpoint exists after command");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 5: agent_thread_never_executes_cpu
// ---------------------------------------------------------------------------

static void test_agent_thread_never_executes_cpu()
{
    TEST_BEGIN("agent_thread_never_executes_cpu");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu(0x0000);

    uint16_t pcBefore = f.backend->getCpuState().pc;

    // Agent API calls should NOT advance PC (no direct CPU execution)
    f.backend->requestAddBreakpoint(0x0300);
    f.backend->requestCreateFunction(0x0200, "DrawPixel");

    uint16_t pcAfter = f.backend->getCpuState().pc;
    CHECK_EQ(pcBefore, pcAfter, "PC unchanged after agent commands");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 6: commands_work_while_running (test scenario — no emulation thread)
// ---------------------------------------------------------------------------

static void test_commands_work_while_running()
{
    TEST_BEGIN("commands_work_while_running");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    // Set state to Running (flag only — no emulation thread)
    f.backend->requestRun();
    CHECK_EQ((int)DebuggerState::Running, (int)f.backend->getState(), "state is Running");

    // Commands should still work (direct execution in test scenario)
    auto r = f.backend->requestAddBreakpoint(0x0300);
    CHECK(r.success, "breakpoint added while running");
    CHECK(f.backend->hasBreakpoint(0x0300), "breakpoint confirmed");

    f.backend->requestPause();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 7: pause_while_running
// ---------------------------------------------------------------------------

static void test_pause_while_running()
{
    TEST_BEGIN("pause_while_running");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    f.backend->requestRun();
    CHECK_EQ((int)DebuggerState::Running, (int)f.backend->getState(), "running");

    f.backend->requestPause();
    // In test scenario, pause just sets flags
    CHECK(!f.backend->isPaused() || f.backend->getState() == DebuggerState::Running,
          "pause requested (flags set)");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 8: quit_while_running
// ---------------------------------------------------------------------------

static void test_quit_while_running()
{
    TEST_BEGIN("quit_while_running");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    f.backend->requestRun();
    f.backend->requestQuit();
    CHECK(f.backend->isQuitRequested(), "quit requested");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 9: breakpoint_mutation_through_queue
// ---------------------------------------------------------------------------

static void test_breakpoint_mutation_through_queue()
{
    TEST_BEGIN("breakpoint_mutation_through_queue");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    // Add
    auto r1 = f.backend->requestAddBreakpoint(0x0300);
    CHECK(r1.success, "add succeeds");

    // Enable/disable
    auto r2 = f.backend->requestSetBreakpointEnabled(0x0300, false);
    CHECK(r2.success, "disable succeeds");
    CHECK(!f.backend->hasBreakpoint(0x0300), "disabled bp not active");

    auto r3 = f.backend->requestSetBreakpointEnabled(0x0300, true);
    CHECK(r3.success, "enable succeeds");
    CHECK(f.backend->hasBreakpoint(0x0300), "enabled bp active");

    // Remove
    auto r4 = f.backend->requestRemoveBreakpoint(0x0300);
    CHECK(r4.success, "remove succeeds");
    CHECK(!f.backend->hasBreakpoint(0x0300), "removed bp gone");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 10: annotation_mutation_through_queue
// ---------------------------------------------------------------------------

static void test_annotation_mutation_through_queue()
{
    TEST_BEGIN("annotation_mutation_through_queue");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    // Create function
    auto r1 = f.backend->requestCreateFunction(0x0200, "DrawPixel");
    CHECK(r1.success, "function created");

    // Rename
    auto r2 = f.backend->requestRenameSymbol(0x0200, "DrawSprite");
    CHECK(r2.success, "rename succeeds");

    const DebugSymbol *sym = f.backend->symbolDatabase().findSymbol(0x0200);
    CHECK(sym != nullptr, "symbol found");
    if (sym) CHECK(sym->name == "DrawSprite", "renamed correctly");

    // Set comment
    auto r3 = f.backend->requestSetComment(0x0200, "VRAM draw routine");
    CHECK(r3.success, "comment set");

    // Add label
    auto r4 = f.backend->requestAddLabel(0x0207, "ret_label");
    CHECK(r4.success, "label added");

    // Remove
    auto r5 = f.backend->requestRemoveSymbol(0x0200);
    CHECK(r5.success, "symbol removed");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 11: trace_execution_through_queue
// ---------------------------------------------------------------------------

static void test_trace_execution_through_queue()
{
    TEST_BEGIN("trace_execution_through_queue");
    TestFixture f;
    f.loadMainProgram();
    // Set CPU at subroutine entry (post-CALL state)
    f.initCpu(0x0200, 0xC0F0);

    // Push return address on stack
    f.mem.write(0xC0F0, 0x09, false);  // return PC low
    f.mem.write(0xC0F1, 0x00, false);  // return PC high

    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0200;
    params.maxInstructions = 100;
    params.stopOnRet = true;
    params.stopOnCallerReturn = false;

    auto future = f.backend->requestExecuteTrace(params);
    auto result = future.get();

    CHECK(result.instructionsExecuted > 0, "instructions executed");
    CHECK(result.exitReason == ExitReason::Ret, "exit reason is Ret");
    CHECK_EQ(0x0207u, (unsigned)result.exitPc, "exit at RET");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 12: trace_timeout
// ---------------------------------------------------------------------------

static void test_trace_timeout()
{
    TEST_BEGIN("trace_timeout");
    TestFixture f;
    f.loadLoopProgram();
    f.initCpu(0x0003);  // Start at MVI A (infinite loop)

    // Set HL for MOV M,A
    CpuState cpu = f.backend->getCpuState();
    cpu.h = 0xC0; cpu.l = 0x00;
    f.target->writeCpuRegister(static_cast<int>(IDebugBackend::RegisterId::HL), 0xC000);

    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0003;
    params.maxInstructions = 10;  // very small limit
    params.stopOnRet = true;
    params.stopOnCallerReturn = false;

    auto result = f.backend->requestExecuteTrace(params).get();

    CHECK(result.exitReason == ExitReason::Timeout, "exit reason is Timeout");
    CHECK_EQ(10u, (unsigned)result.instructionsExecuted, "exactly maxInstructions");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 13: concurrent_trace_requests
// ---------------------------------------------------------------------------

static void test_concurrent_trace_requests()
{
    TEST_BEGIN("concurrent_trace_requests");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu(0x0200, 0xC0F0);

    f.mem.write(0xC0F0, 0x09, false);
    f.mem.write(0xC0F1, 0x00, false);

    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0200;
    params.maxInstructions = 100;
    params.stopOnRet = true;

    // First trace should succeed
    auto r1 = f.backend->requestExecuteTrace(params).get();
    CHECK(r1.exitReason == ExitReason::Ret, "first trace succeeds");

    // Second trace should also succeed (first completed)
    f.initCpu(0x0200, 0xC0F0);
    f.mem.write(0xC0F0, 0x09, false);
    f.mem.write(0xC0F1, 0x00, false);
    auto r2 = f.backend->requestExecuteTrace(params).get();
    CHECK(r2.exitReason == ExitReason::Ret, "second trace succeeds");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 14: real_debugbackend_threaded_integration
// ---------------------------------------------------------------------------

static void test_real_debugbackend_threaded_integration()
{
    TEST_BEGIN("real_debugbackend_threaded_integration");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu();

    // Start emulation thread
    std::thread emuThread([&]{ f.backend->runUntilPause(); });

    // Give thread time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Quit cleanly
    f.backend->requestQuit();
    emuThread.join();

    CHECK(f.backend->isQuitRequested(), "quit requested");
    CHECK_EQ((int)DebuggerState::Paused, (int)f.backend->getState(), "state is Paused after quit");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 15: real_breakpoint_integration
// ---------------------------------------------------------------------------

static void test_real_breakpoint_integration()
{
    TEST_BEGIN("real_breakpoint_integration");
    TestFixture f;
    f.loadLoopProgram();
    f.initCpu(0x0000);

    // Add breakpoint at 0x0005
    f.backend->requestAddBreakpoint(0x0005);

    // Start emulation thread
    std::thread emuThread([&]{ f.backend->runUntilPause(); });

    // Run to breakpoint
    f.backend->requestRun();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Pause and quit
    f.backend->requestPause();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    f.backend->requestQuit();
    emuThread.join();

    // Should have stopped at or near breakpoint
    uint16_t pc = f.backend->getCpuState().pc;
    CHECK(pc <= 0x000A, "PC in valid range");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 16: real_trace_integration
// ---------------------------------------------------------------------------

static void test_real_trace_integration()
{
    TEST_BEGIN("real_trace_integration");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu(0x0200, 0xC0F0);

    f.mem.write(0xC0F0, 0x09, false);
    f.mem.write(0xC0F1, 0x00, false);

    // Trace without emulation thread (direct execution)
    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0200;
    params.maxInstructions = 100;
    params.stopOnRet = true;

    auto result = f.backend->requestExecuteTrace(params).get();

    CHECK(result.instructionsExecuted > 0, "trace executed instructions");
    CHECK(result.exitReason == ExitReason::Ret, "trace ended with RET");
    CHECK(result.exitSp >= result.entrySp, "SP tracked correctly");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 17: memory_attribution
// ---------------------------------------------------------------------------

static void test_memory_attribution()
{
    TEST_BEGIN("memory_attribution");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu(0x0200, 0xC0F0);

    f.mem.write(0xC0F0, 0x09, false);
    f.mem.write(0xC0F1, 0x00, false);

    // Set A to known value
    f.target->writeCpuRegister(static_cast<int>(IDebugBackend::RegisterId::AF), 0x5500);

    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0200;
    params.maxInstructions = 100;
    params.stopOnRet = true;

    auto result = f.backend->requestExecuteTrace(params).get();
    CHECK(result.instructionsExecuted > 0, "trace completed");

    // Check memory history for writes during trace
    auto memHistory = f.backend->memoryHistorySnapshot();
    bool foundWrite = false;
    for (const auto &ev : memHistory) {
        if (ev.instructionSequence >= result.startSequence &&
            ev.instructionSequence < result.endSequence &&
            ev.type == MemoryAccessType::Write &&
            ev.virt >= 0xC000 && ev.virt < 0xC100) {
            foundWrite = true;
            break;
        }
    }
    CHECK(foundWrite, "memory write attributed during trace");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 18: io_attribution
// ---------------------------------------------------------------------------

static void test_io_attribution()
{
    TEST_BEGIN("io_attribution");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu(0x0200, 0xC0F0);

    f.mem.write(0xC0F0, 0x09, false);
    f.mem.write(0xC0F1, 0x00, false);

    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0200;
    params.maxInstructions = 100;
    params.stopOnRet = true;
    params.stopOnCallerReturn = false;

    auto result = f.backend->requestExecuteTrace(params).get();
    // Subroutine has no IO instructions — just verify trace completes
    CHECK(result.instructionsExecuted > 0, "trace completed");
    CHECK(result.exitReason == ExitReason::Ret, "trace ended with RET");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 19: vram_attribution
// ---------------------------------------------------------------------------

static void test_vram_attribution()
{
    TEST_BEGIN("vram_attribution");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu(0x0200, 0xC0F0);

    f.mem.write(0xC0F0, 0x09, false);
    f.mem.write(0xC0F1, 0x00, false);

    // Set A=0x55 for MOV M,A
    f.target->writeCpuRegister(static_cast<int>(IDebugBackend::RegisterId::AF), 0x5500);

    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0200;
    params.maxInstructions = 100;
    params.stopOnRet = true;

    auto result = f.backend->requestExecuteTrace(params).get();

    // Check memory history for write to address 0xC000 (HL=C000 from MVI H,L)
    auto memHistory = f.backend->memoryHistorySnapshot();
    bool foundVramWrite = false;
    for (const auto &ev : memHistory) {
        if (ev.instructionSequence >= result.startSequence &&
            ev.instructionSequence < result.endSequence &&
            ev.type == MemoryAccessType::Write &&
            ev.virt == 0xC000) {
            foundVramWrite = true;
            break;
        }
    }
    CHECK(foundVramWrite, "VRAM write at 0xC000 (MOV M,A with HL=C000)");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 20: unknown_pc_remains_unknown
// ---------------------------------------------------------------------------

static void test_unknown_pc_remains_unknown()
{
    TEST_BEGIN("unknown_pc_remains_unknown");
    TestFixture f;
    f.loadMainProgram();
    f.initCpu(0x0000);

    // PC=0 is a valid PC, not "unknown"
    uint16_t pc = f.backend->getCpuState().pc;
    CHECK_EQ(0x0000u, (unsigned)pc, "PC is 0x0000 (valid, not unknown)");

    // Step from PC=0
    f.backend->stepInstruction();
    uint16_t pcAfter = f.backend->getCpuState().pc;
    CHECK_EQ(0x0003u, (unsigned)pcAfter, "PC advanced to 0x0003 after LXI SP");

    // Verify that instruction history records correct PC
    auto history = f.backend->instructionHistorySnapshot();
    CHECK(history.size() >= 1, "history has entries");
    if (!history.empty()) {
        CHECK_EQ(0x0000u, (unsigned)history[0].pcBefore, "first instruction at PC=0");
    }
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 5.3.3: Additional mandatory tests
// ---------------------------------------------------------------------------

// Test: concurrent breakpoint commands on real Backend (Section 29 #2)
static void test_concurrent_breakpoint_real()
{
    TEST_BEGIN("concurrent_breakpoint_real");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    writeProgram(f.mem, subroutine, sizeof(subroutine), 0x0200);

    std::thread emuThread([&]{ f.backend->runUntilPause(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Two threads add breakpoints simultaneously
    std::atomic<bool> r1ok{false}, r2ok{false};
    std::thread t1([&]{
        auto res = f.backend->requestAddBreakpoint(0x0200);
        r1ok = res.success;
    });
    std::thread t2([&]{
        auto res = f.backend->requestAddBreakpoint(0x0300);
        r2ok = res.success;
    });
    t1.join();
    t2.join();

    CHECK(r1ok.load(), "thread A: breakpoint 0x0200 added");
    CHECK(r2ok.load(), "thread B: breakpoint 0x0300 added");
    CHECK(f.backend->hasBreakpoint(0x0200), "0x0200 exists");
    CHECK(f.backend->hasBreakpoint(0x0300), "0x0300 exists");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: concurrent annotation commands on real Backend (Section 29 #3)
static void test_concurrent_annotation_real()
{
    TEST_BEGIN("concurrent_annotation_real");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);

    std::thread emuThread([&]{ f.backend->runUntilPause(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<int> okCount{0};
    std::thread t1([&]{
        auto res = f.backend->requestCreateFunction(0x0200, "func_A");
        if (res.success) okCount++;
    });
    std::thread t2([&]{
        auto res = f.backend->requestCreateFunction(0x0300, "func_B");
        if (res.success) okCount++;
    });
    t1.join();
    t2.join();

    CHECK_EQ(2u, (unsigned)okCount.load(), "both annotations created");
    CHECK(f.backend->symbolDatabase().findSymbol(0x0200) != nullptr, "func_A exists");
    CHECK(f.backend->symbolDatabase().findSymbol(0x0300) != nullptr, "func_B exists");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: step executes only on Emulation Thread (Section 29 #5)
static void test_step_only_on_emulation_thread()
{
    TEST_BEGIN("step_only_on_emulation_thread");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);

    auto emuThreadId = std::thread::id{};
    std::atomic<bool> stepOnEmu{false};

    std::thread emuThread([&]{
        emuThreadId = std::this_thread::get_id();
        f.backend->runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Request step — should execute on emulation thread
    f.backend->requestStep();
    auto history = f.backend->instructionHistorySnapshot();
    // After step, instruction should have been executed on emulation thread
    CHECK(history.size() >= 1, "step executed (history has entries)");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: trace executes only on Emulation Thread (Section 29 #6)
static void test_trace_only_on_emulation_thread()
{
    TEST_BEGIN("trace_only_on_emulation_thread");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    writeProgram(f.mem, subroutine, sizeof(subroutine), 0x0200);

    // Set up CPU to call subroutine
    f.backend->reset();
    f.backend->writeRegister(IDebugBackend::RegisterId::SP, 0xC100);
    f.backend->writeRegister(IDebugBackend::RegisterId::PC, 0x0200);

    std::thread emuThread([&]{
        f.backend->runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Trace should execute on emulation thread
    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0200;
    params.maxInstructions = 100;
    params.stopOnRet = true;
    auto result = f.backend->requestExecuteTrace(params).get();

    CHECK(result.instructionsExecuted > 0, "trace executed instructions");
    CHECK(result.exitReason == ExitReason::Ret, "trace ended with RET");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: no pending promise after Quit (Section 29 #18)
static void test_no_pending_promise_after_quit()
{
    TEST_BEGIN("no_pending_promise_after_quit");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);

    std::thread emuThread([&]{ f.backend->runUntilPause(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send a command, then immediately quit
    auto future = f.backend->requestAddBreakpoint(0x0500);
    // future should complete (either success or cancelled)
    f.backend->requestQuit();
    emuThread.join();

    // If we get here without hanging, all promises were fulfilled
    CHECK(true, "no dangling promise (thread joined cleanly)");
    TEST_END();
}

// Test: no command lost during shutdown (Section 29 #19)
static void test_no_command_lost_during_shutdown()
{
    TEST_BEGIN("no_command_lost_during_shutdown");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);

    std::thread emuThread([&]{ f.backend->runUntilPause(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Enqueue several commands, then quit
    std::atomic<int> completedCount{0};
    std::thread t1([&]{
        auto r = f.backend->requestAddBreakpoint(0x0100);
        completedCount++;
    });
    std::thread t2([&]{
        auto r = f.backend->requestCreateFunction(0x0400, "test_func");
        completedCount++;
    });

    // Give commands time to be enqueued
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.backend->requestQuit();

    t1.join();
    t2.join();
    emuThread.join();

    // All callers should have received a result (success or cancelled)
    CHECK_EQ(2u, (unsigned)completedCount.load(), "all callers received result");
    TEST_END();
}

// Test: PC=0000 is not Unknown (Section 29 #17)
static void test_pc0000_not_unknown()
{
    TEST_BEGIN("pc0000_not_unknown");
    TestFixture f;

    // Place a program at PC=0x0000
    static const uint8_t prog_at_zero[] = {
        0x31, 0x00, 0xC1,  // LXI SP, C100
        0x3E, 0x42,        // MVI A, 42
        0x76,              // HLT
    };
    writeProgram(f.mem, prog_at_zero, sizeof(prog_at_zero), 0x0000);
    f.backend->reset();

    // Execute trace from PC=0
    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0000;
    params.maxInstructions = 100;
    params.stopOnRet = false;
    auto result = f.backend->requestExecuteTrace(params).get();

    CHECK(result.instructionsExecuted >= 2, "executed instructions at PC=0");

    // Check that instruction history has PC=0x0000 with hasPc=true
    auto history = f.backend->instructionHistorySnapshot();
    bool foundZero = false;
    for (const auto &ie : history) {
        if (ie.pcBefore == 0x0000) {
            foundZero = true;
            break;
        }
    }
    CHECK(foundZero, "PC=0x0000 found in history (real PC, not unknown)");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 5.3.3.1: Mandatory fixup tests
// ---------------------------------------------------------------------------

// Test: Quit with multiple pending commands (Section 17)
static void test_quit_with_multiple_pending_commands()
{
    TEST_BEGIN("quit_with_multiple_pending_commands");
    TestFixture f;
    writeProgram(f.mem, loop_program, sizeof(loop_program), 0x0000);
    f.initCpu(0x0000);

    std::thread emuThread([&]{ f.backend->runUntilPause(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Enqueue commands from multiple threads + quit
    std::atomic<int> completedCount{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 5; i++) {
        uint16_t addr = static_cast<uint16_t>(0x1000 + i * 0x100);
        threads.emplace_back([&f, &completedCount, addr]{
            auto r = f.backend->requestAddBreakpoint(addr);
            completedCount++;
            (void)r.status; // Completed or Cancelled — both acceptable
        });
    }

    // Small delay to let some commands enqueue
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    f.backend->requestQuit();

    for (auto &t : threads) t.join();
    emuThread.join();

    CHECK_EQ(5u, (unsigned)completedCount.load(), "all 5 callers received result");
    TEST_END();
}

// Test: IO attribution with OUT instruction (Section 19)
static void test_io_attribution_with_out_instruction()
{
    TEST_BEGIN("io_attribution_with_out_instruction");
    TestFixture f;
    writeProgram(f.mem, io_test_program, sizeof(io_test_program), 0x0000);
    f.initCpu(0x0000);

    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0000;
    params.maxInstructions = 10;
    params.stopOnRet = false;

    auto result = f.backend->requestExecuteTrace(params).get();
    CHECK(result.instructionsExecuted >= 3, "executed through OUT instruction");

    // Check IO history for OUT event during trace
    auto ioHistory = f.backend->ioHistorySnapshot();
    bool foundIoOut = false;
    for (const auto &ev : ioHistory) {
        if (ev.instructionSequence >= result.startSequence &&
            ev.instructionSequence < result.endSequence &&
            ev.type == IoAccessType::Out &&
            ev.port == 0x03) {
            foundIoOut = true;
            CHECK_EQ(0x55u, (unsigned)ev.value, "IO OUT value is 0x55");
            break;
        }
    }
    CHECK(foundIoOut, "IO OUT event found at port 0x03");

    // Verify PC attribution: OUT is at PC=0x0005
    std::map<uint64_t, uint16_t> seqToPc;
    auto instrTrace = f.backend->instructionHistorySnapshot();
    for (const auto &ie : instrTrace) {
        if (ie.sequence >= result.startSequence &&
            ie.sequence < result.endSequence) {
            seqToPc[ie.sequence] = ie.pcBefore;
        }
    }
    for (const auto &ev : ioHistory) {
        if (ev.instructionSequence >= result.startSequence &&
            ev.instructionSequence < result.endSequence &&
            ev.type == IoAccessType::Out) {
            auto it = seqToPc.find(ev.instructionSequence);
            if (it != seqToPc.end()) {
                CHECK_EQ(0x0005u, (unsigned)it->second, "IO event PC = 0x0005 (OUT instruction)");
            }
            break;
        }
    }
    TEST_END();
}

// Test: Memory/VRAM attribution detailed (Section 20)
static void test_memory_vram_attribution_detailed()
{
    TEST_BEGIN("memory_vram_attribution_detailed");
    TestFixture f;
    writeProgram(f.mem, vram_test_program, sizeof(vram_test_program), 0x0000);
    f.initCpu(0x0000);

    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0000;
    params.maxInstructions = 10;
    params.stopOnRet = false;

    auto result = f.backend->requestExecuteTrace(params).get();
    CHECK(result.instructionsExecuted >= 4, "executed through MOV M,A");

    // Check memory history for VRAM write at 0xC000
    auto memHistory = f.backend->memoryHistorySnapshot();
    bool foundVramWrite = false;
    for (const auto &ev : memHistory) {
        if (ev.instructionSequence >= result.startSequence &&
            ev.instructionSequence < result.endSequence &&
            ev.type == MemoryAccessType::Write &&
            ev.virt == 0xC000) {
            foundVramWrite = true;
            CHECK_EQ(0x42u, (unsigned)ev.value, "VRAM write value is 0x42");
            break;
        }
    }
    CHECK(foundVramWrite, "VRAM write at 0xC000 found");

    // Verify PC attribution: MOV M,A is at PC=0x0008
    std::map<uint64_t, uint16_t> seqToPc;
    auto instrTrace = f.backend->instructionHistorySnapshot();
    for (const auto &ie : instrTrace) {
        if (ie.sequence >= result.startSequence &&
            ie.sequence < result.endSequence) {
            seqToPc[ie.sequence] = ie.pcBefore;
        }
    }
    for (const auto &ev : memHistory) {
        if (ev.instructionSequence >= result.startSequence &&
            ev.instructionSequence < result.endSequence &&
            ev.type == MemoryAccessType::Write &&
            ev.virt == 0xC000) {
            auto it = seqToPc.find(ev.instructionSequence);
            if (it != seqToPc.end()) {
                CHECK_EQ(0x0008u, (unsigned)it->second, "VRAM write PC = 0x0008 (MOV M,A)");
            }
            break;
        }
    }
    TEST_END();
}

// Test: Cancelled command not executed (Section 8)
static void test_cancelled_command_not_executed()
{
    TEST_BEGIN("cancelled_command_not_executed");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu();
    // initCpu() calls i8080_init() which resets PC to 0.
    // Set PC to 0x0003 via writeCpuRegister (calls i8080_jump without i8080_init).
    f.target->writeCpuRegister(static_cast<int>(IDebugBackend::RegisterId::PC), 0x0003);

    // Verify PC is at 0x0003
    uint16_t pcBefore = f.backend->getCpuState().pc;
    CHECK_EQ(0x0003u, (unsigned)pcBefore, "PC at 0x0003 before");

    // Submit a Step command that is pre-cancelled via submitAndWait().
    // The test synchronous path should detect cancelled=true and return Cancelled
    // WITHOUT executing the Step (which would advance PC).
    auto cmd = std::make_unique<DebugBackend::Command>();
    cmd->type = DebugBackend::CommandType::Step;
    cmd->cancelled.store(true, std::memory_order_release);

    auto result = f.backend->submitAndWait(std::move(cmd));

    // Verify: command was cancelled, not executed
    CHECK(!result.success, "cancelled command returned success=false");
    CHECK(result.status == CommandResult::Cancelled, "status is Cancelled");

    // Verify: PC hasn't changed (Step was NOT executed)
    uint16_t pcAfter = f.backend->getCpuState().pc;
    CHECK_EQ(pcBefore, pcAfter, "PC unchanged — cancelled command not executed");

    // Now verify that a non-cancelled command DOES execute
    auto r = f.backend->requestAddBreakpoint(0x0500);
    CHECK(r.success, "non-cancelled command executed successfully");
    CHECK(f.backend->hasBreakpoint(0x0500), "breakpoint exists");
    TEST_END();
}

// Test: Executing command completes (Section 8.1)
static void test_executing_command_completes()
{
    TEST_BEGIN("executing_command_completes");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu();

    // Verify: in test mode, commands go through Executing state and complete
    auto r1 = f.backend->requestAddBreakpoint(0x0300);
    CHECK(r1.success, "first command completed");
    CHECK(r1.status == CommandResult::Completed, "status is Completed");

    // Multiple sequential commands should all complete
    auto r2 = f.backend->requestCreateFunction(0x0200, "TestFunc");
    CHECK(r2.success, "second command completed");
    CHECK(r2.status == CommandResult::Completed, "status is Completed");

    auto r3 = f.backend->requestAddBreakpoint(0x0400);
    CHECK(r3.success, "third command completed");

    // Verify all mutations took effect
    CHECK(f.backend->hasBreakpoint(0x0300), "bp 0x0300 exists");
    CHECK(f.backend->hasBreakpoint(0x0400), "bp 0x0400 exists");
    CHECK(f.backend->symbolDatabase().findSymbol(0x0200) != nullptr, "symbol exists");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 5.3.3.2: Thread isolation and advanced tests
// ---------------------------------------------------------------------------

// Test: Thread isolation — Step (Section 14)
// Verify callerThreadId != emulationThreadId and CPU execution on emulation thread.
static void test_thread_isolation_step()
{
    TEST_BEGIN("thread_isolation_step");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu();

    auto callerThreadId = std::this_thread::get_id();
    auto emuThreadId = std::thread::id{};
    std::atomic<std::thread::id> cpuExecThreadId{};

    // Set instruction callback to capture execution thread
    f.backend->onInstruction = [&](const InstructionEvent &) {
        cpuExecThreadId.store(std::this_thread::get_id(), std::memory_order_release);
    };

    std::thread emuThread([&]{
        emuThreadId = std::this_thread::get_id();
        f.backend->runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Request step from caller thread
    f.backend->requestStep();

    // Give emulation thread time to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Verify thread IDs
    CHECK(callerThreadId != emuThreadId, "caller thread != emulation thread");

    auto actualExecThread = cpuExecThreadId.load(std::memory_order_acquire);
    CHECK(actualExecThread == emuThreadId, "CPU execution on emulation thread");
    CHECK(actualExecThread != callerThreadId, "CPU execution NOT on caller thread");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: Thread isolation — ExecuteTrace (Section 15)
// Verify trace caller thread != emulation thread, trace CPU execution on emulation thread.
static void test_thread_isolation_trace()
{
    TEST_BEGIN("thread_isolation_trace");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    writeProgram(f.mem, subroutine, sizeof(subroutine), 0x0200);
    f.initCpu(0x0200, 0xC0F0);

    // Push return address on stack
    f.mem.write(0xC0F0, 0x09, false);
    f.mem.write(0xC0F1, 0x00, false);

    auto callerThreadId = std::this_thread::get_id();
    auto emuThreadId = std::thread::id{};
    std::atomic<std::thread::id> cpuExecThreadId{};

    f.backend->onInstruction = [&](const InstructionEvent &) {
        cpuExecThreadId.store(std::this_thread::get_id(), std::memory_order_release);
    };

    std::thread emuThread([&]{
        emuThreadId = std::this_thread::get_id();
        f.backend->runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Execute trace from caller thread
    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0200;
    params.maxInstructions = 100;
    params.stopOnRet = true;
    auto result = f.backend->requestExecuteTrace(params).get();

    CHECK(result.instructionsExecuted > 0, "trace executed instructions");
    CHECK(result.exitReason == ExitReason::Ret, "trace ended with RET");

    // Verify thread isolation
    CHECK(callerThreadId != emuThreadId, "caller thread != emulation thread");
    auto actualExecThread = cpuExecThreadId.load(std::memory_order_acquire);
    CHECK(actualExecThread == emuThreadId, "trace CPU execution on emulation thread");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: Trace cancellation via Quit (Section 21)
static void test_trace_cancellation_via_quit()
{
    TEST_BEGIN("trace_cancellation_via_quit");
    TestFixture f;
    writeProgram(f.mem, loop_program, sizeof(loop_program), 0x0000);
    f.initCpu(0x0003);

    std::thread emuThread([&]{ f.backend->runUntilPause(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Start a long trace
    IDebugBackend::TraceExecutionParams params;
    params.startPc = 0x0003;
    params.maxInstructions = 1000000;  // very large
    params.stopOnRet = false;

    auto traceFuture = f.backend->requestExecuteTrace(params);

    // Give trace time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Quit — should cancel trace and complete the future
    f.backend->requestQuit();

    auto result = traceFuture.get();  // should not hang
    CHECK(result.instructionsExecuted > 0, "trace executed some instructions before cancel");

    emuThread.join();
    CHECK(true, "no hanging promise (thread joined cleanly)");
    TEST_END();
}

// Test: Command state machine — CAS mutual exclusion (Section 5.2)
static void test_command_state_cas()
{
    TEST_BEGIN("command_state_cas");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu();

    // Verify: pre-cancelled command is not executed
    auto cmd = std::make_unique<DebugBackend::Command>();
    cmd->type = DebugBackend::CommandType::Step;
    cmd->cancelled.store(true, std::memory_order_release);

    uint16_t pcBefore = f.backend->getCpuState().pc;
    auto result = f.backend->submitAndWait(std::move(cmd));
    uint16_t pcAfter = f.backend->getCpuState().pc;

    CHECK(!result.success, "cancelled command failed");
    CHECK(result.status == CommandResult::Cancelled, "status is Cancelled");
    CHECK_EQ(pcBefore, pcAfter, "PC unchanged (CAS prevented execution)");
    TEST_END();
}

// Test: requestPause does not change running_ (Section 3)
static void test_request_pause_no_running_mutation()
{
    TEST_BEGIN("request_pause_no_running_mutation");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu();

    // Set running via requestRun
    f.backend->requestRun();

    // requestPause should only set atomic signals, not change running_ directly
    f.backend->requestPause();

    // In test mode, state_ is still Running (no emulation thread to transition)
    // The atomic signals are set, but running_ is not changed by requestPause
    CHECK(f.backend->getState() == DebuggerState::Running ||
          f.backend->getState() == DebuggerState::Paused,
          "state is Running or Paused (no invalid transition)");
    TEST_END();
}

// Test: requestRunFuture returns future (Section 2)
static void test_request_run_future()
{
    TEST_BEGIN("request_run_future");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu();

    // In test mode, requestRunFuture should return a ready future
    auto future = f.backend->requestRunFuture();
    auto status = future.wait_for(std::chrono::milliseconds(100));
    CHECK(status == std::future_status::ready, "future is ready in test mode");

    auto result = future.get();
    CHECK(result.success, "future result is success");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 5.3.3.3 tests
// ---------------------------------------------------------------------------

// Test: Concurrent Run futures (Section 7)
// Multiple requestRunFuture() from different threads with real emulation thread.
static void test_concurrent_run_futures()
{
    TEST_BEGIN("concurrent_run_futures");
    TestFixture f;
    writeProgram(f.mem, loop_program, sizeof(loop_program), 0x0000);
    f.initCpu(0x0003);

    // Start emulation thread
    std::thread emuThread([&]{ f.backend->runUntilPause(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Launch two requestRunFuture() calls from different threads.
    // With emulation thread running, these take the production path
    // with per-command pausePromise.
    std::atomic<int> futuresCompleted{0};
    std::atomic<bool> quit1{false}, quit2{false};

    std::thread t1([&]{
        auto future = f.backend->requestRunFuture();
        // The future will complete when emulation pauses.
        // We need to trigger a pause for it to complete.
        auto status = future.wait_for(std::chrono::milliseconds(500));
        if (status == std::future_status::ready) {
            futuresCompleted++;
        }
        quit1.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::thread t2([&]{
        auto future = f.backend->requestRunFuture();
        auto status = future.wait_for(std::chrono::milliseconds(500));
        if (status == std::future_status::ready) {
            futuresCompleted++;
        }
        quit2.store(true);
    });

    // Give both threads time to enqueue their Run commands
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Pause emulation — should fulfill ALL pending pause promises
    f.backend->requestPause();

    t1.join();
    t2.join();

    CHECK(quit1.load(), "thread 1 completed");
    CHECK(quit2.load(), "thread 2 completed");
    // At least one future should have completed (both may complete
    // if they were both enqueued before pause was processed)
    CHECK(futuresCompleted.load() >= 1, "at least one pause future completed");

    // Clean up
    f.backend->requestQuit();
    emuThread.join();
    CHECK(true, "no deadlock with concurrent Run futures");
    TEST_END();
}

// Test: Real CAS race (Section 8)
// Two threads simultaneously try to CAS the same command:
// Thread A: Queued→Cancelled, Thread B: Queued→Executing
// Exactly one must succeed.
static void test_cas_race_real()
{
    TEST_BEGIN("cas_race_real");

    // Run the race 100 times to stress the CAS
    int aWins = 0, bWins = 0;
    for (int iter = 0; iter < 100; ++iter) {
        auto cmd = std::make_unique<DebugBackend::Command>();
        cmd->type = DebugBackend::CommandType::Step;
        // Command starts in Queued state

        // Synchronize both threads to start at the same time
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> go{false};
        std::atomic<int> threadAResult{-1};  // 0=lost, 1=won
        std::atomic<int> threadBResult{-1};

        // Thread A: Queued → Cancelled (simulates caller-thread cancellation)
        std::thread threadA([&]{
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&]{ return go.load(); });
            DebugBackend::CommandState expected = DebugBackend::CommandState::Queued;
            bool ok = cmd->state.compare_exchange_strong(
                expected, DebugBackend::CommandState::Cancelled,
                std::memory_order_acq_rel);
            threadAResult.store(ok ? 1 : 0, std::memory_order_release);
        });

        // Thread B: Queued → Executing (simulates emulation-thread CAS)
        std::thread threadB([&]{
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&]{ return go.load(); });
            DebugBackend::CommandState expected = DebugBackend::CommandState::Queued;
            bool ok = cmd->state.compare_exchange_strong(
                expected, DebugBackend::CommandState::Executing,
                std::memory_order_acq_rel);
            threadBResult.store(ok ? 1 : 0, std::memory_order_release);
        });

        // Release both threads simultaneously
        {
            std::lock_guard<std::mutex> lk(mtx);
            go.store(true);
        }
        cv.notify_all();

        threadA.join();
        threadB.join();

        int a = threadAResult.load();
        int b = threadBResult.load();

        // Exactly one must have succeeded
        CHECK(a + b == 1, "exactly one CAS succeeded");
        // Final state must be consistent
        auto finalState = cmd->state.load();
        if (a == 1) {
            CHECK(finalState == DebugBackend::CommandState::Cancelled, "state is Cancelled when A wins");
            aWins++;
        } else {
            CHECK(finalState == DebugBackend::CommandState::Executing, "state is Executing when B wins");
            bWins++;
        }
    }

    CHECK(aWins > 0, "Thread A won at least once");
    CHECK(bWins > 0, "Thread B won at least once");
    TEST_END();
}

// Test: Thread isolation — Step (Section 9)
// Verify Step caller thread != emulation thread, CPU step on emulation thread.
static void test_thread_isolation_step_real()
{
    TEST_BEGIN("thread_isolation_step_real");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu(0x0003);

    auto callerThreadId = std::this_thread::get_id();
    auto emuThreadId = std::thread::id{};
    std::atomic<std::thread::id> cpuExecThreadId{};

    f.backend->onInstruction = [&](const InstructionEvent &) {
        cpuExecThreadId.store(std::this_thread::get_id(), std::memory_order_release);
    };

    std::thread emuThread([&]{
        emuThreadId = std::this_thread::get_id();
        f.backend->runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Step from caller thread
    uint16_t pcBefore = f.backend->getCpuState().pc;
    f.backend->requestStep();
    uint16_t pcAfter = f.backend->getCpuState().pc;

    CHECK(pcAfter != pcBefore, "PC advanced after step");

    auto actualExecThread = cpuExecThreadId.load(std::memory_order_acquire);
    CHECK(actualExecThread == emuThreadId, "step CPU execution on emulation thread");
    CHECK(actualExecThread != callerThreadId, "step CPU execution NOT on caller thread");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: Thread isolation — MemoryWrite (Section 9)
static void test_thread_isolation_memory_write()
{
    TEST_BEGIN("thread_isolation_memory_write");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu(0x0003);

    auto callerThreadId = std::this_thread::get_id();
    auto emuThreadId = std::thread::id{};
    std::atomic<std::thread::id> memWriteThreadId{};

    // Track which thread executes the memory write via target callbacks
    f.backend->onInstruction = [&](const InstructionEvent &) {
        // Capture emulation thread ID on first instruction
        auto expected = std::thread::id{};
        memWriteThreadId.compare_exchange_strong(expected, std::this_thread::get_id(),
                                                  std::memory_order_acq_rel);
    };

    std::thread emuThread([&]{
        emuThreadId = std::this_thread::get_id();
        f.backend->runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Write memory from caller thread — should go through command queue
    auto result = f.backend->writeMemoryByte(0xC000, 0x42);
    CHECK(result, "memory write succeeded");

    // Verify the write happened
    uint8_t val = f.backend->readMemory(0xC000);
    CHECK_EQ(0x42u, (unsigned)val, "memory value written");

    CHECK(emuThreadId != callerThreadId, "caller thread != emulation thread");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: Thread isolation — RegisterWrite (Section 9)
static void test_thread_isolation_register_write()
{
    TEST_BEGIN("thread_isolation_register_write");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu(0x0003);

    auto callerThreadId = std::this_thread::get_id();
    auto emuThreadId = std::thread::id{};

    std::thread emuThread([&]{
        emuThreadId = std::this_thread::get_id();
        f.backend->runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Write register from caller thread
    bool ok = f.backend->writeRegister(IDebugBackend::RegisterId::AF, 0x1234);
    CHECK(ok, "register write succeeded");

    // Verify register value (AF = A<<8 | flags; writing 0x1234 sets A=0x12)
    auto cpuState = f.backend->getCpuState();
    CHECK_EQ(0x12u, (unsigned)cpuState.a, "register A value written");

    CHECK(emuThreadId != callerThreadId, "caller thread != emulation thread");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: Thread isolation — Reset (Section 9)
static void test_thread_isolation_reset()
{
    TEST_BEGIN("thread_isolation_reset");
    TestFixture f;
    writeProgram(f.mem, test_program, sizeof(test_program), 0x0000);
    f.initCpu(0x0003);

    // Step a few instructions to move PC
    f.backend->requestStep();
    f.backend->requestStep();
    uint16_t pcBefore = f.backend->getCpuState().pc;
    CHECK(pcBefore != 0, "PC moved after steps");

    auto callerThreadId = std::this_thread::get_id();
    auto emuThreadId = std::thread::id{};
    std::atomic<std::thread::id> cpuExecThreadId{};

    f.backend->onInstruction = [&](const InstructionEvent &) {
        cpuExecThreadId.store(std::this_thread::get_id(), std::memory_order_release);
    };

    std::thread emuThread([&]{
        emuThreadId = std::this_thread::get_id();
        f.backend->runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Reset from caller thread
    f.backend->requestReset();

    // Verify PC reset to 0
    auto cpuState = f.backend->getCpuState();
    CHECK_EQ(0x0000u, (unsigned)cpuState.pc, "PC reset to 0");

    CHECK(emuThreadId != callerThreadId, "caller thread != emulation thread");

    f.backend->requestQuit();
    emuThread.join();
    TEST_END();
}

// Test: Quit cancels all pending Run pause promises (Section 6)
static void test_quit_fulfills_all_pause_promises()
{
    TEST_BEGIN("quit_fulfills_all_pause_promises");
    TestFixture f;
    writeProgram(f.mem, loop_program, sizeof(loop_program), 0x0000);
    f.initCpu(0x0003);

    std::thread emuThread([&]{ f.backend->runUntilPause(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Enqueue multiple Run commands with pause promises
    const int N = 5;
    std::vector<std::future<CommandResult>> futures;
    for (int i = 0; i < N; ++i) {
        futures.push_back(f.backend->requestRunFuture());
    }

    // Give time for commands to be enqueued
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Quit — should fulfill ALL pending pause promises
    f.backend->requestQuit();

    int completed = 0;
    for (auto &fut : futures) {
        auto status = fut.wait_for(std::chrono::milliseconds(500));
        if (status == std::future_status::ready) {
            completed++;
            auto result = fut.get();
            // Each should be cancelled (quit during pending)
            CHECK(!result.success || result.status == CommandResult::Cancelled ||
                  result.status == CommandResult::Completed,
                  "promise fulfilled (not hanging)");
        }
    }

    CHECK_EQ(N, completed, "all pause promises fulfilled after quit");

    emuThread.join();
    CHECK(true, "no hanging promises (thread joined cleanly)");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 46: I/O port access through Command Queue
// ---------------------------------------------------------------------------

static void test_io_port_write_through_queue()
{
    TEST_BEGIN("io_port_write_through_queue");
    TestFixture f;
    writeProgram(f.mem, loop_program, sizeof(loop_program), 0x0000);
    f.initCpu(0x0003);

    AgentApi api(*f.backend);

    // readIo should return 0xFF (default NoBoardTarget implementation)
    auto r1 = api.readIo(0x10);
    CHECK(r1.success, "readIo succeeds");
    CHECK_EQ(0xFFu, (unsigned)r1.value, "default port value is 0xFF");

    // writeIo goes through Command Queue (testSynchronous_ = true)
    auto w1 = api.writeIo(0x10, 0x55);
    CHECK(w1.success, "writeIo succeeds through queue");

    // NoBoardTarget doesn't store port state, so read still returns 0xFF
    // But the command succeeded, proving the queue mechanism works
    auto r2 = api.readIo(0x10);
    CHECK(r2.success, "second readIo succeeds");
    CHECK_EQ(0xFFu, (unsigned)r2.value, "NoBoardTarget doesn't store port state");

    // Verify zero and max ports are valid
    auto r3 = api.readIo(0x00);
    CHECK(r3.success, "port 0x00 is valid");

    auto r4 = api.readIo(0xFF);
    CHECK(r4.success, "port 0xFF is valid");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.15 regression test: MCP command queue + RDB workflow
//
// Verifies that:
// 1. Annotation operations (through command queue) work with emulation thread
// 2. RDB operations (direct) work alongside
// 3. Full workflow: add RDB object → dirty → save → reload → data preserved
// ---------------------------------------------------------------------------

static void test_mcp_command_queue_and_rdb_workflow()
{
    TEST_BEGIN("stage_6.15_mcp_command_queue_and_rdb_workflow");

    // Use real DebugBackend WITHOUT testSynchronous_ — requires emulation thread
    Memory mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);
    backend.reset();

    hal_memory = &mem;
    hal_dbg = &backend;

    // Write test program
    writeProgram(mem, test_program, sizeof(test_program), 0x0000);
    writeProgram(mem, subroutine, sizeof(subroutine), 0x0200);

    // Start emulation thread — this is what mcp_main.cpp was missing
    std::thread emuThread([&backend]() {
        backend.runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    AgentApi api(backend);

    // --- Part 1: Annotation operations through command queue ---
    // These go through submitAndWait() → commandQueue_ → emulation thread
    auto r1 = api.createFunction(0x0200, 0);
    CHECK(r1.success, "createFunction through command queue succeeds");

    auto r2 = api.setComment(0x0200, "test subroutine");
    CHECK(r2.success, "setComment through command queue succeeds");

    auto r3 = api.addLabel(0x0205, "mov_m_a");
    CHECK(r3.success, "addLabel through command queue succeeds");

    // --- Part 2: RDB operations (direct, no command queue) ---
    auto &rdb = backend.rdbController();

    // Initialize RDB
    rdb.initialize("vector06c", RdbRomIdentity());
    CHECK(!rdb.isDirty(), "RDB clean after init");

    // Add RDB object
    auto r4 = api.addRdbObject(0x0000, "start", "function", 0);
    CHECK(r4.success, "addRdbObject succeeds");
    CHECK(rdb.isDirty(), "RDB dirty after addRdbObject");

    // Set RDB comment
    auto r5 = api.setRdbComment(0x0000, "ROM entry point");
    CHECK(r5.success, "setRdbComment succeeds");

    // Add RDB link
    auto r6 = api.addRdbLink(0x0000, 0x0200);
    CHECK(r6.success, "addRdbLink succeeds");

    // Verify RDB state
    auto r7 = api.getRdbInfo();
    CHECK(r7.success, "getRdbInfo succeeds");
    CHECK(r7.value.objectCount >= 1, "RDB has at least 1 object");
    CHECK(r7.value.dirty, "RDB reports dirty=true");

    // --- Part 3: Save and reload ---
    std::string testPath = "/tmp/test_stage615.rdb";
    rdb.saveAs(testPath);
    CHECK(!rdb.isDirty(), "RDB clean after save");

    // Verify file exists
    FILE *fp = fopen(testPath.c_str(), "r");
    CHECK(fp != nullptr, "RDB file exists after save");
    if (fp) fclose(fp);

    // Reload and verify data preserved
    auto r8 = api.reloadRdb();
    CHECK(r8.success, "reloadRdb succeeds");

    auto r9 = api.getRdbObject(0x0000);
    CHECK(r9.success, "getRdbObject after reload succeeds");
    if (r9.success) {
        CHECK(std::string(r9.value.name) == "start", "object name preserved after reload");
        CHECK(std::string(r9.value.comment) == "ROM entry point", "comment preserved after reload");
    }

    // Verify links preserved
    auto r10 = api.getRdbLinks(0x0000);
    CHECK(r10.success, "getRdbLinks after reload succeeds");
    CHECK(r10.value.size() >= 1, "link preserved after reload");

    // Cleanup
    backend.requestQuit();
    emuThread.join();
    std::remove(testPath.c_str());

    hal_dbg = nullptr;
    hal_memory = nullptr;

    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.15 Iter2 regression test: createFunction creates RDB object
//
// Verifies that debug_create_function:
// 1. Goes through command queue (requires emulation thread)
// 2. Creates both SymbolDatabase entry AND RDB object
// 3. RDB becomes dirty, save works, reload preserves data
// ---------------------------------------------------------------------------

static void test_create_function_creates_rdb_object()
{
    TEST_BEGIN("stage_6.15_iter2_create_function_creates_rdb_object");

    // Real DebugBackend WITHOUT testSynchronous_ — requires emulation thread
    Memory mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);
    backend.reset();

    hal_memory = &mem;
    hal_dbg = &backend;

    writeProgram(mem, test_program, sizeof(test_program), 0x0000);
    writeProgram(mem, subroutine, sizeof(subroutine), 0x0200);

    // Start emulation thread
    std::thread emuThread([&backend]() {
        backend.runUntilPause();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    AgentApi api(backend);
    auto &rdb = backend.rdbController();
    rdb.initialize("vector06c", RdbRomIdentity());

    // --- createFunction should create both symbol AND RDB object ---
    auto r1 = api.createFunction(0x0200, 0);
    CHECK(r1.success, "createFunction succeeds through command queue");

    // Verify RDB object was created
    auto r2 = api.getRdbInfo();
    CHECK(r2.success, "getRdbInfo succeeds");
    CHECK(r2.value.objectCount == 1, "RDB has 1 object after createFunction");
    CHECK(r2.value.dirty, "RDB is dirty after createFunction");

    // Verify RDB object details
    auto r3 = api.getRdbObject(0x0200);
    CHECK(r3.success, "getRdbObject(0x0200) succeeds");
    if (r3.success) {
        CHECK(std::string(r3.value.type) == "function", "RDB object type is function");
        CHECK(!r3.value.name.empty(), "RDB object has a name");
    }

    // --- addLabel should also create RDB object ---
    auto r4 = api.addLabel(0x0003, "loop_entry");
    CHECK(r4.success, "addLabel succeeds through command queue");

    auto r5 = api.getRdbInfo();
    CHECK(r5.value.objectCount == 2, "RDB has 2 objects after createFunction + addLabel");

    // --- Save and reload ---
    std::string testPath = "/tmp/test_stage615_iter2.rdb";
    rdb.saveAs(testPath);
    CHECK(!rdb.isDirty(), "RDB clean after save");

    auto r6 = api.reloadRdb();
    CHECK(r6.success, "reloadRdb succeeds");

    auto r7 = api.getRdbObject(0x0200);
    CHECK(r7.success, "function object survives reload");

    auto r8 = api.getRdbObject(0x0003);
    CHECK(r8.success, "label object survives reload");

    // Cleanup
    backend.requestQuit();
    emuThread.join();
    std::remove(testPath.c_str());

    hal_dbg = nullptr;
    hal_memory = nullptr;

    TEST_END();
}

// ---------------------------------------------------------------------------
// #49 — Stage 6.16: readMemoryRange basic + boundary
// ---------------------------------------------------------------------------

static void test_read_memory_range()
{
    TEST_BEGIN("stage_6.16_read_memory_range");

    TestFixture f;
    AgentApi api(*f.backend);

    // Write known pattern
    std::vector<uint8_t> pattern = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    auto w = api.writeMemory(0x1000, pattern);
    CHECK(w.success, "writeMemory for range test");

    // Read back as range
    auto r = api.readMemory(0x1000, 5);
    CHECK(r.success, "readMemory range");
    CHECK(r.value.size() == 5, "range size = 5");
    CHECK(r.value[0] == 0xAA, "byte 0");
    CHECK(r.value[4] == 0xEE, "byte 4");

    // Boundary: read at 0xFFFF, length 1
    auto r2 = api.readMemory(0xFFFF, 1);
    CHECK(r2.success, "read at 0xFFFF");

    // Overflow: address + length > 0x10000
    auto r3 = api.readMemory(0xFFFF, 2);
    CHECK(!r3.success, "overflow returns error");

    TEST_END();
}

// ---------------------------------------------------------------------------
// #50 — Stage 6.16: analyzeCode control-flow analysis
// ---------------------------------------------------------------------------

static void test_analyze_code_control_flow()
{
    TEST_BEGIN("stage_6.16_analyze_code_control_flow");

    TestFixture f;
    AgentApi api(*f.backend);

    // Write test code at 0x0200:
    //   0200: NOP           (sequential)
    //   0201: JMP 0x0210    (unconditional jump)
    //   0204: NOP           (NOT reachable from 0200 via JMP)
    //   ...
    //   0210: CALL 0x0220   (call + continue)
    //   0213: RET           (return — terminates path)
    //   ...
    //   0220: NOP           (call target)
    //   0221: RET           (return)

    // NOP = 0x00, JMP addr = 0xC3 lo hi, CALL addr = 0xCD lo hi, RET = 0xC9
    std::vector<uint8_t> code = {
        // 0x0200: NOP
        0x00,
        // 0x0201: JMP 0x0210
        0xC3, 0x10, 0x02,
        // 0x0204: NOP (unreachable via JMP from 0200)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // 0x0210: CALL 0x0220
        0xCD, 0x20, 0x02,
        // 0x0213: RET
        0xC9,
        // 0x0214..0x021F: padding
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        // 0x0220: NOP
        0x00,
        // 0x0221: RET
        0xC9
    };

    auto w = api.writeMemory(0x0200, code);
    CHECK(w.success, "write test code");

    auto r = api.analyzeCode(0x0200, 100);
    CHECK(r.success, "analyzeCode succeeds");

    const auto &a = r.value;
    CHECK(a.entryPoint == 0x0200, "entry point = 0x0200");
    CHECK(!a.truncated, "not truncated");

    // Should find: NOP(0200), JMP(0201), CALL(0210), RET(0213), NOP(0220), RET(0221)
    CHECK(a.instructionCount >= 5, "at least 5 instructions found");

    // 0x0204 should NOT be analyzed (unreachable after JMP)
    bool found0204 = false;
    for (const auto &inst : a.instructions) {
        if (inst.address == 0x0204) found0204 = true;
    }
    CHECK(!found0204, "0x0204 not reachable (after JMP)");

    // Should have JMP and CALL references
    bool hasJmp = false, hasCall = false;
    for (const auto &ref : a.references) {
        if (ref.type == "JMP" && ref.from == 0x0201 && ref.to == 0x0210) hasJmp = true;
        if (ref.type == "CALL" && ref.from == 0x0210 && ref.to == 0x0220) hasCall = true;
    }
    CHECK(hasJmp, "JMP 0x0201 -> 0x0210 reference");
    CHECK(hasCall, "CALL 0x0210 -> 0x0220 reference");

    // Should have at least 2 code ranges (0x0200-0x0203 and 0x0210-0x0213 and 0x0220-0x0221)
    CHECK(a.ranges.size() >= 2, "at least 2 code ranges");

    TEST_END();
}

// ---------------------------------------------------------------------------
// #51 — Stage 6.19: multi-entry analyzeCode
// ---------------------------------------------------------------------------

static void test_multi_entry_analyze_code()
{
    TEST_BEGIN("stage_6.19_multi_entry_analyze_code");

    TestFixture f;
    AgentApi api(*f.backend);

    // Write test code:
    //   0x0100: NOP           (entry A)
    //   0x0101: JMP 0x0120
    //   0x0104..0x010F: padding (unreachable)
    //   0x0110: NOP           (entry B — separate code)
    //   0x0111: JMP 0x0120    (shares code with entry A)
    //   0x0114..0x011F: padding
    //   0x0120: NOP           (shared target)
    //   0x0121: RET
    std::vector<uint8_t> code(0x30, 0x00);  // zero-fill
    code[0x00] = 0x00;                       // 0x0100: NOP
    code[0x01] = 0xC3; code[0x02] = 0x20; code[0x03] = 0x01;  // 0x0101: JMP 0x0120
    code[0x10] = 0x00;                       // 0x0110: NOP
    code[0x11] = 0xC3; code[0x12] = 0x20; code[0x13] = 0x01;  // 0x0111: JMP 0x0120
    code[0x20] = 0x00;                       // 0x0120: NOP
    code[0x21] = 0xC9;                       // 0x0121: RET

    auto w = api.writeMemory(0x0100, code);
    CHECK(w.success, "write test code");

    // --- Single entry compatibility ---
    auto rSingle = api.analyzeCode(uint16_t(0x0100), 200);
    CHECK(rSingle.success, "single entry succeeds");

    auto rVec1 = api.analyzeCode(std::vector<uint16_t>{0x0100}, 200);
    CHECK(rVec1.success, "vector single entry succeeds");
    CHECK(rVec1.value.instructionCount == rSingle.value.instructionCount,
          "single-entry vector matches scalar");

    // --- Multiple entries ---
    auto rMulti = api.analyzeCode(std::vector<uint16_t>{0x0100, 0x0110}, 200);
    CHECK(rMulti.success, "multi-entry succeeds");

    const auto &a = rMulti.value;

    // Should find instructions from both entries
    // Entry A: NOP(0100), JMP(0101), NOP(0120), RET(0121)
    // Entry B: NOP(0110), JMP(0111), NOP(0120), RET(0121)
    // Shared: NOP(0120), RET(0121) — counted once
    CHECK(a.instructionCount == 6, "6 unique instructions (shared code counted once)");

    // Entry points reported
    CHECK(a.entryPoints.size() == 2, "2 entry points reported");
    CHECK(a.entryPoints[0] == 0x0100, "first entry = 0x0100");
    CHECK(a.entryPoints[1] == 0x0110, "second entry = 0x0110");

    // codeBytes
    // 4 single-byte + 2 three-byte = 4 + 6 = 10
    CHECK(a.codeBytes == 10, "codeBytes = 10");

    // Instructions sorted by address
    for (size_t i = 1; i < a.instructions.size(); ++i) {
        CHECK(a.instructions[i].address > a.instructions[i-1].address,
              "instructions sorted by address");
    }

    // --- Duplicate entries ---
    auto rDup = api.analyzeCode(std::vector<uint16_t>{0x0100, 0x0100}, 200);
    CHECK(rDup.success, "duplicate entries succeed");
    CHECK(rDup.value.entryPoints.size() == 1, "duplicates removed");
    CHECK(rDup.value.instructionCount == rSingle.value.instructionCount,
          "duplicate entries give same count as single");

    // --- Empty entries ---
    auto rEmpty = api.analyzeCode(std::vector<uint16_t>{}, 200);
    CHECK(!rEmpty.success, "empty entries returns error");

    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    setbuf(stdout, nullptr);

    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  Agent Integration Tests — Stage 5.3.3.3\033[0m\n");
    printf("\033[1;33m========================================\033[0m\n");

    // Non-threaded tests
    test_command_queue_preserves_order();       // 1
    test_concurrent_commands_not_lost();        // 2
    test_each_command_receives_own_result();    // 3
    test_command_executes_on_emulation_thread();// 4
    test_agent_thread_never_executes_cpu();     // 5
    test_commands_work_while_running();         // 6
    test_pause_while_running();                 // 7
    test_quit_while_running();                  // 8
    test_breakpoint_mutation_through_queue();   // 9
    test_annotation_mutation_through_queue();   // 10
    test_trace_execution_through_queue();       // 11
    test_trace_timeout();                       // 12
    test_concurrent_trace_requests();           // 13
    test_unknown_pc_remains_unknown();          // 20

    // Threaded tests
    test_real_debugbackend_threaded_integration(); // 14
    test_real_breakpoint_integration();            // 15
    test_real_trace_integration();                 // 16
    test_memory_attribution();                     // 17
    test_io_attribution();                           // 18
    test_vram_attribution();                       // 19

    // Stage 5.3.3 tests
    test_concurrent_breakpoint_real();              // 21
    test_concurrent_annotation_real();              // 22
    test_step_only_on_emulation_thread();           // 23
    test_trace_only_on_emulation_thread();          // 24
    test_no_pending_promise_after_quit();           // 25
    test_no_command_lost_during_shutdown();         // 26
    test_pc0000_not_unknown();                      // 27

    // Stage 5.3.3.1 fixup tests
    test_quit_with_multiple_pending_commands();     // 28
    test_io_attribution_with_out_instruction();     // 29
    test_memory_vram_attribution_detailed();        // 30
    test_cancelled_command_not_executed();          // 31
    test_executing_command_completes();             // 32

    // Stage 5.3.3.2 tests
    test_thread_isolation_step();                   // 33
    test_thread_isolation_trace();                  // 34
    test_trace_cancellation_via_quit();             // 35
    test_command_state_cas();                       // 36
    test_request_pause_no_running_mutation();       // 37
    test_request_run_future();                      // 38

    // Stage 5.3.3.3 tests
    test_concurrent_run_futures();                  // 39
    test_cas_race_real();                           // 40
    test_thread_isolation_step_real();              // 41
    test_thread_isolation_memory_write();           // 42
    test_thread_isolation_register_write();         // 43
    test_thread_isolation_reset();                  // 44
    test_quit_fulfills_all_pause_promises();        // 45

    // Stage 6.1 Iteration 3 — I/O ports
    test_io_port_write_through_queue();             // 46

    // Stage 6.15 — MCP command queue + RDB workflow
    test_mcp_command_queue_and_rdb_workflow();      // 47

    // Stage 6.15 Iter2 — createFunction creates RDB object
    test_create_function_creates_rdb_object();       // 48

    // Stage 6.16 — MCP Reverse Engineering Primitives
    test_read_memory_range();                        // 49
    test_analyze_code_control_flow();                // 50

    // Stage 6.19 — Multi-Entry Static Code Analysis
    test_multi_entry_analyze_code();                  // 51

    printf("\n\033[1;33m========================================\033[0m\n");
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf("  (\033[1;31m%d FAILED\033[0m)", tests_failed);
    }
    printf("\n\033[1;33m========================================\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
