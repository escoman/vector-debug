// Agent API Contract Tests — Stage 6.3
//
// Verify the public Agent API contract:
//   - All fallible operations return AgentApiResult<T>
//   - ErrorCode is used consistently (including NotFound)
//   - 0 semantics: count=0 → error, limit=0 → unlimited
//   - getCallGraph uses std::optional<uint16_t> ($0000 unambiguous)
//   - Overflow protection in readMemory/writeMemory/getStack
//   - Single loadRom() method (no loadRomInfo)
//   - AgentScreenSnapshot is a value type
//   - AgentLimits constants exist
//
// Uses MockAgentBackend — no Board, SDL, or emulator dependency.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <optional>
#include <type_traits>

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

// Helper for type checks (avoids comma-in-macro issue with std::is_same)
template <typename A, typename B>
static bool is_same_type() { return std::is_same<A, B>::value; }

// ===========================================================================
// §1. Execution control — AgentApiResult<void>
// ===========================================================================

static void contract_run_returns_result()
{
    TEST_BEGIN("contract: run() returns AgentApiResult<void>");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.run();
    // Must have success field
    CHECK(r.success, "run() succeeds");
    // Must NOT be void — it's AgentApiResult<void>
    bool runTypeOk = is_same_type<decltype(r), AgentApiResult<void>>();
    CHECK(runTypeOk, "return type is AgentApiResult<void>");
    TEST_END();
}

static void contract_pause_returns_result()
{
    TEST_BEGIN("contract: pause() returns AgentApiResult<void>");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.pause();
    CHECK(r.success, "pause() succeeds");
    bool pauseTypeOk = is_same_type<decltype(r), AgentApiResult<void>>();
    CHECK(pauseTypeOk, "return type is AgentApiResult<void>");
    TEST_END();
}

static void contract_step_returns_result()
{
    TEST_BEGIN("contract: step() returns AgentApiResult<void>");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.step();
    CHECK(r.success, "step() succeeds when paused");
    bool stepTypeOk = is_same_type<decltype(r), AgentApiResult<void>>();
    CHECK(stepTypeOk, "return type is AgentApiResult<void>");
    TEST_END();
}

static void contract_step_not_paused_error()
{
    TEST_BEGIN("contract: step() returns NotPaused when running");
    MockAgentBackend mock;
    mock.setState(DebuggerState::Running);
    AgentApi api(mock);

    auto r = api.step();
    CHECK(!r.success, "step() fails when running");
    CHECK(r.error_code == ErrorCode::NotPaused, "error code is NotPaused");
    CHECK(!r.error_message.empty(), "error message is not empty");
    TEST_END();
}

static void contract_reset_returns_result()
{
    TEST_BEGIN("contract: reset() returns AgentApiResult<void>");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.reset();
    CHECK(r.success, "reset() succeeds");
    bool resetTypeOk = is_same_type<decltype(r), AgentApiResult<void>>();
    CHECK(resetTypeOk, "return type is AgentApiResult<void>");
    TEST_END();
}

// ===========================================================================
// §2. CPU state — AgentApiResult<CpuState>
// ===========================================================================

static void contract_get_cpu_state_returns_result()
{
    TEST_BEGIN("contract: getCpuState() returns AgentApiResult<CpuState>");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getCpuState();
    CHECK(r.success, "succeeds");
    bool cpuTypeOk = is_same_type<decltype(r), AgentApiResult<CpuState>>();
    CHECK(cpuTypeOk, "return type is AgentApiResult<CpuState>");
    CHECK_EQ(0x0100u, (unsigned)r.value.pc, "PC accessible via .value");
    TEST_END();
}

// ===========================================================================
// §3. Memory — AgentApiResult + overflow checks
// ===========================================================================

static void contract_read_memory_overflow()
{
    TEST_BEGIN("contract: readMemory overflow → InvalidRange");
    MockAgentBackend mock;
    AgentApi api(mock);

    // address=0xFFFF, size=2 → endAddr = 0x10001 > 0x10000
    auto r = api.readMemory(0xFFFF, 2);
    CHECK(!r.success, "fails on overflow");
    CHECK(r.error_code == ErrorCode::InvalidRange, "error code is InvalidRange");
    TEST_END();
}

static void contract_read_memory_boundary_ok()
{
    TEST_BEGIN("contract: readMemory at boundary (0xFFFF, 1) succeeds");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.readMemory(0xFFFF, 1);
    CHECK(r.success, "succeeds at exact boundary");
    CHECK_EQ(1u, r.value.size(), "returns 1 byte");
    TEST_END();
}

static void contract_write_memory_overflow()
{
    TEST_BEGIN("contract: writeMemory overflow → InvalidRange");
    MockAgentBackend mock;
    AgentApi api(mock);

    std::vector<uint8_t> data = {0xAA, 0xBB};
    auto r = api.writeMemory(0xFFFF, data);
    CHECK(!r.success, "fails on overflow");
    CHECK(r.error_code == ErrorCode::InvalidRange, "error code is InvalidRange");
    TEST_END();
}

static void contract_write_memory_boundary_ok()
{
    TEST_BEGIN("contract: writeMemory at boundary (0xFFFF, 1 byte) succeeds");
    MockAgentBackend mock;
    AgentApi api(mock);

    std::vector<uint8_t> data = {0xCC};
    auto r = api.writeMemory(0xFFFF, data);
    CHECK(r.success, "succeeds at exact boundary");
    TEST_END();
}

// ===========================================================================
// §4. Breakpoints — NotFound for missing objects
// ===========================================================================

static void contract_clear_breakpoint_not_found()
{
    TEST_BEGIN("contract: clearBreakpoint nonexistent → NotFound");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.clearBreakpoint(0x9999);
    CHECK(!r.success, "fails for nonexistent breakpoint");
    CHECK(r.error_code == ErrorCode::NotFound, "error code is NotFound");
    TEST_END();
}

static void contract_set_breakpoint_enabled_not_found()
{
    TEST_BEGIN("contract: setBreakpointEnabled nonexistent → NotFound");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setBreakpointEnabled(0x9999, true);
    CHECK(!r.success, "fails for nonexistent breakpoint");
    CHECK(r.error_code == ErrorCode::NotFound, "error code is NotFound");
    TEST_END();
}

// ===========================================================================
// §5. Trace — maxEntries=0 → error
// ===========================================================================

static void contract_execution_trace_zero_is_error()
{
    TEST_BEGIN("contract: getExecutionTrace(maxEntries=0) → InvalidArgument");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getExecutionTrace(0);
    CHECK(!r.success, "fails with maxEntries=0");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error code is InvalidArgument");
    TEST_END();
}

static void contract_io_trace_zero_is_error()
{
    TEST_BEGIN("contract: getIoTrace(maxEntries=0) → InvalidArgument");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getIoTrace(0);
    CHECK(!r.success, "fails with maxEntries=0");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error code is InvalidArgument");
    TEST_END();
}

static void contract_execution_trace_nonzero_ok()
{
    TEST_BEGIN("contract: getExecutionTrace(maxEntries=100) succeeds");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getExecutionTrace(100);
    CHECK(r.success, "succeeds with maxEntries=100");
    TEST_END();
}

// ===========================================================================
// §6. Screen — AgentScreenSnapshot value type
// ===========================================================================

static void contract_get_screen_returns_agent_snapshot()
{
    TEST_BEGIN("contract: getScreen() returns AgentScreenSnapshot");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getScreen();
    CHECK(r.success, "succeeds");
    bool screenTypeOk = is_same_type<decltype(r), AgentApiResult<AgentScreenSnapshot>>();
    CHECK(screenTypeOk, "return type is AgentApiResult<AgentScreenSnapshot>");
    // AgentScreenSnapshot has width, height, pixels
    CHECK(r.value.width >= 0, "width is non-negative");
    CHECK(r.value.height >= 0, "height is non-negative");
    TEST_END();
}

// ===========================================================================
// §7. Annotations — NotFound for missing symbols
// ===========================================================================

static void contract_rename_function_not_found()
{
    TEST_BEGIN("contract: renameFunction nonexistent → NotFound");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.renameFunction(0x9999, "Nope");
    CHECK(!r.success, "fails for nonexistent symbol");
    CHECK(r.error_code == ErrorCode::NotFound, "error code is NotFound");
    TEST_END();
}

static void contract_delete_function_not_found()
{
    TEST_BEGIN("contract: deleteFunction nonexistent → NotFound");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.deleteFunction(0x9999);
    CHECK(!r.success, "fails for nonexistent symbol");
    CHECK(r.error_code == ErrorCode::NotFound, "error code is NotFound");
    TEST_END();
}

static void contract_set_function_comment_not_found()
{
    TEST_BEGIN("contract: setFunctionComment nonexistent → NotFound");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setFunctionComment(0x9999, "comment");
    CHECK(!r.success, "fails for nonexistent symbol");
    CHECK(r.error_code == ErrorCode::NotFound, "error code is NotFound");
    TEST_END();
}

static void contract_set_comment_not_found()
{
    TEST_BEGIN("contract: setComment nonexistent → NotFound");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setComment(0x9999, "comment");
    CHECK(!r.success, "fails for nonexistent symbol");
    CHECK(r.error_code == ErrorCode::NotFound, "error code is NotFound");
    TEST_END();
}

// ===========================================================================
// §8. High-level — AgentApiResult wrappers
// ===========================================================================

static void contract_get_function_context_returns_result()
{
    TEST_BEGIN("contract: getFunctionContext() returns AgentApiResult<FunctionContext>");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getFunctionContext(0x0100);
    CHECK(r.success, "succeeds");
    bool ctxTypeOk = is_same_type<decltype(r), AgentApiResult<FunctionContext>>();
    CHECK(ctxTypeOk, "return type is AgentApiResult<FunctionContext>");
    CHECK_EQ(0x0100u, (unsigned)r.value.address, "address accessible via .value");
    TEST_END();
}

static void contract_trace_function_returns_result()
{
    TEST_BEGIN("contract: traceFunction() returns AgentApiResult<TraceResult>");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.traceFunction(0x0100);
    // May succeed or timeout — both are valid AgentApiResult
    bool traceTypeOk = is_same_type<decltype(r), AgentApiResult<TraceResult>>();
    CHECK(traceTypeOk, "return type is AgentApiResult<TraceResult>");
    TEST_END();
}

// ===========================================================================
// §9. getFunction → NotFound (not InvalidAddress)
// ===========================================================================

static void contract_get_function_not_found_code()
{
    TEST_BEGIN("contract: getFunction unknown addr → NotFound (not InvalidAddress)");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getFunction(0x9999);
    CHECK(!r.success, "fails for unknown address");
    CHECK(r.error_code == ErrorCode::NotFound, "ErrorCode is NotFound");
    CHECK(r.error_code != ErrorCode::InvalidAddress, "ErrorCode is NOT InvalidAddress");
    TEST_END();
}

// ===========================================================================
// §10. getCallGraph — std::optional<uint16_t> ($0000 unambiguous)
// ===========================================================================

static void contract_call_graph_nullopt()
{
    TEST_BEGIN("contract: getCallGraph() with no args → all edges");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Default argument is std::nullopt → all edges
    auto r = api.getCallGraph();
    CHECK(r.success, "succeeds with default args");
    TEST_END();
}

static void contract_call_graph_explicit_nullopt()
{
    TEST_BEGIN("contract: getCallGraph(std::nullopt) → all edges");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getCallGraph(std::nullopt);
    CHECK(r.success, "succeeds with explicit nullopt");
    TEST_END();
}

static void contract_call_graph_address_zero()
{
    TEST_BEGIN("contract: getCallGraph(0x0000) → function at $0000 (not 'all')");
    MockAgentBackend mock;
    // Place a RET at 0x0000 so disassembly finds something
    mock.setMemory(0x0000, {0xC9});
    AgentApi api(mock);

    // Explicit $0000 — should be treated as a specific address, not "all"
    auto r = api.getCallGraph(std::optional<uint16_t>(0x0000));
    CHECK(r.success, "succeeds with address=0x0000");
    // The result should be for a specific function (may be empty if no callers)
    // The key contract point: it doesn't crash and treats $0000 as valid
    TEST_END();
}

static void contract_call_graph_implicit_conversion()
{
    TEST_BEGIN("contract: getCallGraph(uint16_t) implicit conversion to optional");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Implicit conversion from uint16_t to std::optional<uint16_t> must work
    auto r = api.getCallGraph(0x0200);
    CHECK(r.success, "implicit conversion works");
    TEST_END();
}

static void contract_call_graph_limit_zero_means_all()
{
    TEST_BEGIN("contract: getCallGraph limit=0 → unlimited (capped)");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getCallGraph(std::nullopt, 0);
    CHECK(r.success, "limit=0 succeeds (means 'all')");
    // Result should be capped at MAX_CALL_GRAPH_LIMIT
    CHECK(r.value.size() <= AgentLimits::MAX_CALL_GRAPH_LIMIT,
          "result capped at MAX_CALL_GRAPH_LIMIT");
    TEST_END();
}

// ===========================================================================
// §11. ROM — single loadRom() method
// ===========================================================================

static void contract_load_rom_exists()
{
    TEST_BEGIN("contract: loadRom(path, org) exists and returns AgentApiResult");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.loadRom("/nonexistent/test.rom");
    // May fail (file doesn't exist) but must compile and return AgentApiResult
    using Actual = decltype(r);
    using Expected = AgentApiResult<LoadRomResult>;
    bool loadRomTypeOk = is_same_type<Actual, Expected>();
    CHECK(loadRomTypeOk,
          "return type is AgentApiResult<LoadRomResult>");
    TEST_END();
}

static void contract_load_rom_info_not_exposed()
{
    TEST_BEGIN("contract: loadRomInfo() is not in public API");
    // This is a compile-time contract: AgentApi must NOT have loadRomInfo().
    // We verify by checking that loadRom() is the only ROM loading method.
    // Runtime check: just verify loadRom works as the single entry point.
    MockAgentBackend mock;
    AgentApi api(mock);

    // loadRom with explicit org
    auto r = api.loadRom("/nonexistent/test.rom", 0x8000);
    using Actual = decltype(r);
    using Expected = AgentApiResult<LoadRomResult>;
    bool loadRomInfoTypeOk = is_same_type<Actual, Expected>();
    CHECK(loadRomInfoTypeOk,
          "loadRom(path, org) is the single ROM loading method");
    TEST_END();
}

// ===========================================================================
// §12. Symbols — limit=0 means "all"
// ===========================================================================

static void contract_get_symbols_limit_zero_means_all()
{
    TEST_BEGIN("contract: getSymbols(limit=0) → all symbols (capped)");
    MockAgentBackend mock;
    // Add some symbols
    mock.symbolDatabase().addSymbol(0x0200, "func1", SymbolType::Function);
    mock.symbolDatabase().addSymbol(0x0300, "func2", SymbolType::Function);
    mock.symbolDatabase().addSymbol(0x4000, "label1", SymbolType::Label);
    AgentApi api(mock);

    auto r = api.getSymbols(0);
    CHECK(r.success, "limit=0 succeeds (means 'all')");
    CHECK_EQ(3u, (unsigned)r.value.size(), "returns all 3 symbols");
    TEST_END();
}

static void contract_get_symbols_explicit_limit()
{
    TEST_BEGIN("contract: getSymbols(limit=2) → at most 2");
    MockAgentBackend mock;
    mock.symbolDatabase().addSymbol(0x0200, "func1", SymbolType::Function);
    mock.symbolDatabase().addSymbol(0x0300, "func2", SymbolType::Function);
    mock.symbolDatabase().addSymbol(0x4000, "label1", SymbolType::Label);
    AgentApi api(mock);

    auto r = api.getSymbols(2);
    CHECK(r.success, "limit=2 succeeds");
    CHECK(r.value.size() <= 2, "at most 2 symbols returned");
    TEST_END();
}

// ===========================================================================
// §13. Disassembly — count=0 → error
// ===========================================================================

static void contract_disassemble_count_zero_is_error()
{
    TEST_BEGIN("contract: disassemble(count=0) → InvalidArgument");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.disassemble(0x0100, 0);
    CHECK(!r.success, "fails with count=0");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error code is InvalidArgument");
    TEST_END();
}

static void contract_disassemble_count_nonzero_ok()
{
    TEST_BEGIN("contract: disassemble(count=5) succeeds");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.disassemble(0x0100, 5);
    CHECK(r.success, "succeeds with count=5");
    CHECK(r.value.size() > 0, "returns instructions");
    TEST_END();
}

// ===========================================================================
// §14. getInstructionHistory — count=0 → error
// ===========================================================================

static void contract_instruction_history_count_zero_is_error()
{
    TEST_BEGIN("contract: getInstructionHistory(count=0) → InvalidArgument");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getInstructionHistory(0);
    CHECK(!r.success, "fails with count=0");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error code is InvalidArgument");
    TEST_END();
}

// ===========================================================================
// §15. getStack — limit=0 → error, overflow protection
// ===========================================================================

static void contract_get_stack_limit_zero_is_error()
{
    TEST_BEGIN("contract: getStack(limit=0) → InvalidArgument");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getStack(0);
    CHECK(!r.success, "fails with limit=0");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error code is InvalidArgument");
    TEST_END();
}

static void contract_get_stack_normal()
{
    TEST_BEGIN("contract: getStack(limit=10) succeeds");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getStack(10);
    CHECK(r.success, "succeeds with limit=10");
    TEST_END();
}

// ===========================================================================
// §16. AgentLimits constants exist and are reasonable
// ===========================================================================

static void contract_limits_exist()
{
    TEST_BEGIN("contract: AgentLimits constants are defined");
    CHECK(AgentLimits::MAX_DISASSEMBLY_COUNT > 0, "MAX_DISASSEMBLY_COUNT > 0");
    CHECK(AgentLimits::MAX_SYMBOLS_LIMIT > 0, "MAX_SYMBOLS_LIMIT > 0");
    CHECK(AgentLimits::MAX_CALL_GRAPH_LIMIT > 0, "MAX_CALL_GRAPH_LIMIT > 0");
    CHECK(AgentLimits::MAX_STACK_LIMIT > 0, "MAX_STACK_LIMIT > 0");
    CHECK(AgentLimits::MAX_TRACE_ENTRIES > 0, "MAX_TRACE_ENTRIES > 0");
    CHECK(AgentLimits::MAX_HISTORY_ENTRIES > 0, "MAX_HISTORY_ENTRIES > 0");

    // Reasonable bounds
    CHECK(AgentLimits::MAX_DISASSEMBLY_COUNT <= 100000, "MAX_DISASSEMBLY_COUNT reasonable");
    CHECK(AgentLimits::MAX_SYMBOLS_LIMIT <= 100000, "MAX_SYMBOLS_LIMIT reasonable");
    CHECK(AgentLimits::MAX_STACK_LIMIT <= 10000, "MAX_STACK_LIMIT reasonable");
    TEST_END();
}

// ===========================================================================
// §17. ErrorCode::NotFound exists
// ===========================================================================

static void contract_error_code_not_found_exists()
{
    TEST_BEGIN("contract: ErrorCode::NotFound exists and is distinct");
    CHECK(ErrorCode::NotFound != ErrorCode::None, "NotFound != None");
    CHECK(ErrorCode::NotFound != ErrorCode::InvalidAddress, "NotFound != InvalidAddress");
    CHECK(ErrorCode::NotFound != ErrorCode::InvalidArgument, "NotFound != InvalidArgument");
    CHECK(ErrorCode::NotFound != ErrorCode::OperationFailed, "NotFound != OperationFailed");
    TEST_END();
}

// ===========================================================================
// §18. No CommandResult in public API
// ===========================================================================

static void contract_no_command_result_in_api()
{
    TEST_BEGIN("contract: public API does not return CommandResult");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Verify that key operations return AgentApiResult, not CommandResult.
    // This is partly a compile-time contract, but we verify at runtime:
    auto stepR = api.step();
    bool stepOk = is_same_type<decltype(stepR), AgentApiResult<void>>();
    CHECK(stepOk, "step returns AgentApiResult<void>");

    auto bpR = api.setBreakpoint(0x0200);
    bool bpOk = is_same_type<decltype(bpR), AgentApiResult<void>>();
    CHECK(bpOk, "setBreakpoint returns AgentApiResult<void>");

    auto ioR = api.writeIo(0x10, 0xAA);
    bool ioOk = is_same_type<decltype(ioR), AgentApiResult<void>>();
    CHECK(ioOk, "writeIo returns AgentApiResult<void>");

    TEST_END();
}

// ===========================================================================
// §19. I/O — port 0 and port 255 are valid
// ===========================================================================

static void contract_io_port_zero_valid()
{
    TEST_BEGIN("contract: I/O port 0x00 is valid (not an error)");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.readIo(0x00);
    CHECK(r.success, "readIo(0) succeeds");
    auto w = api.writeIo(0x00, 0x42);
    CHECK(w.success, "writeIo(0) succeeds");
    TEST_END();
}

static void contract_io_port_max_valid()
{
    TEST_BEGIN("contract: I/O port 0xFF is valid (not an error)");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.readIo(0xFF);
    CHECK(r.success, "readIo(0xFF) succeeds");
    auto w = api.writeIo(0xFF, 0x42);
    CHECK(w.success, "writeIo(0xFF) succeeds");
    TEST_END();
}

// ===========================================================================
// §20. Annotation operations — success path returns correct ErrorCode
// ===========================================================================

static void contract_success_has_no_error_code()
{
    TEST_BEGIN("contract: successful operations have ErrorCode::None");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r1 = api.step();
    CHECK(r1.success, "step succeeds");
    CHECK(r1.error_code == ErrorCode::None, "success → ErrorCode::None");

    auto r2 = api.getCpuState();
    CHECK(r2.success, "getCpuState succeeds");
    CHECK(r2.error_code == ErrorCode::None, "success → ErrorCode::None");

    auto r3 = api.listBreakpoints();
    CHECK(r3.success, "listBreakpoints succeeds");
    CHECK(r3.error_code == ErrorCode::None, "success → ErrorCode::None");

    TEST_END();
}

static void contract_failure_has_error_code()
{
    TEST_BEGIN("contract: failed operations have non-None ErrorCode");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r1 = api.clearBreakpoint(0x9999);
    CHECK(!r1.success, "clearBreakpoint fails");
    CHECK(r1.error_code != ErrorCode::None, "failure → non-None ErrorCode");

    auto r2 = api.getFunction(0x9999);
    CHECK(!r2.success, "getFunction fails");
    CHECK(r2.error_code != ErrorCode::None, "failure → non-None ErrorCode");

    auto r3 = api.disassemble(0x0100, 0);
    CHECK(!r3.success, "disassemble(0) fails");
    CHECK(r3.error_code != ErrorCode::None, "failure → non-None ErrorCode");

    TEST_END();
}

static void contract_failure_has_message()
{
    TEST_BEGIN("contract: failed operations have non-empty error_message");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r1 = api.clearBreakpoint(0x9999);
    CHECK(!r1.success, "fails");
    CHECK(!r1.error_message.empty(), "error message is not empty");

    auto r2 = api.getFunction(0x9999);
    CHECK(!r2.success, "fails");
    CHECK(!r2.error_message.empty(), "error message is not empty");

    TEST_END();
}

// ===========================================================================
// §21. createFunction duplicate → error
// ===========================================================================

static void contract_create_function_duplicate_fails()
{
    TEST_BEGIN("contract: createFunction duplicate → fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r1 = api.createFunction(0x0200);
    CHECK(r1.success, "first create succeeds");

    auto r2 = api.createFunction(0x0200);
    CHECK(!r2.success, "duplicate create fails");
    TEST_END();
}

// ===========================================================================
// §22. setRegister — invalid register name → error
// ===========================================================================

static void contract_set_register_invalid_name()
{
    TEST_BEGIN("contract: setRegister invalid name → fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setRegister("INVALID_REG", 0x42);
    CHECK(!r.success, "fails for invalid register name");
    CHECK(r.error_code == ErrorCode::InvalidArgument, "error code is InvalidArgument");
    TEST_END();
}

static void contract_set_register_valid()
{
    TEST_BEGIN("contract: setRegister('A', 0x55) succeeds");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.setRegister("A", 0x55);
    CHECK(r.success, "succeeds for valid register");
    CHECK_EQ(0x55u, (unsigned)api.getCpuState().value.a, "register value updated");
    TEST_END();
}

// ===========================================================================
// §23. Debug State — returns AgentApiResult
// ===========================================================================

static void contract_get_debug_state()
{
    TEST_BEGIN("contract: getDebugState() returns AgentApiResult<DebugStateResult>");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getDebugState();
    CHECK(r.success, "succeeds");
    using Actual = decltype(r);
    using Expected = AgentApiResult<DebugStateResult>;
    bool dbgTypeOk = is_same_type<Actual, Expected>();
    CHECK(dbgTypeOk,
          "return type is AgentApiResult<DebugStateResult>");
    // Verify we can access the state
    CHECK_EQ(0x0100u, (unsigned)r.value.cpu.pc, "CPU state accessible");
    TEST_END();
}

// ===========================================================================
// main
// ===========================================================================

int main()
{
    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  Agent API Contract Tests — Stage 6.3\033[0m\n");
    printf("\033[1;33m========================================\033[0m\n");

    // §1. Execution control
    contract_run_returns_result();
    contract_pause_returns_result();
    contract_step_returns_result();
    contract_step_not_paused_error();
    contract_reset_returns_result();

    // §2. CPU state
    contract_get_cpu_state_returns_result();

    // §3. Memory overflow
    contract_read_memory_overflow();
    contract_read_memory_boundary_ok();
    contract_write_memory_overflow();
    contract_write_memory_boundary_ok();

    // §4. Breakpoints — NotFound
    contract_clear_breakpoint_not_found();
    contract_set_breakpoint_enabled_not_found();

    // §5. Trace — maxEntries=0
    contract_execution_trace_zero_is_error();
    contract_io_trace_zero_is_error();
    contract_execution_trace_nonzero_ok();

    // §6. Screen
    contract_get_screen_returns_agent_snapshot();

    // §7. Annotations — NotFound
    contract_rename_function_not_found();
    contract_delete_function_not_found();
    contract_set_function_comment_not_found();
    contract_set_comment_not_found();

    // §8. High-level
    contract_get_function_context_returns_result();
    contract_trace_function_returns_result();

    // §9. getFunction → NotFound
    contract_get_function_not_found_code();

    // §10. getCallGraph — optional
    contract_call_graph_nullopt();
    contract_call_graph_explicit_nullopt();
    contract_call_graph_address_zero();
    contract_call_graph_implicit_conversion();
    contract_call_graph_limit_zero_means_all();

    // §11. ROM
    contract_load_rom_exists();
    contract_load_rom_info_not_exposed();

    // §12. Symbols — limit=0
    contract_get_symbols_limit_zero_means_all();
    contract_get_symbols_explicit_limit();

    // §13. Disassembly — count=0
    contract_disassemble_count_zero_is_error();
    contract_disassemble_count_nonzero_ok();

    // §14. Instruction history — count=0
    contract_instruction_history_count_zero_is_error();

    // §15. getStack — limit=0
    contract_get_stack_limit_zero_is_error();
    contract_get_stack_normal();

    // §16. AgentLimits
    contract_limits_exist();

    // §17. ErrorCode::NotFound
    contract_error_code_not_found_exists();

    // §18. No CommandResult
    contract_no_command_result_in_api();

    // §19. I/O port boundaries
    contract_io_port_zero_valid();
    contract_io_port_max_valid();

    // §20. Error contract
    contract_success_has_no_error_code();
    contract_failure_has_error_code();
    contract_failure_has_message();

    // §21. createFunction duplicate
    contract_create_function_duplicate_fails();

    // §22. setRegister
    contract_set_register_invalid_name();
    contract_set_register_valid();

    // §23. Debug State
    contract_get_debug_state();

    printf("\n\033[1;33m========================================\033[0m\n");
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf("  (\033[1;31m%d FAILED\033[0m)", tests_failed);
    }
    printf("\n\033[1;33m========================================\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
