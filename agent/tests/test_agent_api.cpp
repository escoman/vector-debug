// Agent API tests — Stage 5.3 / Stage 6.1
//
// Unit tests for AgentApi using MockAgentBackend.
// No Board, SDL, or emulator dependency.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "agent_api.h"
#include "mock_backend_for_agent.h"

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

#define CHECK_STR(exp, act, msg) \
        do { \
            std::string _e(exp); \
            std::string _a(act); \
            if (_e != _a) { \
                printf("  \033[41;97m FAIL \033[0m %s: expected \"%s\", got \"%s\" (line %d)\n", \
                       msg, _e.c_str(), _a.c_str(), __LINE__); \
                _test_ok = false; \
            } else { \
                printf("  \033[46;30m ok \033[0m %s = \"%s\"\n", msg, _a.c_str()); \
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
// Tests: Execution control
// ---------------------------------------------------------------------------

static void test_run_pause()
{
    TEST_BEGIN("run and pause");
    MockAgentBackend mock;

    // Fill memory with NOPs so requestRun() doesn't hit HLT
    std::vector<uint8_t> nops(256, 0x00);
    mock.setMemory(0x0100, nops);

    AgentApi api(mock);

    CHECK(mock.isPaused(), "initially paused");

    auto runResult = api.run();
    CHECK(runResult.success, "run succeeds");
    // Mock requestRun() is synchronous — it runs until HLT/breakpoint/limit.
    // With NOPs it hits the 100000-step safety limit, then is paused again.
    // So we verify that stepping occurred (PC advanced far from 0x0100).
    CHECK(api.getCpuState().value.pc != 0x0100, "PC advanced during run");

    auto pauseResult = api.pause();
    CHECK(pauseResult.success, "pause succeeds");
    CHECK(mock.isPaused(), "paused after pause()");
    TEST_END();
}

static void test_step()
{
    TEST_BEGIN("step advances PC");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto beforeResult = api.getCpuState();
    CHECK(beforeResult.success, "getCpuState succeeds");
    CHECK_EQ(0x0100u, (unsigned)beforeResult.value.pc, "PC at 0x0100");

    auto stepResult = api.step();
    CHECK(stepResult.success, "step succeeds");

    auto afterResult = api.getCpuState();
    // 0x0100 is LXI SP,word (3 bytes) → PC should be 0x0103
    CHECK_EQ(0x0103u, (unsigned)afterResult.value.pc, "PC advanced to 0x0103");
    TEST_END();
}

static void test_reset()
{
    TEST_BEGIN("reset returns PC to start");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.step();
    api.step();
    CHECK(api.getCpuState().value.pc != 0x0100, "PC moved");

    auto resetResult = api.reset();
    CHECK(resetResult.success, "reset succeeds");
    CHECK_EQ(0x0100u, (unsigned)api.getCpuState().value.pc, "PC back to 0x0100");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: CPU state
// ---------------------------------------------------------------------------

static void test_get_cpu_state()
{
    TEST_BEGIN("getCpuState returns current CPU state");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto result = api.getCpuState();
    CHECK(result.success, "succeeds");
    CpuState cpu = result.value;
    CHECK_EQ(0x0100u, (unsigned)cpu.pc, "PC");
    CHECK_EQ(0xF800u, (unsigned)cpu.sp, "SP");
    CHECK_EQ(0x42u, (unsigned)cpu.a, "A");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Memory access
// ---------------------------------------------------------------------------

static void test_read_memory()
{
    TEST_BEGIN("readMemory returns bytes");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto result = api.readMemory(0x0100, 4);
    CHECK(result.success, "succeeds");
    CHECK_EQ(4u, result.value.size(), "4 bytes");
    CHECK_EQ(0x31u, (unsigned)result.value[0], "first byte");
    TEST_END();
}

static void test_write_memory()
{
    TEST_BEGIN("writeMemory modifies memory");
    MockAgentBackend mock;
    AgentApi api(mock);

    std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};
    auto writeResult = api.writeMemory(0x8000, data);
    CHECK(writeResult.success, "write succeeds");

    auto readResult = api.readMemory(0x8000, 3);
    CHECK(readResult.success, "read succeeds");
    CHECK_EQ(0xAAu, (unsigned)readResult.value[0], "byte 0");
    CHECK_EQ(0xBBu, (unsigned)readResult.value[1], "byte 1");
    CHECK_EQ(0xCCu, (unsigned)readResult.value[2], "byte 2");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: I/O ports (Stage 6.1 Iteration 3)
// ---------------------------------------------------------------------------

static void test_read_io()
{
    TEST_BEGIN("readIo returns port value");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Pre-set port value
    mock.setIoPort(0x10, 0x55);

    auto r = api.readIo(0x10);
    CHECK(r.success, "read succeeds");
    CHECK_EQ(0x55u, (unsigned)r.value, "value is 0x55");
    TEST_END();
}

static void test_write_io()
{
    TEST_BEGIN("writeIo modifies port");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.writeIo(0x10, 0xAA);
    CHECK(r.success, "write succeeds");
    CHECK_EQ(0xAAu, (unsigned)mock.getIoPort(0x10), "port value is 0xAA");
    TEST_END();
}

static void test_io_zero_port()
{
    TEST_BEGIN("I/O port 0x00 is valid");
    MockAgentBackend mock;
    AgentApi api(mock);

    mock.setIoPort(0x00, 0x42);
    auto r = api.readIo(0x00);
    CHECK(r.success, "read port 0 succeeds");
    CHECK_EQ(0x42u, (unsigned)r.value, "value is 0x42");

    auto w = api.writeIo(0x00, 0x99);
    CHECK(w.success, "write port 0 succeeds");
    CHECK_EQ(0x99u, (unsigned)mock.getIoPort(0x00), "port 0 value is 0x99");
    TEST_END();
}

static void test_io_max_port()
{
    TEST_BEGIN("I/O port 0xFF is valid");
    MockAgentBackend mock;
    AgentApi api(mock);

    mock.setIoPort(0xFF, 0x77);
    auto r = api.readIo(0xFF);
    CHECK(r.success, "read port 0xFF succeeds");
    CHECK_EQ(0x77u, (unsigned)r.value, "value is 0x77");

    auto w = api.writeIo(0xFF, 0x88);
    CHECK(w.success, "write port 0xFF succeeds");
    CHECK_EQ(0x88u, (unsigned)mock.getIoPort(0xFF), "port 0xFF value is 0x88");
    TEST_END();
}

static void test_io_read_write_sequence()
{
    TEST_BEGIN("I/O read/write sequence");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Write and read back
    api.writeIo(0x20, 0x12);
    auto r1 = api.readIo(0x20);
    CHECK(r1.success, "first read succeeds");
    CHECK_EQ(0x12u, (unsigned)r1.value, "first value is 0x12");

    // Write new value and read back
    api.writeIo(0x20, 0x34);
    auto r2 = api.readIo(0x20);
    CHECK(r2.success, "second read succeeds");
    CHECK_EQ(0x34u, (unsigned)r2.value, "second value is 0x34");
    TEST_END();
}

static void test_io_trace_not_affected()
{
    TEST_BEGIN("I/O trace not affected by agent I/O");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Add a CPU I/O event (simulating real CPU I/O)
    IoAccessEvent cpuIo{};
    cpuIo.instructionSequence = 100;
    cpuIo.port = 0x02;
    cpuIo.value = 0x55;
    cpuIo.type = IoAccessType::Out;
    mock.addIoEvent(cpuIo);

    // Agent I/O operations should not affect the trace
    api.writeIo(0x10, 0xAA);
    api.readIo(0x10);

    auto traceResult = api.getIoTrace();
    CHECK(traceResult.success, "getIoTrace succeeds");
    CHECK_EQ(1u, (unsigned)traceResult.value.size(), "only CPU I/O in trace");
    CHECK_EQ(0x02u, (unsigned)traceResult.value[0].port, "CPU I/O port preserved");
    CHECK_EQ(0x55u, (unsigned)traceResult.value[0].value, "CPU I/O value preserved");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Breakpoints
// ---------------------------------------------------------------------------

static void test_set_clear_breakpoint()
{
    TEST_BEGIN("set and clear breakpoint");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r1 = api.setBreakpoint(0x0200);
    CHECK(r1.success, "breakpoint set");
    CHECK(mock.hasBreakpoint(0x0200), "backend confirms breakpoint");

    auto r2 = api.setBreakpoint(0x0200);
    CHECK(!r2.success, "duplicate rejected");

    auto bpsResult = api.listBreakpoints();
    CHECK(bpsResult.success, "listBreakpoints succeeds");
    CHECK_EQ(1u, (unsigned)bpsResult.value.size(), "1 breakpoint");

    auto r3 = api.clearBreakpoint(0x0200);
    CHECK(r3.success, "breakpoint cleared");
    CHECK(!mock.hasBreakpoint(0x0200), "backend confirms removed");

    auto r4 = api.clearBreakpoint(0x0200);
    CHECK(!r4.success, "clear nonexistent fails");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Trace / IO / VRAM
// ---------------------------------------------------------------------------

static void test_get_execution_trace()
{
    TEST_BEGIN("getExecutionTrace returns events");
    MockAgentBackend mock;

    // Add some instruction events
    InstructionEvent ev1{};
    ev1.pcBefore = 0x0100; ev1.pcAfter = 0x0103; ev1.opcode = 0x31;
    mock.addInstructionEvent(ev1);

    InstructionEvent ev2{};
    ev2.pcBefore = 0x0103; ev2.pcAfter = 0x0105; ev2.opcode = 0x3E;
    mock.addInstructionEvent(ev2);

    AgentApi api(mock);
    auto traceResult = api.getExecutionTrace(100);
    CHECK(traceResult.success, "getExecutionTrace succeeds");
    CHECK_EQ(2u, (unsigned)traceResult.value.size(), "2 events");
    CHECK_EQ(0x0100u, (unsigned)traceResult.value[0].pcBefore, "first PC");
    CHECK_EQ(0x0103u, (unsigned)traceResult.value[1].pcBefore, "second PC");
    TEST_END();
}

static void test_list_breakpoints()
{
    TEST_BEGIN("listBreakpoints returns all breakpoints");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.setBreakpoint(0x0200);
    api.setBreakpoint(0x0300);
    auto bpsResult2 = api.listBreakpoints();
    CHECK(bpsResult2.success, "listBreakpoints succeeds");
    CHECK_EQ(2u, (unsigned)bpsResult2.value.size(), "2 breakpoints");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Annotations
// ---------------------------------------------------------------------------

static void test_create_function()
{
    TEST_BEGIN("createFunction adds symbol");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.createFunction(0x0200);
    CHECK(r.success, "function created");

    const DebugSymbol *sym = mock.symbolDatabase().findSymbol(0x0200);
    CHECK(sym != nullptr, "symbol exists");
    CHECK_STR("sub_0200", sym->name, "auto-name");
    CHECK_EQ((unsigned)SymbolType::Function, (unsigned)sym->type, "function type");

    auto r2 = api.createFunction(0x0200);
    CHECK(!r2.success, "duplicate rejected");
    TEST_END();
}

static void test_rename_function()
{
    TEST_BEGIN("renameFunction changes name");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.createFunction(0x0200);
    auto r = api.renameFunction(0x0200, "DrawSprite");
    CHECK(r.success, "rename succeeds");

    const DebugSymbol *sym = mock.symbolDatabase().findSymbol(0x0200);
    CHECK_STR("DrawSprite", sym->name, "new name");

    auto r2 = api.renameFunction(0x9999, "Nope");
    CHECK(!r2.success, "rename nonexistent fails");
    TEST_END();
}

static void test_set_function_comment()
{
    TEST_BEGIN("setFunctionComment sets comment");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.createFunction(0x0200);
    auto r = api.setFunctionComment(0x0200, "Draws a sprite");
    CHECK(r.success, "comment set");

    const DebugSymbol *sym = mock.symbolDatabase().findSymbol(0x0200);
    CHECK_STR("Draws a sprite", sym->comment, "comment text");
    TEST_END();
}

static void test_delete_function()
{
    TEST_BEGIN("deleteFunction removes symbol");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.createFunction(0x0200);
    auto r = api.deleteFunction(0x0200);
    CHECK(r.success, "deleted");
    CHECK(mock.symbolDatabase().findSymbol(0x0200) == nullptr, "gone");

    auto r2 = api.deleteFunction(0x0200);
    CHECK(!r2.success, "delete again fails");
    TEST_END();
}

static void test_add_label()
{
    TEST_BEGIN("addLabel creates label symbol");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.addLabel(0x4000, "SCORE");
    CHECK(r.success, "label created");

    const DebugSymbol *sym = mock.symbolDatabase().findSymbol(0x4000);
    CHECK(sym != nullptr, "exists");
    CHECK_STR("SCORE", sym->name, "name");
    CHECK_EQ((unsigned)SymbolType::Label, (unsigned)sym->type, "label type");
    TEST_END();
}

static void test_apply_annotation()
{
    TEST_BEGIN("applyAnnotation with confidence");
    MockAgentBackend mock;
    AgentApi api(mock);

    Annotation ann;
    ann.type = Annotation::Function;
    ann.address = 0x0200;
    ann.name = "InitSound";
    ann.comment = "Initialize AY-3-8912";
    ann.confidence = 0.91;

    auto r = api.applyAnnotation(ann);
    CHECK(r.success, "annotation applied");

    const DebugSymbol *sym = mock.symbolDatabase().findSymbol(0x0200);
    CHECK(sym != nullptr, "symbol exists");
    CHECK_STR("InitSound", sym->name, "name");
    CHECK_STR("Initialize AY-3-8912", sym->comment, "comment");

    // Verify log has confidence
    auto entries = api.log().entries();
    CHECK(entries.size() > 0, "log has entries");
    bool foundConfidence = false;
    for (const auto &e : entries) {
        if (e.tool == "applyAnnotation" && e.arguments.find("0.91") != std::string::npos) {
            foundConfidence = true;
        }
    }
    CHECK(foundConfidence, "confidence logged");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: High-level — getFunctionContext
// ---------------------------------------------------------------------------

static void test_get_function_context()
{
    TEST_BEGIN("getFunctionContext returns disassembly");
    MockAgentBackend mock;
    AgentApi api(mock);

    // The subroutine at 0x0200 is:
    // E5        PUSH H
    // 26 C0     MVI H, 0xC0
    // 2E 00     MVI L, 0x00
    // 77        MOV M, A
    // E1        POP H
    // C9        RET

    auto ctxResult = api.getFunctionContext(0x0200);
    CHECK(ctxResult.success, "succeeds");
    FunctionContext ctx = ctxResult.value;

    CHECK_EQ(0x0200u, (unsigned)ctx.address, "address");
    CHECK(ctx.instructions.size() >= 6, "at least 6 instructions");
    CHECK(ctx.size > 0, "size > 0");

    // Check first instruction is PUSH H
    CHECK_EQ(0xE5u, (unsigned)ctx.instructions[0].bytes[0], "first opcode PUSH H");

    // Check last instruction is RET
    CHECK_EQ(0xC9u, (unsigned)ctx.instructions.back().bytes[0], "last opcode RET");

    // Name should be auto-generated
    CHECK_STR("sub_0200", ctx.name, "auto-name");
    TEST_END();
}

static void test_get_function_context_with_name()
{
    TEST_BEGIN("getFunctionContext uses existing name");
    MockAgentBackend mock;
    mock.symbolDatabase().addSymbol(0x0200, "WriteVRAM", SymbolType::Function);
    mock.symbolDatabase().setComment(0x0200, "Writes A to VRAM");

    AgentApi api(mock);
    auto ctxResult = api.getFunctionContext(0x0200);
    CHECK(ctxResult.success, "succeeds");
    FunctionContext ctx = ctxResult.value;

    CHECK_STR("WriteVRAM", ctx.name, "existing name");
    CHECK_STR("Writes A to VRAM", ctx.comment, "existing comment");
    TEST_END();
}

static void test_get_function_context_callees()
{
    TEST_BEGIN("getFunctionContext finds callees");
    MockAgentBackend mock;
    AgentApi api(mock);

    // The main program at 0x0100 has CALL 0x0200
    auto ctxResult3 = api.getFunctionContext(0x0100);
    CHECK(ctxResult3.success, "succeeds");
    FunctionContext ctx = ctxResult3.value;

    // Should find 0x0200 as a callee
    bool found = false;
    for (uint16_t addr : ctx.callees) {
        if (addr == 0x0200) found = true;
    }
    CHECK(found, "CALL 0x0200 found in callees");
    TEST_END();
}

static void test_get_function_context_vram()
{
    TEST_BEGIN("getFunctionContext finds VRAM writes");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto ctxResult = api.getFunctionContext(0x0200);
    CHECK(ctxResult.success, "succeeds");
    FunctionContext ctx = ctxResult.value;

    // Without trace, VRAM writes are unknown (no global counters)
    CHECK(ctx.vramSource == DataSource::Unknown, "VRAM source unknown without trace");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: traceFunction
// ---------------------------------------------------------------------------

static void test_trace_function()
{
    TEST_BEGIN("traceFunction executes and analyzes");
    MockAgentBackend mock;

    // Set up a simple function at 0x0200: MOV H,A; RET
    // (MOV H,A = 0x26 is not specially handled, just advances PC)
    std::vector<uint8_t> func = {0x67, 0xC9};  // MOV H,A; RET
    mock.setMemory(0x0200, func);

    // Place NOP at 0x01FF so requestRun() advances to 0x0200
    std::vector<uint8_t> nop = {0x00};
    mock.setMemory(0x01FF, nop);

    // Set CPU state: PC just before function, SP as if CALL pushed return addr
    CpuState cpu{};
    cpu.pc = 0x01FF;
    cpu.sp = 0xF7FE;
    cpu.a = 0x42;
    mock.setCpuState(cpu);

    // Place return address on stack at SP (simulating CALL having pushed it)
    std::vector<uint8_t> retAddr = {0x08, 0x01};  // 0x0108 little-endian
    mock.setMemory(0xF7FE, retAddr);

    AgentApi api(mock);
    auto trResult = api.traceFunction(0x0200);
    CHECK(trResult.success, "trace succeeds");
    TraceResult tr = trResult.value;

    CHECK_EQ(0x0200u, (unsigned)tr.entryPc, "entry PC");
    CHECK(tr.instructionCount > 0, "instructions executed");
    CHECK_EQ(0x0201u, (unsigned)tr.exitPc, "exit at RET (0201)");
    CHECK_EQ(0xF7FEu, (unsigned)tr.entrySp, "SP entry");
    CHECK_EQ(0xF800u, (unsigned)tr.exitSp, "SP exit (restored by RET)");
    CHECK(tr.exitReason == ExitReason::Ret, "exit reason Ret");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Agent log
// ---------------------------------------------------------------------------

static void test_agent_log()
{
    TEST_BEGIN("Agent log records operations");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.getCpuState();
    api.readMemory(0x0100, 4);
    api.setBreakpoint(0x0200);

    auto entries = api.log().entries();
    CHECK_EQ(3u, (unsigned)entries.size(), "3 log entries");
    CHECK_STR("getCpuState", entries[0].tool, "first tool");
    CHECK_STR("readMemory", entries[1].tool, "second tool");
    CHECK_STR("setBreakpoint", entries[2].tool, "third tool");

    api.clearLog();
    CHECK_EQ(0u, (unsigned)api.log().size(), "log cleared");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Error cases
// ---------------------------------------------------------------------------

static void test_read_memory_zero_size()
{
    TEST_BEGIN("readMemory with zero size");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto dataResult = api.readMemory(0x0100, 0);
    CHECK(dataResult.success, "readMemory(0) succeeds");
    CHECK_EQ(0u, (unsigned)dataResult.value.size(), "empty result");
    TEST_END();
}

static void test_rename_nonexistent()
{
    TEST_BEGIN("rename nonexistent function fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.renameFunction(0x9999, "Nope");
    CHECK(!r.success, "rename fails for nonexistent");
    TEST_END();
}

static void test_comment_nonexistent()
{
    TEST_BEGIN("setComment on nonexistent symbol fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setComment(0x9999, "Nope");
    CHECK(!r.success, "comment fails for nonexistent");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Stage 6.1 — ErrorCode and AgentApiResult
// ---------------------------------------------------------------------------

static void test_agent_api_result_ok()
{
    TEST_BEGIN("AgentApiResult::ok factory");
    auto r = AgentApiResult<int>::ok(42);
    CHECK(r.success, "success is true");
    CHECK_EQ(42u, (unsigned)r.value, "value is 42");
    CHECK(r.error_code == ErrorCode::None, "error_code is None");
    CHECK(r.error_message.empty(), "error_message is empty");
    TEST_END();
}

static void test_agent_api_result_fail()
{
    TEST_BEGIN("AgentApiResult::fail factory");
    auto r = AgentApiResult<int>::fail(ErrorCode::InvalidArgument, "bad arg");
    CHECK(!r.success, "success is false");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error_code is InvalidArgument");
    CHECK_STR("bad arg", r.error_message, "error_message matches");
    TEST_END();
}

static void test_agent_api_result_void()
{
    TEST_BEGIN("AgentApiResult<void> factory");
    auto ok = AgentApiResult<void>::ok();
    CHECK(ok.success, "void ok success");
    CHECK(ok.error_code == ErrorCode::None, "void ok no error");

    auto fail = AgentApiResult<void>::fail(ErrorCode::OperationFailed, "oops");
    CHECK(!fail.success, "void fail not success");
    CHECK(fail.error_code == ErrorCode::OperationFailed, "void fail error code");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Stage 6.1 — isRunning()
// ---------------------------------------------------------------------------

static void test_is_running_initially()
{
    TEST_BEGIN("isRunning initially false (paused)");
    MockAgentBackend mock;
    AgentApi api(mock);

    CHECK(!api.isRunning(), "not running when paused");
    CHECK(mock.isPaused(), "backend confirms paused");
    TEST_END();
}

static void test_is_running_after_run()
{
    TEST_BEGIN("isRunning after run/pause cycle");
    MockAgentBackend mock;

    // Fill with NOPs so run hits safety limit and pauses
    std::vector<uint8_t> nops(256, 0x00);
    mock.setMemory(0x0100, nops);

    AgentApi api(mock);
    api.run();
    // Mock runs synchronously until HLT/limit, then pauses
    CHECK(!api.isRunning(), "not running after mock run completes");
    CHECK(mock.isPaused(), "backend paused after run");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Stage 6.1 — clearAllBreakpoints()
// ---------------------------------------------------------------------------

static void test_clear_all_breakpoints()
{
    TEST_BEGIN("clearAllBreakpoints removes all");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.setBreakpoint(0x0200);
    api.setBreakpoint(0x0300);
    api.setBreakpoint(0x0400);
    CHECK_EQ(3u, (unsigned)api.listBreakpoints().value.size(), "3 breakpoints");

    auto r = api.clearAllBreakpoints();
    CHECK(r.success, "clearAll succeeds");
    CHECK(r.error_code == ErrorCode::None, "no error");
    CHECK_EQ(0u, (unsigned)api.listBreakpoints().value.size(), "0 breakpoints after clear");
    TEST_END();
}

static void test_clear_all_breakpoints_empty()
{
    TEST_BEGIN("clearAllBreakpoints on empty list");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.clearAllBreakpoints();
    CHECK(r.success, "clearAll on empty succeeds");
    CHECK_EQ(0u, (unsigned)api.listBreakpoints().value.size(), "still 0 breakpoints");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Stage 6.1 — setRegister()
// ---------------------------------------------------------------------------

static void test_set_register_a()
{
    TEST_BEGIN("setRegister A");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setRegister("A", 0xAA);
    CHECK(r.success, "setRegister A succeeds");
    CHECK_EQ(0xAAu, (unsigned)api.getCpuState().value.a, "A = 0xAA");
    TEST_END();
}

static void test_set_register_f_preserves_a()
{
    TEST_BEGIN("setRegister F preserves A");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.setRegister("A", 0x55);
    auto r = api.setRegister("F", 0x03);
    CHECK(r.success, "setRegister F succeeds");
    CHECK_EQ(0x55u, (unsigned)api.getCpuState().value.a, "A preserved");
    CHECK_EQ(0x03u, (unsigned)api.getCpuState().value.flags, "F = 0x03");
    TEST_END();
}

static void test_set_register_bc()
{
    TEST_BEGIN("setRegister B and C");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.setRegister("B", 0x12);
    api.setRegister("C", 0x34);
    CHECK_EQ(0x12u, (unsigned)api.getCpuState().value.b, "B = 0x12");
    CHECK_EQ(0x34u, (unsigned)api.getCpuState().value.c, "C = 0x34");
    TEST_END();
}

static void test_set_register_hl()
{
    TEST_BEGIN("setRegister H and L");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.setRegister("H", 0xDE);
    api.setRegister("L", 0xAD);
    CHECK_EQ(0xDEu, (unsigned)api.getCpuState().value.h, "H = 0xDE");
    CHECK_EQ(0xADu, (unsigned)api.getCpuState().value.l, "L = 0xAD");
    TEST_END();
}

static void test_set_register_pc()
{
    TEST_BEGIN("setRegister PC");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setRegister("PC", 0x8000);
    CHECK(r.success, "setRegister PC succeeds");
    CHECK_EQ(0x8000u, (unsigned)api.getCpuState().value.pc, "PC = 0x8000");
    TEST_END();
}

static void test_set_register_sp()
{
    TEST_BEGIN("setRegister SP");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setRegister("SP", 0xF000);
    CHECK(r.success, "setRegister SP succeeds");
    CHECK_EQ(0xF000u, (unsigned)api.getCpuState().value.sp, "SP = 0xF000");
    TEST_END();
}

static void test_set_register_invalid()
{
    TEST_BEGIN("setRegister invalid name");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setRegister("X", 0x42);
    CHECK(!r.success, "invalid register fails");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error is InvalidArgument");
    CHECK(!r.error_message.empty(), "error message provided");
    TEST_END();
}

static void test_set_register_empty_name()
{
    TEST_BEGIN("setRegister empty name");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setRegister("", 0x42);
    CHECK(!r.success, "empty name fails");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error is InvalidArgument");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Stage 6.1 — disassemble()
// ---------------------------------------------------------------------------

static void test_disassemble_basic()
{
    TEST_BEGIN("disassemble basic program");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Program at 0x0100: LXI SP,0xF800 (31 00 F8), MVI A,0x55 (3E 55)
    auto r = api.disassemble(0x0100, 2);
    CHECK(r.success, "disassemble succeeds");
    CHECK(r.error_code == ErrorCode::None, "no error");
    CHECK_EQ(2u, (unsigned)r.value.size(), "2 instructions");

    // First: LXI SP, 0xF800
    CHECK_EQ(0x0100u, (unsigned)r.value[0].address, "first addr 0100");
    CHECK_EQ(0x0103u, (unsigned)r.value[0].next_address, "first next_addr 0103");
    CHECK_EQ(3u, (unsigned)r.value[0].bytes.size(), "3 bytes");
    CHECK_EQ(0x31u, (unsigned)r.value[0].bytes[0], "opcode 0x31");

    // Second: MVI A, 0x55
    CHECK_EQ(0x0103u, (unsigned)r.value[1].address, "second addr 0103");
    CHECK_EQ(0x0105u, (unsigned)r.value[1].next_address, "second next_addr 0105");
    CHECK_EQ(2u, (unsigned)r.value[1].bytes.size(), "2 bytes");
    CHECK_EQ(0x3Eu, (unsigned)r.value[1].bytes[0], "opcode 0x3E");
    TEST_END();
}

static void test_disassemble_single_instruction()
{
    TEST_BEGIN("disassemble single instruction");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.disassemble(0x0100, 1);
    CHECK(r.success, "disassemble succeeds");
    CHECK_EQ(1u, (unsigned)r.value.size(), "1 instruction");
    CHECK(!r.value[0].text.empty(), "text is non-empty");
    CHECK(!r.value[0].mnemonic.empty(), "mnemonic is non-empty");
    TEST_END();
}

static void test_disassemble_count_zero()
{
    TEST_BEGIN("disassemble count=0 fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.disassemble(0x0100, 0);
    CHECK(!r.success, "count=0 fails");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error is InvalidArgument");
    TEST_END();
}

static void test_disassemble_hlt()
{
    TEST_BEGIN("disassemble HLT instruction");
    MockAgentBackend mock;
    AgentApi api(mock);

    // HLT at 0x0108
    auto r = api.disassemble(0x0108, 1);
    CHECK(r.success, "disassemble succeeds");
    CHECK_EQ(1u, (unsigned)r.value.size(), "1 instruction");
    CHECK_EQ(0x76u, (unsigned)r.value[0].bytes[0], "opcode 0x76 (HLT)");
    CHECK_EQ(1u, (unsigned)r.value[0].bytes.size(), "1 byte");
    CHECK_EQ(0x0109u, (unsigned)r.value[0].next_address, "next_addr 0109");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Stage 6.1 — getInstructionHistory()
// ---------------------------------------------------------------------------

static void test_get_instruction_history_after_steps()
{
    TEST_BEGIN("getInstructionHistory after stepping");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Step a few instructions
    api.step();  // LXI SP at 0100
    api.step();  // MVI A at 0103
    api.step();  // CALL at 0105

    auto r = api.getInstructionHistory(10);
    CHECK(r.success, "getInstructionHistory succeeds");
    CHECK(r.error_code == ErrorCode::None, "no error");
    CHECK(r.value.size() >= 3, "at least 3 entries");

    // First entry should be at 0x0100
    CHECK_EQ(0x0100u, (unsigned)r.value[0].address, "first entry at 0100");
    // Second at 0x0103
    CHECK_EQ(0x0103u, (unsigned)r.value[1].address, "second entry at 0103");
    // Third at 0x0105
    CHECK_EQ(0x0105u, (unsigned)r.value[2].address, "third entry at 0105");

    // Each entry should have bytes and disassembly
    CHECK(!r.value[0].bytes.empty(), "first entry has bytes");
    CHECK(!r.value[0].disassembly.empty(), "first entry has disassembly");
    TEST_END();
}

static void test_get_instruction_history_limit()
{
    TEST_BEGIN("getInstructionHistory respects limit");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.step();
    api.step();
    api.step();

    auto r = api.getInstructionHistory(2);
    CHECK(r.success, "succeeds");
    CHECK_EQ(2u, (unsigned)r.value.size(), "limited to 2 entries");
    // Should be the LAST 2 entries
    CHECK_EQ(0x0103u, (unsigned)r.value[0].address, "first of last 2 at 0103");
    CHECK_EQ(0x0105u, (unsigned)r.value[1].address, "second of last 2 at 0105");
    TEST_END();
}

static void test_get_instruction_history_count_zero()
{
    TEST_BEGIN("getInstructionHistory count=0 fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getInstructionHistory(0);
    CHECK(!r.success, "count=0 fails");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error is InvalidArgument");
    TEST_END();
}

static void test_get_instruction_history_empty()
{
    TEST_BEGIN("getInstructionHistory with no history");
    MockAgentBackend mock;
    // Don't step — mock has pre-populated events from constructor? No, only from simulateStep.
    AgentApi api(mock);

    auto r = api.getInstructionHistory(10);
    CHECK(r.success, "succeeds even with empty history");
    CHECK_EQ(0u, (unsigned)r.value.size(), "0 entries");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.1 Iteration 2 — getStack (§13)
// ---------------------------------------------------------------------------

static void test_get_stack_basic()
{
    TEST_BEGIN("getStack returns SP entries");
    MockAgentBackend mock;
    AgentApi api(mock);

    // SP = 0xF800, memory is zeroed
    auto r = api.getStack(4);
    CHECK(r.success, "getStack succeeds");
    CHECK_EQ(4u, (unsigned)r.value.size(), "4 entries");
    CHECK_EQ(0xF800u, (unsigned)r.value[0].address, "first entry at SP");
    CHECK_EQ(0xF802u, (unsigned)r.value[1].address, "second entry at SP+2");
    CHECK_EQ(0xF804u, (unsigned)r.value[2].address, "third entry at SP+4");
    CHECK_EQ(0xF806u, (unsigned)r.value[3].address, "fourth entry at SP+6");
    // Values are 0 (memory is zeroed)
    CHECK_EQ(0x0000u, (unsigned)r.value[0].value, "first value = 0");
    TEST_END();
}

static void test_get_stack_with_data()
{
    TEST_BEGIN("getStack reads 16-bit values from memory");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Write known values at SP
    // SP = 0xF800: lo=0x34, hi=0x12 → value = 0x1234
    mock.setMemory(0xF800, {0x34, 0x12});
    // SP+2: lo=0xAB, hi=0xCD → value = 0xCDAB
    mock.setMemory(0xF802, {0xAB, 0xCD});

    auto r = api.getStack(2);
    CHECK(r.success, "succeeds");
    CHECK_EQ(0x1234u, (unsigned)r.value[0].value, "first value = 0x1234");
    CHECK_EQ(0xCDABu, (unsigned)r.value[1].value, "second value = 0xCDAB");
    TEST_END();
}

static void test_get_stack_with_symbols()
{
    TEST_BEGIN("getStack resolves symbols");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Create a symbol at 0x0200
    mock.requestCreateFunction(0x0200, "myFunc");

    // Write 0x0200 at SP
    mock.setMemory(0xF800, {0x00, 0x02});

    auto r = api.getStack(1);
    CHECK(r.success, "succeeds");
    CHECK_EQ(0x0200u, (unsigned)r.value[0].value, "value = 0x0200");
    CHECK_STR("myFunc", r.value[0].symbol, "symbol resolved");
    TEST_END();
}

static void test_get_stack_limit_zero()
{
    TEST_BEGIN("getStack limit=0 fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getStack(0);
    CHECK(!r.success, "limit=0 fails");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "InvalidArgument");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.1 Iteration 2 — getMemoryMap (§18)
// ---------------------------------------------------------------------------

static void test_get_memory_map()
{
    TEST_BEGIN("getMemoryMap returns 256 blocks");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getMemoryMap();
    CHECK(r.success, "succeeds");
    CHECK_EQ(256u, (unsigned)r.value.size(), "256 blocks");

    // First block: 0x0000-0x00FF
    CHECK_EQ(0x0000u, (unsigned)r.value[0].start, "block 0 start");
    CHECK_EQ(0x00FFu, (unsigned)r.value[0].end, "block 0 end");

    // Last block: 0xFF00-0xFFFF
    CHECK_EQ(0xFF00u, (unsigned)r.value[255].start, "block 255 start");
    CHECK_EQ(0xFFFFu, (unsigned)r.value[255].end, "block 255 end");
    TEST_END();
}

static void test_get_memory_map_has_content()
{
    TEST_BEGIN("getMemoryMap detects non-zero content");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Mock has program at 0x0100-0x0208, so block 1 (0x0100-0x01FF) and
    // block 2 (0x0200-0x02FF) should have content
    auto r = api.getMemoryMap();
    CHECK(r.success, "succeeds");
    CHECK(r.value[1].has_content, "block 1 (0x0100-0x01FF) has content");
    CHECK(r.value[2].has_content, "block 2 (0x0200-0x02FF) has content");
    CHECK(!r.value[10].has_content, "block 10 (0x0A00-0x0AFF) is empty");
    TEST_END();
}


// ---------------------------------------------------------------------------
// Stage 6.22 §4 — execute_activity is a measurement, not a constant zero
// ---------------------------------------------------------------------------

static void test_get_memory_map_execute_activity()
{
    TEST_BEGIN("getMemoryMap sums execute_activity per block");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Regression guard: the summation runs start..end inclusive and the last
    // block ends at 0xFFFF.  A 16-bit loop counter wraps there and never
    // leaves the loop, so this call would hang instead of returning.
    auto r = api.getMemoryMap();
    CHECK(r.success, "succeeds (and returns at all)");
    CHECK_EQ(256u, (unsigned)r.value.size(), "256 blocks");

    // The mock program started 3 instructions inside block 1 and 6 inside block 2.
    CHECK_EQ(3u, (unsigned)r.value[1].execute_activity, "block 1 = 3 instructions started in it");
    CHECK_EQ(6u, (unsigned)r.value[2].execute_activity, "block 2 = 6 instructions started in it");
    CHECK_EQ(0u, (unsigned)r.value[0].execute_activity, "block 0 executed nothing");
    CHECK_EQ(0u, (unsigned)r.value[255].execute_activity, "last block executed nothing");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.1 Iteration 2 — getScreenInfo (§20)
// ---------------------------------------------------------------------------

static void test_get_screen_info()
{
    TEST_BEGIN("getScreenInfo returns video mode");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getScreenInfo();
    CHECK(r.success, "succeeds");
    CHECK_EQ(576u, (unsigned)r.value.width, "width = 576");
    CHECK_EQ(288u, (unsigned)r.value.height, "height = 288");
    CHECK_EQ(256u, (unsigned)r.value.visible_width, "visible_width = 256");
    CHECK_EQ(256u, (unsigned)r.value.visible_height, "visible_height = 256");
    CHECK(!r.value.mode512, "256-mode by default");
    CHECK_EQ(0xC000u, (unsigned)r.value.vram_base, "vram_base = 0xC000");
    CHECK_EQ(8u, (unsigned)r.value.pixels_per_byte, "pixels_per_byte = 8");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.1 Iteration 2 — getVramInfo (§19)
// ---------------------------------------------------------------------------

static void test_get_vram_info()
{
    TEST_BEGIN("getVramInfo returns 4 fixed bit-planes in 256-mode");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getVramInfo();
    CHECK(r.success, "succeeds");
    // Stage 6.20: VRAM is exposed as the 4 real bit-planes regardless of mode
    CHECK_EQ(4u, (unsigned)r.value.planes.size(), "4 planes");
    CHECK(!r.value.mode512, "256-mode");
    CHECK_EQ(0xC000u, (unsigned)r.value.vram_base, "vram_base = 0xC000");

    // Fixed plane addresses, 8 KB each
    CHECK_EQ(0u, (unsigned)r.value.planes[0].plane, "plane 0 index");
    CHECK_EQ(0xE000u, (unsigned)r.value.planes[0].address, "plane 0 addr = 0xE000");
    CHECK_EQ(8192u, (unsigned)r.value.planes[0].size, "plane 0 size = 8192");
    CHECK_EQ(0x8000u, (unsigned)r.value.planes[3].address, "plane 3 addr = 0x8000");
    TEST_END();
}

static void test_get_vram_info_512()
{
    TEST_BEGIN("getVramInfo returns 4 fixed bit-planes in 512-mode");
    MockAgentBackend mock;
    mock.setVideoMode(true);  // 512-mode
    AgentApi api(mock);

    auto r = api.getVramInfo();
    CHECK(r.success, "succeeds");
    CHECK(r.value.mode512, "512-mode");
    CHECK_EQ(4u, (unsigned)r.value.planes.size(), "4 planes in 512-mode");

    // Same fixed plane layout in both modes
    CHECK_EQ(0u, (unsigned)r.value.planes[0].plane, "plane 0 index");
    CHECK_EQ(0xE000u, (unsigned)r.value.planes[0].address, "plane 0 addr = E000");
    CHECK_EQ(8192u, (unsigned)r.value.planes[0].size, "plane 0 size = 8192");
    CHECK_EQ(1u, (unsigned)r.value.planes[1].plane, "plane 1 index");
    CHECK_EQ(0xC000u, (unsigned)r.value.planes[1].address, "plane 1 addr = C000");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.1 Iteration 2 — getSymbols / getFunction (§15)
// ---------------------------------------------------------------------------

static void test_get_symbols_empty()
{
    TEST_BEGIN("getSymbols with no symbols");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getSymbols();
    CHECK(r.success, "succeeds");
    CHECK_EQ(0u, (unsigned)r.value.size(), "0 symbols");
    TEST_END();
}

static void test_get_symbols_with_data()
{
    TEST_BEGIN("getSymbols returns created symbols");
    MockAgentBackend mock;
    AgentApi api(mock);

    mock.requestCreateFunction(0x0200, "myFunc");
    mock.requestAddLabel(0x0300, "myLabel");

    auto r = api.getSymbols();
    CHECK(r.success, "succeeds");
    CHECK_EQ(2u, (unsigned)r.value.size(), "2 symbols");

    // Sorted by address: 0x0200 first, 0x0300 second
    CHECK_EQ(0x0200u, (unsigned)r.value[0].address, "first addr");
    CHECK_STR("myFunc", r.value[0].name, "first name");
    CHECK(r.value[0].type == SymbolInfo::Type::Function, "first is Function");

    CHECK_EQ(0x0300u, (unsigned)r.value[1].address, "second addr");
    CHECK_STR("myLabel", r.value[1].name, "second name");
    CHECK(r.value[1].type == SymbolInfo::Type::Label, "second is Label");
    TEST_END();
}

static void test_get_symbols_with_limit()
{
    TEST_BEGIN("getSymbols with limit");
    MockAgentBackend mock;
    AgentApi api(mock);

    mock.requestCreateFunction(0x0200, "func1");
    mock.requestAddLabel(0x0300, "label1");
    mock.requestCreateFunction(0x0400, "func2");

    auto r = api.getSymbols(2);
    CHECK(r.success, "succeeds");
    CHECK_EQ(2u, (unsigned)r.value.size(), "limited to 2");
    TEST_END();
}

static void test_get_function_found()
{
    TEST_BEGIN("getFunction finds existing symbol");
    MockAgentBackend mock;
    AgentApi api(mock);

    mock.requestCreateFunction(0x0200, "myFunc");

    auto r = api.getFunction(0x0200);
    CHECK(r.success, "found");
    CHECK_STR("myFunc", r.value.name, "name matches");
    CHECK_EQ(0x0200u, (unsigned)r.value.address, "address matches");
    CHECK(r.value.type == SymbolInfo::Type::Function, "is Function");
    TEST_END();
}

static void test_get_function_not_found()
{
    TEST_BEGIN("getFunction fails for unknown address");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getFunction(0x9999);
    CHECK(!r.success, "not found");
    CHECK(r.error_code == ErrorCode::NotFound, "NotFound");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.1 Iteration 2 — getXrefs (§16)
// ---------------------------------------------------------------------------

static void test_get_xrefs_empty()
{
    TEST_BEGIN("getXrefs returns empty for an unreferenced address");
    MockAgentBackend mock;
    AgentApi api(mock);

    // The mock's default ROM has CALL 0x0200 at 0x0105, so 0x0200 is NOT a
    // valid "empty" target once xrefs are actually built. Query an address no
    // instruction references and that itself references nothing.
    auto r = api.getXrefs(0x3FFF);
    CHECK(r.success, "succeeds");
    CHECK_EQ(0u, (unsigned)r.value.size(), "0 xrefs for unreferenced address");
    TEST_END();
}

// Regression: xrefs must be built lazily by the query itself (no explicit
// rebuildXrefs call). This is the actual bug — the server never wired the scan
// into the real flow, so debug_get_xrefs / callers were always empty.
static void test_get_xrefs_lazy_rebuild()
{
    TEST_BEGIN("getXrefs builds xref graph lazily without explicit rebuild");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Fresh query with no manual rebuildXrefs(): the dirty cache must be
    // recomputed from the mock ROM (CALL 0x0200 @ 0x0105).
    auto r = api.getXrefs(0x0200);
    CHECK(r.success, "succeeds");
    CHECK(r.value.size() > 0, "lazy scan found the caller");
    bool found = false;
    for (const auto &x : r.value)
        if (x.from == 0x0105 && x.to == 0x0200) found = true;
    CHECK(found, "caller 0x0105 -> 0x0200 present");
    TEST_END();
}

// Regression: getFunctionContext().callers must be populated via the same lazy
// xref build (this was always empty before the fix).
static void test_get_function_context_callers()
{
    TEST_BEGIN("getFunctionContext reports callers via lazy xref build");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto ctxResult = api.getFunctionContext(0x0200);
    CHECK(ctxResult.success, "succeeds");
    const FunctionContext &ctx = ctxResult.value;
    bool found = false;
    for (uint16_t c : ctx.callers)
        if (c == 0x0105) found = true;
    CHECK(found, "callers contains 0x0105");
    TEST_END();
}

static void test_get_xrefs_with_data()
{
    TEST_BEGIN("getXrefs returns xrefs after rebuild");
    MockAgentBackend mock;
    AgentApi api(mock);

    // The mock has CALL 0x0200 at address 0x0105 (CD 00 02)
    // Rebuild xrefs to detect it
    auto readByte = [&mock](uint16_t addr) -> uint8_t {
        return mock.readMemory(addr);
    };
    mock.symbolDatabase().rebuildXrefs(readByte);

    auto r = api.getXrefs(0x0200);
    CHECK(r.success, "succeeds");
    CHECK(r.value.size() > 0, "has xrefs to 0x0200");
    CHECK_EQ(0x0105u, (unsigned)r.value[0].from, "xref from 0x0105");
    CHECK_EQ(0x0200u, (unsigned)r.value[0].to, "xref to 0x0200");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.1 Iteration 2 — getCallGraph (§17)
// ---------------------------------------------------------------------------

static void test_get_call_graph()
{
    TEST_BEGIN("getCallGraph returns edges");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Rebuild xrefs so call graph has data
    auto readByte = [&mock](uint16_t addr) -> uint8_t {
        return mock.readMemory(addr);
    };
    mock.symbolDatabase().rebuildXrefs(readByte);

    // Get all edges
    auto r = api.getCallGraph();
    CHECK(r.success, "succeeds");
    // Mock has CALL 0x0200 at 0x0105
    CHECK(r.value.size() > 0, "has at least 1 edge");

    bool found = false;
    for (const auto &e : r.value) {
        if (e.from == 0x0105 && e.to == 0x0200) {
            found = true;
            break;
        }
    }
    CHECK(found, "found CALL 0x0200 edge");
    TEST_END();
}

static void test_get_call_graph_filtered()
{
    TEST_BEGIN("getCallGraph filtered by address");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto readByte = [&mock](uint16_t addr) -> uint8_t {
        return mock.readMemory(addr);
    };
    mock.symbolDatabase().rebuildXrefs(readByte);

    // Filter for address 0x0200
    auto r = api.getCallGraph(0x0200);
    CHECK(r.success, "succeeds");
    CHECK(r.value.size() > 0, "has edges involving 0x0200");
    for (const auto &e : r.value) {
        CHECK(e.from == 0x0200 || e.to == 0x0200, "edge involves 0x0200");
    }
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.1 Iteration 2 — loadRomInfo (§4)
// ---------------------------------------------------------------------------

static void test_load_rom_info()
{
    TEST_BEGIN("loadRomInfo returns structured result");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.loadRom("test.rom");
    CHECK(r.success, "succeeds");
    CHECK_STR("test.rom", r.value.path, "path matches");
    CHECK_EQ(0x0100u, (unsigned)r.value.origin, ".rom origin = 0x0100");
    // PC is whatever the mock returns after load (mock doesn't change PC in loadRom)
    // So PC stays at 0x0100 (initial)
    CHECK_EQ(0x0100u, (unsigned)r.value.pc, "pc after load");
    TEST_END();
}

static void test_load_rom_info_r0m()
{
    TEST_BEGIN("loadRomInfo .r0m origin = 0x0000");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.loadRom("game.r0m");
    CHECK(r.success, "succeeds");
    CHECK_EQ(0x0000u, (unsigned)r.value.origin, ".r0m origin = 0x0000");
    TEST_END();
}

static void test_load_rom_info_explicit_org()
{
    TEST_BEGIN("loadRomInfo with explicit org");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.loadRom("data.bin", 0x8000);
    CHECK(r.success, "succeeds");
    CHECK_EQ(0x8000u, (unsigned)r.value.origin, "explicit org = 0x8000");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.1 Iteration 2 — getDebugState (§21)
// ---------------------------------------------------------------------------

static void test_get_debug_state()
{
    TEST_BEGIN("getDebugState returns full snapshot via AgentApiResult");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getDebugState();
    CHECK(r.success, "succeeds");
    CHECK(r.error_code == ErrorCode::None, "no error");
    CHECK(!r.value.running, "not running (paused)");
    CHECK_EQ(0x0100u, (unsigned)r.value.cpu.pc, "PC = 0x0100");
    CHECK_EQ(0xF800u, (unsigned)r.value.cpu.sp, "SP = 0xF800");
    CHECK_EQ(0x42u, (unsigned)r.value.cpu.a, "A = 0x42");
    CHECK_EQ(0u, (unsigned)r.value.breakpoints.size(), "no breakpoints");
    // Current instruction at PC=0x0100: LXI SP, 0xF800
    CHECK(!r.value.current_instruction.empty(), "has current instruction");
    TEST_END();
}

static void test_get_debug_state_with_breakpoint()
{
    TEST_BEGIN("getDebugState includes breakpoints");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.setBreakpoint(0x0200);

    auto r = api.getDebugState();
    CHECK(r.success, "succeeds");
    CHECK_EQ(1u, (unsigned)r.value.breakpoints.size(), "1 breakpoint");
    CHECK_EQ(0x0200u, (unsigned)r.value.breakpoints[0].address, "at 0x0200");
    TEST_END();
}

static void test_get_debug_state_with_function()
{
    TEST_BEGIN("getDebugState resolves current function");
    MockAgentBackend mock;
    AgentApi api(mock);

    mock.requestCreateFunction(0x0100, "main");

    auto r = api.getDebugState();
    CHECK(r.success, "succeeds");
    CHECK_STR("main", r.value.current_function, "current function = main");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.11 — ROM Database (RDB) tests
// ---------------------------------------------------------------------------

static void test_rdb_info_empty()
{
    TEST_BEGIN("getRdbInfo — empty RDB");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getRdbInfo();
    CHECK(r.success, "succeeds");
    CHECK_EQ(0, r.value.objectCount, "object count = 0");
    CHECK(!r.value.dirty, "not dirty");
    TEST_END();
}

static void test_rdb_add_and_get()
{
    TEST_BEGIN("addRdbObject + getRdbObject");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto addR = api.addRdbObject(0x0100, "init", "Function", 10);
    CHECK(addR.success, "add succeeds");

    auto r = api.getRdbInfo();
    CHECK(r.success, "info succeeds");
    CHECK_EQ(1, r.value.objectCount, "1 object");
    CHECK(r.value.dirty, "dirty after add");

    auto obj = api.getRdbObject(0x0100);
    CHECK(obj.success, "get succeeds");
    CHECK_EQ(0x0100, obj.value.address, "address");
    CHECK_STR("function", obj.value.type, "type");
    CHECK_STR("init", obj.value.name, "name");
    CHECK_EQ(10, obj.value.size, "size");
    CHECK(obj.value.hasSize, "hasSize");
    TEST_END();
}

static void test_rdb_add_duplicate()
{
    TEST_BEGIN("addRdbObject — duplicate address fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r1 = api.addRdbObject(0x0200, "foo", "Label");
    CHECK(r1.success, "first add succeeds");

    auto r2 = api.addRdbObject(0x0200, "bar", "Label");
    CHECK(!r2.success, "duplicate fails");
    TEST_END();
}

static void test_rdb_find_by_name()
{
    TEST_BEGIN("findRdbObject — by name");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0300, "myFunc", "Function");
    api.addRdbObject(0x0400, "myVar", "Variable");

    auto r = api.findRdbObject("myVar");
    CHECK(r.success, "found");
    CHECK_EQ(0x0400, r.value.address, "address matches");
    CHECK_STR("variable", r.value.type, "type matches");

    auto r2 = api.findRdbObject("nonexistent");
    CHECK(!r2.success, "not found returns failure");
    TEST_END();
}

static void test_rdb_list_objects()
{
    TEST_BEGIN("listRdbObjects");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "a", "Function");
    api.addRdbObject(0x0200, "b", "Label");
    api.addRdbObject(0x0050, "c", "Variable");

    auto r = api.listRdbObjects();
    CHECK(r.success, "succeeds");
    CHECK_EQ(3, static_cast<unsigned>(r.value.size()), "3 objects");
    // listObjects returns sorted by address
    CHECK_EQ(0x0050, r.value[0].address, "first = 0x0050");
    CHECK_EQ(0x0100, r.value[1].address, "second = 0x0100");
    CHECK_EQ(0x0200, r.value[2].address, "third = 0x0200");
    TEST_END();
}

static void test_rdb_list_objects_limit()
{
    TEST_BEGIN("listRdbObjects — with limit");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "a", "Function");
    api.addRdbObject(0x0200, "b", "Label");
    api.addRdbObject(0x0300, "c", "Variable");

    auto r = api.listRdbObjects(2);
    CHECK(r.success, "succeeds");
    CHECK_EQ(2, static_cast<unsigned>(r.value.size()), "limited to 2");
    TEST_END();
}

static void test_rdb_update_object()
{
    TEST_BEGIN("updateRdbObject");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "old", "Label");
    auto r = api.updateRdbObject(0x0100, "new", "Function", 20, true);
    CHECK(r.success, "update succeeds");

    auto obj = api.getRdbObject(0x0100);
    CHECK_STR("new", obj.value.name, "name updated");
    CHECK_STR("function", obj.value.type, "type updated");
    CHECK_EQ(20, obj.value.size, "size updated");
    TEST_END();
}

static void test_rdb_update_not_found()
{
    TEST_BEGIN("updateRdbObject — not found");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.updateRdbObject(0x9999, "x", "Label", 0, false);
    CHECK(!r.success, "fails for missing object");
    TEST_END();
}

static void test_rdb_remove_object()
{
    TEST_BEGIN("removeRdbObject");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "foo", "Label");
    auto r = api.removeRdbObject(0x0100);
    CHECK(r.success, "remove succeeds");

    auto info = api.getRdbInfo();
    CHECK_EQ(0, info.value.objectCount, "0 objects after remove");

    auto r2 = api.removeRdbObject(0x0100);
    CHECK(!r2.success, "remove again fails");
    TEST_END();
}

static void test_rdb_set_comment()
{
    TEST_BEGIN("setRdbComment");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "foo", "Label");
    auto r = api.setRdbComment(0x0100, "hello world");
    CHECK(r.success, "set comment succeeds");

    auto obj = api.getRdbObject(0x0100);
    CHECK_STR("hello world", obj.value.comment, "comment stored");

    auto r2 = api.setRdbComment(0x9999, "nope");
    CHECK(!r2.success, "comment on missing object fails");
    TEST_END();
}

// Regression for the RDB line-break contract: a comment/property set through
// the MCP surface must never retain a newline, so it is safe to splice verbatim
// into layout.asm.
static void test_rdb_comment_property_normalization()
{
    TEST_BEGIN("RDB comment/property line-break normalization");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "foo", "Label");
    auto r = api.setRdbComment(0x0100, "first\nsecond");
    CHECK(r.success, "set multiline comment succeeds");

    auto obj = api.getRdbObject(0x0100);
    CHECK_STR("first second", obj.value.comment.c_str(), "newline collapsed to space");
    CHECK(obj.value.comment.find('\n') == std::string::npos, "no newline in output");

    auto pr = api.setRdbProperty(0x0100, "note", "v1\r\nv2");
    CHECK(pr.success, "set multiline property succeeds");
    auto obj2 = api.getRdbObject(0x0100);
    CHECK(obj2.value.properties.count("note") > 0, "property present");
    CHECK_STR("v1 v2", obj2.value.properties["note"].c_str(), "property newline collapsed");
    TEST_END();
}

static void test_rdb_set_property()
{
    TEST_BEGIN("setRdbProperty");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "foo", "Label");
    auto r = api.setRdbProperty(0x0100, "source_file", "main.c");
    CHECK(r.success, "set property succeeds");

    auto obj = api.getRdbObject(0x0100);
    CHECK(obj.value.properties.count("source_file") > 0, "property exists");
    CHECK_STR("main.c", obj.value.properties["source_file"].c_str(), "property value");

    auto r2 = api.setRdbProperty(0x9999, "x", "y");
    CHECK(!r2.success, "property on missing object fails");
    TEST_END();
}

static void test_rdb_get_not_found()
{
    TEST_BEGIN("getRdbObject — not found");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getRdbObject(0xBEEF);
    CHECK(!r.success, "fails for missing address");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.13 — RDB Links
// ---------------------------------------------------------------------------

static void test_rdb_add_link()
{
    TEST_BEGIN("addRdbLink — basic");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");
    api.addRdbObject(0x0200, "tgt", "Function");

    auto r = api.addRdbLink(0x0100, 0x0200);
    CHECK(r.success, "add link succeeds");

    auto links = api.getRdbLinks(0x0100);
    CHECK(links.success, "get links succeeds");
    CHECK_EQ(1, static_cast<int>(links.value.size()), "1 link");
    CHECK_EQ(0x0200, links.value[0], "target address");
    TEST_END();
}

static void test_rdb_add_link_unresolved_target()
{
    TEST_BEGIN("addRdbLink — unresolved target");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");
    // No object at 0x0345

    auto r = api.addRdbLink(0x0100, 0x0345);
    CHECK(r.success, "link to unresolved target succeeds");

    auto links = api.getRdbLinks(0x0100);
    CHECK(links.success, "get links succeeds");
    CHECK_EQ(1, static_cast<int>(links.value.size()), "1 link");
    CHECK_EQ(0x0345, links.value[0], "unresolved target stored");
    TEST_END();
}

static void test_rdb_add_link_duplicate()
{
    TEST_BEGIN("addRdbLink — idempotent duplicate");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");

    auto r1 = api.addRdbLink(0x0100, 0x0200);
    CHECK(r1.success, "first add succeeds");

    auto r2 = api.addRdbLink(0x0100, 0x0200);
    CHECK(r2.success, "duplicate add succeeds (idempotent)");

    auto links = api.getRdbLinks(0x0100);
    CHECK_EQ(1, static_cast<int>(links.value.size()), "still 1 link");
    TEST_END();
}

static void test_rdb_add_link_multiple()
{
    TEST_BEGIN("addRdbLink — multiple links");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");

    api.addRdbLink(0x0100, 0x0300);
    api.addRdbLink(0x0100, 0x0100);
    api.addRdbLink(0x0100, 0x0200);

    auto links = api.getRdbLinks(0x0100);
    CHECK(links.success, "get links succeeds");
    CHECK_EQ(3, static_cast<int>(links.value.size()), "3 links");
    TEST_END();
}

static void test_rdb_add_link_self()
{
    TEST_BEGIN("addRdbLink — self-link");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");

    auto r = api.addRdbLink(0x0100, 0x0100);
    CHECK(r.success, "self-link succeeds");

    auto links = api.getRdbLinks(0x0100);
    CHECK_EQ(1, static_cast<int>(links.value.size()), "1 link");
    CHECK_EQ(0x0100, links.value[0], "self target");
    TEST_END();
}

static void test_rdb_add_link_cyclic()
{
    TEST_BEGIN("addRdbLink — cyclic links");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "a", "Function");
    api.addRdbObject(0x0200, "b", "Function");
    api.addRdbObject(0x0300, "c", "Function");

    api.addRdbLink(0x0100, 0x0200);  // a -> b
    api.addRdbLink(0x0200, 0x0300);  // b -> c
    api.addRdbLink(0x0300, 0x0100);  // c -> a

    auto la = api.getRdbLinks(0x0100);
    CHECK_EQ(1, static_cast<int>(la.value.size()), "a has 1 link");
    CHECK_EQ(0x0200, la.value[0], "a -> b");

    auto lb = api.getRdbLinks(0x0200);
    CHECK_EQ(0x0300, lb.value[0], "b -> c");

    auto lc = api.getRdbLinks(0x0300);
    CHECK_EQ(0x0100, lc.value[0], "c -> a");
    TEST_END();
}

static void test_rdb_add_link_no_source()
{
    TEST_BEGIN("addRdbLink — no source object fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.addRdbLink(0x9999, 0x0100);
    CHECK(!r.success, "fails when source doesn't exist");
    TEST_END();
}

static void test_rdb_remove_link()
{
    TEST_BEGIN("removeRdbLink — basic");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");
    api.addRdbLink(0x0100, 0x0200);
    api.addRdbLink(0x0100, 0x0300);

    auto r = api.removeRdbLink(0x0100, 0x0200);
    CHECK(r.success, "remove succeeds");

    auto links = api.getRdbLinks(0x0100);
    CHECK_EQ(1, static_cast<int>(links.value.size()), "1 link remains");
    CHECK_EQ(0x0300, links.value[0], "remaining link");
    TEST_END();
}

static void test_rdb_remove_link_missing()
{
    TEST_BEGIN("removeRdbLink — missing link fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");

    auto r = api.removeRdbLink(0x0100, 0x0200);
    CHECK(!r.success, "fails when link doesn't exist");
    TEST_END();
}

static void test_rdb_remove_link_no_source()
{
    TEST_BEGIN("removeRdbLink — no source object fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.removeRdbLink(0x9999, 0x0100);
    CHECK(!r.success, "fails when source doesn't exist");
    TEST_END();
}

static void test_rdb_get_links_empty()
{
    TEST_BEGIN("getRdbLinks — empty");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");

    auto links = api.getRdbLinks(0x0100);
    CHECK(links.success, "succeeds");
    CHECK_EQ(0, static_cast<int>(links.value.size()), "0 links");
    TEST_END();
}

static void test_rdb_get_links_not_found()
{
    TEST_BEGIN("getRdbLinks — source not found");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto links = api.getRdbLinks(0x9999);
    CHECK(!links.success, "fails when source doesn't exist");
    TEST_END();
}

static void test_rdb_get_links_sorted()
{
    TEST_BEGIN("getRdbLinks — stable ascending order");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");
    // Add in reverse order
    api.addRdbLink(0x0100, 0x0500);
    api.addRdbLink(0x0100, 0x0200);
    api.addRdbLink(0x0100, 0x0300);

    auto links = api.getRdbLinks(0x0100);
    CHECK(links.success, "succeeds");
    CHECK_EQ(3, static_cast<int>(links.value.size()), "3 links");
    CHECK_EQ(0x0200, links.value[0], "first = 0x0200");
    CHECK_EQ(0x0300, links.value[1], "second = 0x0300");
    CHECK_EQ(0x0500, links.value[2], "third = 0x0500");
    TEST_END();
}

static void test_rdb_link_persistence()
{
    TEST_BEGIN("RDB link persistence — save/reload");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");
    api.addRdbObject(0x0200, "tgt", "Function");
    api.addRdbLink(0x0100, 0x0200);

    // Save to temp file
    std::string tmpPath = "/tmp/test_rdb_link_persistence.rdb";
    auto &rdb = mock.rdbController();
    CHECK(rdb.saveAs(tmpPath), "save succeeds");

    // Reload
    CHECK(rdb.load(tmpPath), "reload succeeds");

    auto links = api.getRdbLinks(0x0100);
    CHECK(links.success, "get links after reload succeeds");
    CHECK_EQ(1, static_cast<int>(links.value.size()), "1 link after reload");
    CHECK_EQ(0x0200, links.value[0], "link preserved");

    // Cleanup
    std::remove(tmpPath.c_str());
    TEST_END();
}

static void test_rdb_remove_source_object_links()
{
    TEST_BEGIN("removeRdbObject — outgoing links removed with source");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");
    api.addRdbObject(0x0200, "tgt", "Function");
    api.addRdbLink(0x0100, 0x0200);

    // Remove source object
    api.removeRdbObject(0x0100);

    // Links should be gone (object doesn't exist)
    auto links = api.getRdbLinks(0x0100);
    CHECK(!links.success, "get links fails after source removed");
    TEST_END();
}

static void test_rdb_remove_target_object_links()
{
    TEST_BEGIN("removeRdbObject — incoming links preserved");
    MockAgentBackend mock;
    AgentApi api(mock);

    api.addRdbObject(0x0100, "src", "Function");
    api.addRdbObject(0x0200, "tgt", "Function");
    api.addRdbLink(0x0100, 0x0200);

    // Remove target object
    api.removeRdbObject(0x0200);

    // Source's link to target should still exist (unresolved now)
    auto links = api.getRdbLinks(0x0100);
    CHECK(links.success, "get links succeeds");
    CHECK_EQ(1, static_cast<int>(links.value.size()), "link preserved");
    CHECK_EQ(0x0200, links.value[0], "target address preserved");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.18 — Range Disassembly
// ---------------------------------------------------------------------------

static void test_disassemble_range_basic()
{
    TEST_BEGIN("disassembleRange — basic sequential disassembly");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Load a simple ROM: NOP, NOP, RET
    std::vector<uint8_t> rom = {0x00, 0x00, 0xC9};
    mock.writeMemory(0x0000, rom.data(), rom.size());

    auto r = api.disassembleRange(0x0000, 3);
    CHECK(r.success, "succeeds");
    CHECK_EQ(3, static_cast<int>(r.value.instructions.size()), "3 instructions");
    CHECK_EQ(0x0000, r.value.instructions[0].address, "first at 0000");
    CHECK_EQ(0x0001, r.value.instructions[1].address, "second at 0001");
    CHECK_EQ(0x0002, r.value.instructions[2].address, "third at 0002");
    CHECK_STR("NOP", r.value.instructions[0].mnemonic, "first is NOP");
    CHECK_STR("RET", r.value.instructions[2].mnemonic, "third is RET");
    TEST_END();
}

static void test_disassemble_range_branches()
{
    TEST_BEGIN("disassembleRange — branch targets and types");
    MockAgentBackend mock;
    AgentApi api(mock);

    // JMP 0100h, CALL 0200h, RET, RST 7
    std::vector<uint8_t> rom = {
        0xC3, 0x00, 0x01,  // JMP 0100h
        0xCD, 0x00, 0x02,  // CALL 0200h
        0xC9,              // RET
        0xFF               // RST 7 (target = 0038h)
    };
    mock.writeMemory(0x0000, rom.data(), rom.size());

    auto r = api.disassembleRange(0x0000, rom.size());
    CHECK(r.success, "succeeds");
    CHECK_EQ(4, static_cast<int>(r.value.instructions.size()), "4 instructions");

    // JMP
    CHECK_STR("JMP", r.value.instructions[0].mnemonic, "first is JMP");
    CHECK_STR("JMP", r.value.instructions[0].branch_type, "branch type JMP");
    CHECK(r.value.instructions[0].branch_target.has_value(), "has target");
    CHECK_EQ(0x0100, r.value.instructions[0].branch_target.value(), "target = 0100h");

    // CALL
    CHECK_STR("CALL", r.value.instructions[1].mnemonic, "second is CALL");
    CHECK_STR("CALL", r.value.instructions[1].branch_type, "branch type CALL");
    CHECK_EQ(0x0200, r.value.instructions[1].branch_target.value(), "target = 0200h");

    // RET
    CHECK_STR("RET", r.value.instructions[2].mnemonic, "third is RET");
    CHECK_STR("RET", r.value.instructions[2].branch_type, "branch type RET");
    CHECK(!r.value.instructions[2].branch_target.has_value(), "RET has no target");

    // RST
    CHECK_STR("RST", r.value.instructions[3].mnemonic, "fourth is RST");
    CHECK_STR("RST", r.value.instructions[3].branch_type, "branch type RST");
    CHECK_EQ(0x0038, r.value.instructions[3].branch_target.value(), "RST 7 target = 0038h");
    TEST_END();
}

static void test_disassemble_range_invalid()
{
    TEST_BEGIN("disassembleRange — invalid range");
    MockAgentBackend mock;
    AgentApi api(mock);

    std::vector<uint8_t> rom(256, 0x00);
    mock.writeMemory(0x0000, rom.data(), rom.size());

    // size = 0
    auto r1 = api.disassembleRange(0x0000, 0);
    CHECK(!r1.success, "size=0 fails");
    CHECK_EQ(static_cast<int>(ErrorCode::InvalidArgument), static_cast<int>(r1.error_code), "InvalidArgument");

    // Stage 6.20: cap raised to the full 64K space — MAX_MEMORY_READ_RANGE=65536
    // is unreachable for uint16_t size, so a large in-range sweep must succeed
    auto r2 = api.disassembleRange(0x0000, 20000);
    CHECK(r2.success, "size=20000 within 64K succeeds");
    // Mostly 1-byte NOPs minus a few multi-byte program instructions
    CHECK(r2.value.instructions.size() >= 19900 &&
          r2.value.instructions.size() <= 20000, "full linear sweep");

    // address + size > 64K
    auto r3 = api.disassembleRange(0xFF00, 0x0200);
    CHECK(!r3.success, "wrap-around fails");
    CHECK_EQ(static_cast<int>(ErrorCode::InvalidRange), static_cast<int>(r3.error_code), "InvalidRange");
    TEST_END();
}

static void test_disassemble_range_incomplete()
{
    TEST_BEGIN("disassembleRange — incomplete instruction at end");
    MockAgentBackend mock;
    AgentApi api(mock);

    // CALL instruction (3 bytes) but only 2 bytes in range
    std::vector<uint8_t> rom = {0xCD, 0x00, 0x01};
    mock.writeMemory(0x0000, rom.data(), rom.size());

    auto r = api.disassembleRange(0x0000, 2);
    CHECK(r.success, "succeeds");
    CHECK_EQ(0, static_cast<int>(r.value.instructions.size()), "no complete instructions");
    CHECK(r.value.incomplete_instruction, "incomplete flag set");
    TEST_END();
}

static void test_disassemble_range_readonly()
{
    TEST_BEGIN("disassembleRange — does not modify RDB");
    MockAgentBackend mock;
    AgentApi api(mock);

    std::vector<uint8_t> rom = {0x00, 0xC9};
    mock.writeMemory(0x0000, rom.data(), rom.size());

    // Get RDB state before
    auto rdbBefore = api.getRdbInfo();
    CHECK(rdbBefore.success, "RDB info before");

    // Disassemble
    auto r = api.disassembleRange(0x0000, 2);
    CHECK(r.success, "disassemble succeeds");

    // Get RDB state after
    auto rdbAfter = api.getRdbInfo();
    CHECK(rdbAfter.success, "RDB info after");
    CHECK_EQ(rdbBefore.value.objectCount, rdbAfter.value.objectCount, "object count unchanged");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.27 — Beam / raster / screen-snapshot Agent API
// ---------------------------------------------------------------------------

static void test_get_beam_state()
{
    TEST_BEGIN("getBeamState — maps adapter fields, fails when unavailable");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Default: unavailable -> Unsupported failure (target has no video path).
    auto rNo = api.getBeamState();
    CHECK(!rNo.success, "fails when beam state unavailable");

    BeamState s;
    s.available = true;
    s.running = false;
    s.frame = 42;
    s.rasterLine = 100;
    s.vCycleInLine = 300;
    s.vCycleInFrame = 100 * 768 + 300;
    s.frameVCycles = 239616; s.lineVCycles = 768; s.frameLines = 312;
    s.visible = true; s.visibleX = 12; s.visibleY = 60;
    s.cpuPc = 0x1234; s.cpuOpcode = 0xCD;
    s.hasPaletteIndex = true; s.paletteIndex = 5; s.paletteValue = 0xA3;
    s.borderIndex = 1;
    mock.setBeamState(s);

    auto r = api.getBeamState();
    CHECK(r.success, "succeeds when available");
    CHECK_EQ(42u, (unsigned)r.value.frame, "frame");
    CHECK_EQ(100u, r.value.rasterLine, "raster_line");
    CHECK_EQ(300u, r.value.vCycleInLine, "v_cycle_in_line");
    CHECK_EQ(768u, r.value.lineVCycles, "line_v_cycles");
    CHECK(r.value.visible, "visible");
    CHECK_EQ(0x1234u, r.value.cpuPc, "cpu_pc");
    CHECK(r.value.hasPaletteIndex, "has_palette_index");
    CHECK_EQ(5u, (unsigned)r.value.paletteIndex, "palette_index");
    CHECK_EQ(0xA3u, r.value.paletteValue, "palette_value");
    CHECK_EQ(1u, (unsigned)r.value.borderIndex, "border_index");
    TEST_END();
}

static void test_get_screen_snapshot_png()
{
    TEST_BEGIN("getScreenSnapshot — encodes real TV buffer to PNG (base64)");
    MockAgentBackend mock;
    AgentApi api(mock);

    BeamState bs; bs.available = true; bs.frame = 7; bs.running = false;
    mock.setBeamState(bs);

    // Tiny 2x1 ARGB8888 buffer: red, green.
    std::vector<uint32_t> px = { 0xFFFF0000u, 0xFF00FF00u };
    mock.setScreenSnapshot(2, 1, px);

    auto r = api.getScreenSnapshot();
    CHECK(r.success, "snapshot succeeds");
    CHECK_EQ(2, r.value.width, "width preserved");
    CHECK_EQ(1, r.value.height, "height preserved");
    CHECK_EQ(7u, (unsigned)r.value.frame, "frame from beam state");
    CHECK_STR("tv", r.value.source, "source is real TV buffer");
    CHECK_STR("RGB", r.value.format, "format");
    CHECK(r.value.completeFrame, "complete frame when paused");
    // PNG base64 must start with the signature bytes 89 50 4E 47 0D 0A 1A 0A.
    CHECK(r.value.pngBase64.size() > 8, "png base64 non-empty");
    CHECK(r.value.pngBase64.compare(0, 8, "iVBORw0K") == 0,
          "png base64 has PNG signature prefix");
    TEST_END();
}

static void test_screen_snapshot_empty_fails()
{
    TEST_BEGIN("getScreenSnapshot — fails on empty buffer");
    MockAgentBackend mock;
    AgentApi api(mock);
    BeamState bs; bs.available = true; mock.setBeamState(bs);
    // screenSnap_ defaults to width/height 0 -> empty.
    auto r = api.getScreenSnapshot();
    CHECK(!r.success, "fails when screen buffer empty");
    TEST_END();
}

static void test_get_raster_events()
{
    TEST_BEGIN("getRasterEvents — filters by frame/port and caps results");
    MockAgentBackend mock;
    AgentApi api(mock);

    std::vector<RasterEvent> evs;
    for (int i = 0; i < 5; ++i) {
        RasterEvent e;
        e.frame = (i < 3) ? 1 : 2;
        e.rasterLine = 10 + i;
        e.vCycleInLine = 200 + i;
        e.vCycle = e.rasterLine * 768 + e.vCycleInLine;
        e.pc = 0x2000;
        e.port = (i % 2 == 0) ? 0x0C : 0x0D;
        e.value = static_cast<uint8_t>(0x10 + i);
        evs.push_back(e);
    }
    mock.setRasterEvents(evs);

    auto all = api.getRasterEvents();
    CHECK(all.success, "query succeeds");
    CHECK_EQ(5u, (unsigned)all.value.size(), "all events");

    auto frame2 = api.getRasterEvents(/*frame*/2);
    CHECK_EQ(2u, (unsigned)frame2.value.size(), "frame filter");

    auto portC = api.getRasterEvents(0, 0, 0xFFFFFFFFu, /*port*/0x0C);
    CHECK_EQ(3u, (unsigned)portC.value.size(), "port filter (0x0C)");

    auto capped = api.getRasterEvents(0, 0, 0xFFFFFFFFu, -1, 0xFFFF, /*max*/2);
    CHECK(capped.success, "cap query ok");
    CHECK_EQ(2u, (unsigned)capped.value.size(), "max_results caps to tail");
    // tail = last two events (frame 2), values 0x13,0x14
    CHECK_EQ(0x14u, capped.value.back().value, "tail is most recent");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  Agent API Tests — Stage 5.3 / 6.1\033[0m\n");
    printf("\033[1;33m========================================\033[0m\n");

    // Execution control
    test_run_pause();
    test_step();
    test_reset();

    // CPU state
    test_get_cpu_state();

    // Memory access
    test_read_memory();
    test_write_memory();

    // I/O ports (Stage 6.1 Iteration 3)
    test_read_io();
    test_write_io();
    test_io_zero_port();
    test_io_max_port();
    test_io_read_write_sequence();
    test_io_trace_not_affected();

    // Breakpoints
    test_set_clear_breakpoint();

    // Trace / IO / VRAM
    test_get_execution_trace();
    test_list_breakpoints();

    // Annotations
    test_create_function();
    test_rename_function();
    test_set_function_comment();
    test_delete_function();
    test_add_label();
    test_apply_annotation();

    // High-level
    test_get_function_context();
    test_get_function_context_with_name();
    test_get_function_context_callees();
    test_get_function_context_vram();
    test_trace_function();

    // Log
    test_agent_log();

    // Error cases
    test_read_memory_zero_size();
    test_rename_nonexistent();
    test_comment_nonexistent();

    // Stage 6.1 — ErrorCode / AgentApiResult
    test_agent_api_result_ok();
    test_agent_api_result_fail();
    test_agent_api_result_void();

    // Stage 6.1 — isRunning
    test_is_running_initially();
    test_is_running_after_run();

    // Stage 6.1 — clearAllBreakpoints
    test_clear_all_breakpoints();
    test_clear_all_breakpoints_empty();

    // Stage 6.1 — setRegister
    test_set_register_a();
    test_set_register_f_preserves_a();
    test_set_register_bc();
    test_set_register_hl();
    test_set_register_pc();
    test_set_register_sp();
    test_set_register_invalid();
    test_set_register_empty_name();

    // Stage 6.1 — disassemble
    test_disassemble_basic();
    test_disassemble_single_instruction();
    test_disassemble_count_zero();
    test_disassemble_hlt();

    // Stage 6.1 — getInstructionHistory
    test_get_instruction_history_after_steps();
    test_get_instruction_history_limit();
    test_get_instruction_history_count_zero();
    test_get_instruction_history_empty();

    // Stage 6.1 Iteration 2 — getStack (§13)
    test_get_stack_basic();
    test_get_stack_with_data();
    test_get_stack_with_symbols();
    test_get_stack_limit_zero();

    // Stage 6.1 Iteration 2 — getMemoryMap (§18)
    test_get_memory_map();
    test_get_memory_map_has_content();
    test_get_memory_map_execute_activity();

    // Stage 6.1 Iteration 2 — getScreenInfo (§20)
    test_get_screen_info();

    // Stage 6.1 Iteration 2 — getVramInfo (§19)
    test_get_vram_info();
    test_get_vram_info_512();

    // Stage 6.1 Iteration 2 — getSymbols / getFunction (§15)
    test_get_symbols_empty();
    test_get_symbols_with_data();
    test_get_symbols_with_limit();
    test_get_function_found();
    test_get_function_not_found();

    // Stage 6.1 Iteration 2 — getXrefs (§16)
    test_get_xrefs_empty();
    test_get_xrefs_lazy_rebuild();
    test_get_function_context_callers();
    test_get_xrefs_with_data();

    // Stage 6.1 Iteration 2 — getCallGraph (§17)
    test_get_call_graph();
    test_get_call_graph_filtered();

    // Stage 6.1 Iteration 2 — loadRomInfo (§4)
    test_load_rom_info();
    test_load_rom_info_r0m();
    test_load_rom_info_explicit_org();

    // Stage 6.1 Iteration 2 — getDebugState (§21)
    test_get_debug_state();
    test_get_debug_state_with_breakpoint();
    test_get_debug_state_with_function();

    // Stage 6.11 — ROM Database (RDB)
    test_rdb_info_empty();
    test_rdb_add_and_get();
    test_rdb_add_duplicate();
    test_rdb_find_by_name();
    test_rdb_list_objects();
    test_rdb_list_objects_limit();
    test_rdb_update_object();
    test_rdb_update_not_found();
    test_rdb_remove_object();
    test_rdb_set_comment();
    test_rdb_comment_property_normalization();
    test_rdb_set_property();
    test_rdb_get_not_found();

    // Stage 6.13 — RDB Links
    test_rdb_add_link();
    test_rdb_add_link_unresolved_target();
    test_rdb_add_link_duplicate();
    test_rdb_add_link_multiple();
    test_rdb_add_link_self();
    test_rdb_add_link_cyclic();
    test_rdb_add_link_no_source();
    test_rdb_remove_link();
    test_rdb_remove_link_missing();
    test_rdb_remove_link_no_source();
    test_rdb_get_links_empty();
    test_rdb_get_links_not_found();
    test_rdb_get_links_sorted();
    test_rdb_link_persistence();
    test_rdb_remove_source_object_links();
    test_rdb_remove_target_object_links();

    // Stage 6.18 — Range Disassembly
    test_disassemble_range_basic();
    test_disassemble_range_branches();
    test_disassemble_range_invalid();
    test_disassemble_range_incomplete();
    test_disassemble_range_readonly();

    // Stage 6.27 — Beam / raster / screen-snapshot
    test_get_beam_state();
    test_get_screen_snapshot_png();
    test_screen_snapshot_empty_fails();
    test_get_raster_events();

    printf("\n\033[1;33m========================================\033[0m\n");
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf("  (\033[1;31m%d FAILED\033[0m)", tests_failed);
    }
    printf("\n\033[1;33m========================================\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
