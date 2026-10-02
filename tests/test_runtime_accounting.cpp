// Runtime memory accounting tests — Stage 6.22 §1–§4 / Stage 6.24 §1–§2
//
// The free-run path (Board/NoBoardTarget executing instructions without the
// debugger stepping them) used to report zero fetches, a port-trace sequence
// that never moved, and an execute_activity field that was always 0.  These
// tests drive the real DebugBackend + NoBoardTarget over a fixed program and
// pin the numbers down.
//
// Stage 6.24 additions:
//   • Instruction-start detection via MemoryReadCallback's pc param
//     replaces the src/-side oninstrbegin hook.
//   • Regression tests for the virt+1==pc heuristic cover: opcode fetch,
//     multi-byte instructions, RD_BYTE(HL), stack accesses, step vs free-run,
//     JMP/CALL/RET, and the 0xFFFF wrap.
//   • The known false-positive case (ADD M with HL == instruction address)
//     is documented with its actual observed behaviour, not suppressed.
//
// Program (11 instruction bytes, 1 data read + 1 data write per loop):
//   0x0100  3E 42     MVI A,42h
//   0x0102  32 00 02  STA 0200h
//   0x0105  3A 01 02  LDA 0200h
//   0x0108  C3 00 01  JMP 0100h
//   0x0200  ..        data byte written and read back every loop

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "memory.h"
#include "i8080.h"
#include "i8080_hal.h"
#include "no_board_target.h"
#include "backend.h"
#include "mcp_json.h"

using namespace i8080cpu;

static Memory *test_memory = nullptr;

int i8080_hal_memory_read_byte(int addr)  { return test_memory->read(addr, false); }
void i8080_hal_memory_write_byte(int addr, int value) { test_memory->write(addr, value, false); }
int i8080_hal_memory_read_word(int addr, bool stack)
{
    return test_memory->read(addr, stack) | (test_memory->read(addr + 1, stack) << 8);
}
void i8080_hal_memory_write_word(int addr, int word, bool stack)
{
    test_memory->write(addr, word & 0xff, stack);
    test_memory->write(addr + 1, word >> 8, stack);
}
int i8080_hal_io_input(int port) { (void)port; return 0xFF; }
void i8080_hal_io_output(int port, int value) { (void)port; (void)value; }
void i8080_hal_iff(int on) { (void)on; }

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
        auto _e = (exp); auto _a = (act); \
        if (_e != _a) { \
            printf("  \033[41;97m FAIL \033[0m %s: expected %lld, got %lld (line %d)\n", \
                   msg, (long long)_e, (long long)_a, __LINE__); \
            _test_ok = false; \
        } else { \
            printf("  \033[46;30m ok \033[0m %s\n", msg); \
        } \
    } while(0)

#define TEST_END() \
        if (_test_ok) { \
            tests_passed++; \
            printf("  \033[46;30m PASSED \033[0m %s\n", _test_name); \
        } else { \
            tests_failed++; \
            printf("  \033[41;97m FAILED \033[0m %s\n", _test_name); \
        } \
    } while(0)

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

static const uint8_t kProgram[] = {
    0x3E, 0x42,             // 0x0100  MVI A,42h
    0x32, 0x00, 0x02,       // 0x0102  STA 0200h
    0x3A, 0x01, 0x02,       // 0x0105  LDA 0200h
    0xC3, 0x00, 0x01,       // 0x0108  JMP 0100h
};
static const int    kOrigin   = 0x0100;
static const size_t kProgLen  = sizeof(kProgram);
static const int    kBytesPerLoop = 11;   // 2 + 3 + 3 + 3

struct Fixture
{
    Memory        mem;
    NoBoardTarget target;
    DebugBackend  backend;

    Fixture() : target(mem), backend(target)
    {
        test_memory = &mem;
        for (size_t i = 0; i < kProgLen; ++i)
            mem.write(static_cast<uint32_t>(kOrigin + i), kProgram[i], false);
        i8080_init();
        i8080_jump(kOrigin);
        i8080_setreg_sp(0xC000);
        // The program bytes above were written while the instrumentation was
        // already live — they would show up as 11 writes to the code block and
        // as log entries with pc 0.  Counters must describe the run, not the
        // loader.
        backend.clearRuntimeAccessMap();
    }
};

static RuntimeAccessBlock blockOf(DebugBackend &be, int block)
{
    auto map = be.getRuntimeAccessMap();
    return map[block];
}

// ---------------------------------------------------------------------------
// Test 1 — §1: fetches are accounted for in the free run
// ---------------------------------------------------------------------------

static void test_free_run_fetch_accounting()
{
    TEST_BEGIN("free run counts instruction fetches separately from data reads");
    Fixture f;
    const int loops = 10;

    // NoBoardTarget::executeFrame() runs exactly one instruction.
    for (int i = 0; i < loops * 4; ++i)
        f.target.executeFrame();

    const RuntimeAccessBlock code = blockOf(f.backend, kOrigin >> 8);
    const RuntimeAccessBlock data = blockOf(f.backend, 0x0200 >> 8);

    CHECK(code.fetch, "code block marked fetch");
    CHECK_EQ(loops * kBytesPerLoop, (int)code.fetch_count,
             "fetch_count = bytes of the instructions executed");
    CHECK_EQ(0, (int)code.read_count,
             "operand bytes are fetches, not data reads — nothing left in read_count");
    CHECK_EQ(0, (int)code.write_count, "no writes into the code block (no self-modifying code)");

    CHECK(!data.fetch, "data block is never marked fetch");
    CHECK_EQ(loops, (int)data.read_count, "LDA reads the data byte once per loop");
    CHECK_EQ(loops, (int)data.write_count, "STA writes the data byte once per loop");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 2 — §1: step and free run agree on the same stretch of code
// ---------------------------------------------------------------------------

static void test_step_and_free_run_agree()
{
    TEST_BEGIN("debug_step and the free run produce the same access map");
    const int instrs = 40;

    Fixture free_run;
    for (int i = 0; i < instrs; ++i) free_run.target.executeFrame();
    auto byRun = free_run.backend.getRuntimeAccessMap();

    Fixture stepped;
    for (int i = 0; i < instrs; ++i) stepped.backend.stepInstruction();
    auto byStep = stepped.backend.getRuntimeAccessMap();

    int diffBlocks = 0;
    for (int b = 0; b < 256; ++b) {
        if (byRun[b].fetch_count != byStep[b].fetch_count ||
            byRun[b].read_count  != byStep[b].read_count  ||
            byRun[b].write_count != byStep[b].write_count) {
            ++diffBlocks;
        }
    }
    CHECK_EQ(0, diffBlocks, "every block matches between the two execution paths");
    CHECK(byRun[1].fetch_count > 0, "and the numbers are not trivially zero");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 3 — §2: the instruction sequence advances during the free run
// ---------------------------------------------------------------------------

static void test_sequence_advances_in_free_run()
{
    TEST_BEGIN("instruction sequence grows in the free run and matches the step path");
    Fixture f;

    // First instruction of a fresh session is number 0 — the same number the
    // step path gives it, so a log entry cannot tell which path produced it.
    f.target.executeFrame();
    auto log = f.backend.getRuntimeAccessLog(100);
    CHECK(!log.empty(), "free run produced runtime access log entries");
    CHECK_EQ(0u, (unsigned)(log.front().address - log.front().pc),
             "first fetch is at pc + 0");

    uint64_t seqAfterOne = f.backend.instructionSequence();
    CHECK_EQ(0u, (unsigned)seqAfterOne, "sequence still 0 after the first instruction");

    for (int i = 0; i < 9; ++i) f.target.executeFrame();
    CHECK_EQ(9u, (unsigned)f.backend.instructionSequence(),
             "sequence reached 9 after 10 free-run instructions");

    // Stepping keeps its own long-standing numbering: one per instruction.
    Fixture s;
    for (int i = 0; i < 3; ++i) s.backend.stepInstruction();
    CHECK_EQ(3u, (unsigned)s.backend.instructionSequence(),
             "3 steps leave sequence at 3");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 4 — §1: the log's pc is the address of the accessing instruction
// ---------------------------------------------------------------------------

static void test_log_pc_and_fetch_window()
{
    TEST_BEGIN("log entries carry the accessing instruction and fetches stay inside it");
    Fixture f;
    for (int i = 0; i < 4; ++i) f.target.executeFrame();   // one full loop

    auto log = f.backend.getRuntimeAccessLog(100);
    int fetches = 0, reads = 0;
    bool windowOk = true;
    for (const auto &e : log) {
        if (e.type == RuntimeAccessLogEntry::Fetch) {
            ++fetches;
            if (e.address < e.pc || e.address > e.pc + 2) windowOk = false;
        } else if (e.type == RuntimeAccessLogEntry::Read) {
            ++reads;
        }
        if (e.pc < kOrigin || e.pc > kOrigin + (int)kProgLen) windowOk = false;
    }
    CHECK_EQ(kBytesPerLoop, fetches, "one loop = 11 fetched bytes logged");
    CHECK(fetches > 0, "fetch entries exist at all (regression of §1)");
    CHECK(windowOk, "every fetch lies within pc..pc+2 and pc is an instruction address");

    bool dataReadTagged = false;
    for (const auto &e : log)
        if (e.type == RuntimeAccessLogEntry::Read && e.address == 0x0201 && e.pc == 0x0105)
            dataReadTagged = true;
    CHECK(dataReadTagged, "the LDA data read is logged against the LDA instruction (0x0105)");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 5 — §4: execute counters are filled by the free run
// ---------------------------------------------------------------------------

static void test_execute_activity_in_free_run()
{
    TEST_BEGIN("per-address execute counters grow in the free run");
    Fixture f;
    for (int i = 0; i < 40; ++i) f.target.executeFrame();   // 10 loops

    auto snap = f.backend.activitySnapshot();
    CHECK_EQ(10u, (unsigned)snap.executeCount[0x0100], "MVI A executed 10 times");
    CHECK_EQ(10u, (unsigned)snap.executeCount[0x0102], "STA 0200h executed 10 times");
    CHECK_EQ(10u, (unsigned)snap.executeCount[0x0105], "LDA 0200h executed 10 times");
    CHECK_EQ(10u, (unsigned)snap.executeCount[0x0108], "JMP executed 10 times");
    CHECK_EQ(0u, (unsigned)snap.executeCount[0x0103], "an operand byte is not an entry point");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 6 — §3: active_blocks counts what the payload actually lists
// ---------------------------------------------------------------------------

static void test_active_blocks_match_payload()
{
    TEST_BEGIN("active block count matches the serialised block list");
    Fixture f;
    for (int i = 0; i < 40; ++i) f.target.executeFrame();   // 10 loops

    const auto map = f.backend.getRuntimeAccessMap();        // all 256 blocks

    int active = 0;
    for (const auto &b : map)
        if (mcp_json::runtimeBlockActive(b)) ++active;
    const auto arr = mcp_json::runtimeAccessBlocksToJson(map);

    CHECK_EQ(256u, (unsigned)map.size(), "the map keeps all 256 blocks");
    CHECK_EQ((unsigned)active, (unsigned)arr.size(),
             "active_blocks == len(blocks) — both use the same predicate");
    CHECK(active > 0 && active < 256, "the filter really filters");

    // The code block is fetched-only: no data read and no write happens in
    // 0x0100–0x01FF.  Until §1 landed it was invisible to the MCP response.
    bool codeListed = false, dataList = false;
    for (const auto &j : arr) {
        const std::string addr = j.value("address", std::string());
        if (addr == mcp_json::hex16(0x0100)) {
            codeListed = true;
            CHECK(j["fetch"].get<bool>(), "code block carries the fetch flag");
            CHECK_EQ(110, (int)j["fetch_count"].get<int>(), "110 fetched bytes");
            CHECK_EQ(0, (int)j["read_count"].get<int>(), "and no data reads attributed to it");
        }
        if (addr == mcp_json::hex16(0x0200)) dataList = true;
    }
    CHECK(codeListed, "a fetch-only block is listed in debug_get_memory_access_map");
    CHECK(dataList, "the data block is listed too");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.24: helper fixture for custom programs
// ---------------------------------------------------------------------------

// Fixture2 accepts an arbitrary program blob and origin.
// It clears the runtime access map after loading so only the run is measured.
// executeCount_ is zero at this point because program writes do not
// increment it (only onMemoryRead via instruction detection does).
struct Fixture2
{
    Memory        mem;
    NoBoardTarget target;
    DebugBackend  backend;

    Fixture2(const uint8_t *prog, int origin, int len)
        : target(mem), backend(target)
    {
        test_memory = &mem;
        for (int i = 0; i < len; ++i)
            mem.write(static_cast<uint32_t>(origin + i), prog[i], false);
        i8080_init();
        i8080_jump(origin);
        i8080_setreg_sp(0xC000);
        backend.clearRuntimeAccessMap();
    }
};

// ---------------------------------------------------------------------------
// Stage 6.24 Test A — ADD M normal case: HL points to RAM, not self-referencing
//   0x0100  21 00 02   LXI H,0200h
//   0x0103  86         ADD M   ← reads [0x0200], a data area
//   0x0104  00         NOP
// ---------------------------------------------------------------------------

static void test_624_add_m_hl_ram()
{
    TEST_BEGIN("6.24: ADD M with HL in RAM — data read classified as Read");
    const uint8_t prog[] = {0x21,0x00,0x02, 0x86, 0x00};
    Fixture2 f(prog, 0x0100, sizeof(prog));

    // 3 free-run instructions: LXI H, ADD M, NOP
    for (int i = 0; i < 3; ++i) f.target.executeFrame();

    const RuntimeAccessBlock code = blockOf(f.backend, 0x0100 >> 8);
    const RuntimeAccessBlock data = blockOf(f.backend, 0x0200 >> 8);

    // Code block: 3 + 1 + 1 = 5 fetched bytes (LXI H=3, ADD M opcode=1, NOP=1)
    CHECK(code.fetch, "code block marked fetch");
    CHECK_EQ(5, (int)code.fetch_count,
             "5 instruction bytes fetched (3+1+1)");
    // Data read from 0x0200 must NOT appear in code block's read_count
    CHECK_EQ(0, (int)code.read_count,
             "no data reads in code block (HL points elsewhere)");
    // Block 0x0200: 1 data read from [HL]
    CHECK(!data.fetch, "data block 0x0200 not marked fetch");
    CHECK_EQ(1, (int)data.read_count,
             "ADD M data read counted as Read in data block");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.24 Test B — ADD M self-referencing: HL == instruction address (known FP)
//   0x0100  21 03 01   LXI H,0103h
//   0x0103  86         ADD M   ← reads [0x0103] which is the ADD M opcode itself
//   0x0104  00         NOP
//
// Known limitation: after the opcode fetch (fetchRemaining=1→0), the data read
// from HL=0x0103 satisfies virt+1==cpuPc (0x0103+1=0x0104==i8080_pc()).
// The heuristic fires a second time, double-counting executeCount_[0x0103].
// This test documents the ACTUAL behaviour so it is tracked, not suppressed.
// ---------------------------------------------------------------------------

static void test_624_add_m_false_positive_self_ref()
{
    TEST_BEGIN("6.24: ADD M with HL==instr addr — documents false-positive behaviour");
    const uint8_t prog[] = {0x21,0x03,0x01, 0x86, 0x00};
    Fixture2 f(prog, 0x0100, sizeof(prog));

    for (int i = 0; i < 3; ++i) f.target.executeFrame();

    auto snap = f.backend.activitySnapshot();
    // False positive: executeCount_[0x0103] is incremented twice
    // (once by opcode-fetch detection, once by data-read false-positive).
    // This is the documented behaviour of the virt+1==pc heuristic.
    CHECK_EQ(2u, (unsigned)snap.executeCount[0x0103],
             "FP documented: executeCount[ADD-M-addr]==2 in free-run (opcode fetch + data read both trigger detection)");
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0100],
             "LXI H counted once (no false positive there)");

    // fetch_count for code block: LXI H(3) + ADD-M opcode(1) +
    // ADD-M data-as-FP-fetch(1) + NOP(1) = 6 (one more than 5 true fetches)
    const RuntimeAccessBlock code = blockOf(f.backend, 0x0100 >> 8);
    CHECK_EQ(6, (int)code.fetch_count,
             "FP documented: 6 fetches in code block (5 true + 1 misclassified data-read)");
    CHECK_EQ(0, (int)code.read_count,
             "FP documented: data read appears as FETCH, not Read");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.24 Test C — Stack accesses (PUSH/POP) must NOT trigger detection
//   0x0100  C5   PUSH B
//   0x0101  D1   POP B
//   0x0102  00   NOP
// ---------------------------------------------------------------------------

static void test_624_stack_not_false_positive()
{
    TEST_BEGIN("6.24: PUSH/POP stack accesses do not falsely trigger detection");
    const uint8_t prog[] = {0xC5, 0xD1, 0x00};
    Fixture2 f(prog, 0x0100, sizeof(prog));

    for (int i = 0; i < 3; ++i) f.target.executeFrame();

    auto snap = f.backend.activitySnapshot();
    // Each instruction executed exactly once (stack reads don't cause FP)
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0100], "PUSH B: 1 execution");
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0101], "POP B: 1 execution");
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0102], "NOP: 1 execution");

    // Code block: 3 fetches (1+1+1 opcodes). No stack byte is in code block.
    const RuntimeAccessBlock code = blockOf(f.backend, 0x0100 >> 8);
    CHECK_EQ(3, (int)code.fetch_count, "3 opcode bytes fetched");

    // Stack block (0xBFFE-0xBFFF is block 0xBF): POP reads 2 stack bytes.
    const RuntimeAccessBlock stk = blockOf(f.backend, 0xBF);
    CHECK(stk.read_count >= 1u,
          "stack area block has at least one read (POP stack accesses)");
    CHECK(!stk.fetch, "stack block is never marked fetch");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.24 Test D — CALL/RET: new instruction at target and after return
//   0x0100  CD 06 01   CALL 0106h
//   0x0103  C3 00 01   JMP  0100h
//   0x0106  C9         RET
//   After 3 free-run instructions: CALL, RET, JMP (one complete loop)
// ---------------------------------------------------------------------------

static void test_624_call_ret_sequence()
{
    TEST_BEGIN("6.24: CALL/RET — each instruction counted exactly once per loop");
    // CALL target = 0x0107, JMP target = 0x0100, RET at 0x0107
    // Execution order: CALL(0x0100) → RET(0x0107) → JMP(0x0103)
    const uint8_t prog[] = {0xCD,0x07,0x01, 0xC3,0x00,0x01, 0x00, 0xC9};
    Fixture2 f(prog, 0x0100, sizeof(prog));

    for (int i = 0; i < 3; ++i) f.target.executeFrame();

    auto snap = f.backend.activitySnapshot();
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0100], "CALL executed once");
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0107], "RET executed once");
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0103], "JMP executed once (after return)");

    // CALL operand bytes at 0x0101,0x0102 must be FETCH (fetchRemaining>0)
    const RuntimeAccessBlock code = blockOf(f.backend, 0x0100 >> 8);
    // Total fetch: CALL(3) + RET(1) + JMP(3) = 7
    CHECK_EQ(7, (int)code.fetch_count,
             "7 total fetch bytes: CALL(3)+RET(1)+JMP(3)");
    // No data reads in code block
    CHECK_EQ(0, (int)code.read_count,
             "no data reads misclassified in code block");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.24 Test E — JCC taken and not-taken: correct detection
//   0x0100  3E 01       MVI A,01h   (Z=0)
//   0x0102  C2 08 01    JNZ 0108h   (taken: Z=0)
//   0x0105  00          NOP         (NOT reached)
//   0x0108  00          NOP         (reached via JNZ taken)
// ---------------------------------------------------------------------------

static void test_624_jcc_taken_not_taken()
{
    TEST_BEGIN("6.24: JCC taken — NOP at target counted, NOP after JCC not counted");
    const uint8_t prog[] = {0x3E,0x01, 0xC2,0x08,0x01, 0x00, 0x00, 0x00, 0x00};
    // prog indices: 0=3E,1=01,2=C2,3=08,4=01,5=00(0x0105),6=00,7=00,8=00(0x0108)
    Fixture2 f(prog, 0x0100, sizeof(prog));

    // 3 instructions: MVI A(1), JNZ taken(2), NOP@0x0108(3)
    for (int i = 0; i < 3; ++i) f.target.executeFrame();

    auto snap = f.backend.activitySnapshot();
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0100], "MVI A counted");
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0102], "JNZ counted");
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0108], "NOP at JNZ target counted");
    CHECK_EQ(0u, (unsigned)snap.executeCount[0x0105],
             "NOP after JNZ (not-taken path) NOT counted");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.24 Test F — 0xFFFF boundary: PC wraps, detection still works
//   0x0000  C3 FF FF   JMP 0FFFFh
//   0xFFFF  00         NOP
//   Run 4 instructions: JMP(0x0000), NOP(0xFFFF), JMP(0x0000), NOP(0xFFFF)
// ---------------------------------------------------------------------------

static void test_624_boundary_ffff()
{
    TEST_BEGIN("6.24: NOP at 0xFFFF — PC wrap handled by virt+1 cast correctly");
    Memory        mem;
    NoBoardTarget target(mem);
    DebugBackend  backend(target);
    test_memory = &mem;

    // JMP 0xFFFF at address 0x0000
    mem.write(0x0000, 0xC3, false);
    mem.write(0x0001, 0xFF, false);
    mem.write(0x0002, 0xFF, false);
    // NOP at 0xFFFF
    mem.write(0xFFFF, 0x00, false);

    i8080_init();
    i8080_jump(0x0000);
    i8080_setreg_sp(0xC000);
    backend.clearRuntimeAccessMap();
    // clearActivityCounters() zeroes executeCount_ (already zero at this
    // point — mem.write() fires onMemoryWrite, not onMemoryRead).
    backend.clearActivityCounters();

    // 4 instructions: JMP→NOP(0xFFFF)→JMP→NOP(0xFFFF)
    for (int i = 0; i < 4; ++i) target.executeFrame();

    auto snap = backend.activitySnapshot();
    CHECK_EQ(2u, (unsigned)snap.executeCount[0x0000],
             "JMP at 0x0000 counted twice");
    CHECK_EQ(2u, (unsigned)snap.executeCount[0xFFFF],
             "NOP at 0xFFFF counted twice (PC wraps to 0, detection works)");

    // Block 255 (0xFF00–0xFFFF) must be marked fetch
    const RuntimeAccessBlock last = blockOf(backend, 255);
    CHECK(last.fetch, "last block (255) marked fetch for NOP at 0xFFFF");
    CHECK_EQ(2, (int)last.fetch_count,
             "2 fetch bytes in block 255 (NOP at 0xFFFF × 2)");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.24 Test G — Step mode: steppingInProgress_ guard prevents FP detection
//   Uses same program as Test B (ADD M self-ref) but via backend.stepInstruction()
// ---------------------------------------------------------------------------

static void test_624_step_mode_no_double_count()
{
    TEST_BEGIN("6.24: step mode — steppingInProgress_ prevents FP double-count");
    const uint8_t prog[] = {0x21,0x03,0x01, 0x86, 0x00};
    Fixture2 f(prog, 0x0100, sizeof(prog));

    // Same self-referencing ADD M but via step path
    for (int i = 0; i < 3; ++i) f.backend.stepInstruction();

    auto snap = f.backend.activitySnapshot();
    // In step mode: executeCount_[0x0103] incremented exactly ONCE
    // (by stepInstructionDetailed(), NOT by the onMemoryRead() detection
    //  which is guarded by steppingInProgress_).
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0103],
             "step mode: ADD M addr counted exactly once (FP guard works)");

    // Code block: true fetch bytes only (5), no extra FP fetch
    const RuntimeAccessBlock code = blockOf(f.backend, 0x0100 >> 8);
    CHECK_EQ(5, (int)code.fetch_count,
             "step mode: 5 fetch bytes (no FP extra fetch)");
    // The data read from 0x0103 IS classified as Read (not FP Fetch)
    CHECK_EQ(1, (int)code.read_count,
             "step mode: data read at 0x0103 correctly classified as Read");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.24 Test H — Multi-byte instruction: operand bytes are FETCH, not Read
//   Already tested in Test 1; this pins the specific case of a 3-byte JMP
//   to verify that operand fetches happen while fetchRemaining_ > 0 and do
//   not spuriously re-trigger detection.
//   0x0100  C3 0A 01   JMP 0x010A
//   0x010A  00         NOP
// ---------------------------------------------------------------------------

static void test_624_multibyte_no_retrigger()
{
    TEST_BEGIN("6.24: JMP operand bytes do not re-trigger detection");
    // JMP 0x010A at 0x0100 (3 bytes), NOP at 0x010A (index 10)
    const uint8_t prog[] = {0xC3,0x0A,0x01,
                            0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                            0x00 /* 0x010A */};
    Fixture2 f(prog, 0x0100, sizeof(prog));

    // 2 instructions: JMP at 0x0100, NOP at 0x010A
    for (int i = 0; i < 2; ++i) f.target.executeFrame();

    auto snap = f.backend.activitySnapshot();
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x0100], "JMP counted once");
    CHECK_EQ(1u, (unsigned)snap.executeCount[0x010A], "NOP counted once");
    // Intermediate operand addresses must NOT have execute counts
    CHECK_EQ(0u, (unsigned)snap.executeCount[0x0101],
             "JMP operand byte 0x0101 is not an entry point");
    CHECK_EQ(0u, (unsigned)snap.executeCount[0x0102],
             "JMP operand byte 0x0102 is not an entry point");

    // fetch_count: JMP(3) + NOP(1) = 4, read_count in code block = 0
    const RuntimeAccessBlock code = blockOf(f.backend, 0x0100 >> 8);
    CHECK_EQ(4, (int)code.fetch_count, "JMP(3)+NOP(1)=4 fetch bytes");
    CHECK_EQ(0, (int)code.read_count, "operand bytes are Fetch, not Read");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Stage 6.25 — sequence field on RuntimeAccessLogEntry
// ---------------------------------------------------------------------------

// Test 6.25-A — monotonic sequence across reads and writes.
//   Reuses the main kProgram fixture (MVI/STA/LDA/JMP loop).  After one
//   full loop we expect strictly increasing sequence numbers and the
//   counter shared across Read/Write/Fetch types (i.e. it is a global log
//   index, not a per-type ordinal).
static void test_625_sequence_monotonic_across_read_and_write()
{
    TEST_BEGIN("6.25: sequence is strictly increasing across read/write/fetch");
    Fixture f;
    for (int i = 0; i < 4; ++i) f.target.executeFrame();   // one loop

    auto log = f.backend.getRuntimeAccessLog(1000);
    CHECK(log.size() >= 4u, "loop produced several access entries");

    bool monotonic = true;
    bool hasRead = false, hasWrite = false, hasFetch = false;
    for (size_t i = 1; i < log.size(); ++i) {
        if (log[i].sequence <= log[i - 1].sequence) { monotonic = false; break; }
    }
    for (const auto &e : log) {
        if (e.type == RuntimeAccessLogEntry::Read)  hasRead  = true;
        if (e.type == RuntimeAccessLogEntry::Write) hasWrite = true;
        if (e.type == RuntimeAccessLogEntry::Fetch) hasFetch = true;
    }
    CHECK(monotonic, "sequence is strictly increasing across all entries");
    CHECK(hasFetch,  "loop produced fetch entries");
    CHECK(hasWrite,  "STA produced write entry");
    CHECK(hasRead,   "LDA produced read entry");
    TEST_END();
}

// Test 6.25-B — clearMemoryAccessLog() clears only the log; the aggregated
// runtime map survives (§18 исходного ТЗ).
static void test_625_clear_access_log_preserves_map()
{
    TEST_BEGIN("6.25: clearMemoryAccessLog leaves runtime map untouched");
    Fixture f;
    for (int i = 0; i < 4; ++i) f.target.executeFrame();

    // Precondition: both log and map have accumulated data.
    CHECK(!f.backend.getRuntimeAccessLog(10).empty(),
          "log is not empty before clear");
    const RuntimeAccessBlock codeBefore = blockOf(f.backend, kOrigin >> 8);
    CHECK(codeBefore.fetch_count > 0 || codeBefore.read_count > 0 ||
          codeBefore.write_count > 0,
          "map has activity before clear");

    f.backend.clearMemoryAccessLog();

    CHECK(f.backend.getRuntimeAccessLog(10).empty(),
          "log is empty after clearMemoryAccessLog");
    const RuntimeAccessBlock codeAfter = blockOf(f.backend, kOrigin >> 8);
    CHECK_EQ((long long)codeBefore.fetch_count, (long long)codeAfter.fetch_count,
             "fetch_count preserved");
    CHECK_EQ((long long)codeBefore.read_count,  (long long)codeAfter.read_count,
             "read_count preserved");
    CHECK_EQ((long long)codeBefore.write_count, (long long)codeAfter.write_count,
             "write_count preserved");
    CHECK(codeAfter.fetch, "aggregated map flags preserved (block still marked fetch)");
    TEST_END();
}

// Test 6.25-C — after clearRuntimeAccessMap() the sequence counter restarts
// so subsequent entries are numbered from 1 (loadRom() takes this path).
static void test_625_clear_runtime_map_resets_sequence()
{
    TEST_BEGIN("6.25: clearRuntimeAccessMap restarts sequence from 1");
    Fixture f;
    for (int i = 0; i < 4; ++i) f.target.executeFrame();
    auto first = f.backend.getRuntimeAccessLog(1);
    CHECK(!first.empty(), "entries exist before reset");

    f.backend.clearRuntimeAccessMap();
    for (int i = 0; i < 4; ++i) f.target.executeFrame();
    auto second = f.backend.getRuntimeAccessLog(1000);
    CHECK(!second.empty(), "entries exist after reset");
    if (!second.empty()) {
        CHECK_EQ(1u, (unsigned long long)second.front().sequence,
                 "first entry after reset has sequence == 1");
    }
    TEST_END();
}

// Test 6.25-D — PUSH/POP stack writes and reads appear in the log so the
// GUI can show them (spec §23 “Stack”).
static void test_625_stack_writes_and_reads_present()
{
    TEST_BEGIN("6.25: PUSH/POP produce stack write/read entries");
    //   0x0100  C5        PUSH B
    //   0x0101  D1        POP  D
    //   0x0102  C9        RET            — unreachable but keeps disasm quiet
    const uint8_t prog[] = {0xC5, 0xD1, 0xC9};
    Fixture2 f(prog, 0x0100, sizeof(prog));

    for (int i = 0; i < 2; ++i) f.target.executeFrame();
    auto log = f.backend.getRuntimeAccessLog(100);

    int stackWrites = 0, stackReads = 0;
    for (const auto &e : log) {
        // After PUSH B SP starts at 0xC000 and decrements — the writes land
        // at 0xBFFE/0xBFFF; POP reads back at the same range.
        if (e.address >= 0xB000 && e.address < 0xC000) {
            if (e.type == RuntimeAccessLogEntry::Write) ++stackWrites;
            if (e.type == RuntimeAccessLogEntry::Read)  ++stackReads;
        }
    }
    CHECK_EQ(2, stackWrites, "PUSH B wrote two stack bytes");
    CHECK_EQ(2, stackReads,  "POP D read  two stack bytes");
    TEST_END();
}

// Test 6.25-E — CALL/RET stack accesses appear (spec §23 “Stack”).
static void test_625_call_ret_stack_writes_and_reads()
{
    TEST_BEGIN("6.25: CALL then RET produce one stack write and one read");
    //   0x0100  CD 06 01   CALL 0106h
    //   0x0103  C9         RET
    //   0x0104  00         NOP
    //   0x0105  00         NOP
    //   0x0106  C9         RET  (callee returns immediately)
    const uint8_t prog[] = {0xCD,0x06,0x01, 0xC9, 0x00, 0x00, 0xC9};
    Fixture2 f(prog, 0x0100, sizeof(prog));

    // CALL, then RET in callee, then RET from main-loop start.  3 instructions.
    for (int i = 0; i < 3; ++i) f.target.executeFrame();
    auto log = f.backend.getRuntimeAccessLog(100);

    int stackWrites = 0, stackReads = 0;
    for (const auto &e : log) {
        if (e.address >= 0xB000 && e.address < 0xC000) {
            if (e.type == RuntimeAccessLogEntry::Write) ++stackWrites;
            if (e.type == RuntimeAccessLogEntry::Read)  ++stackReads;
        }
    }
    CHECK_EQ(2, stackWrites, "CALL wrote 2 bytes of return address to stack");
    CHECK_EQ(2, stackReads,  "RET read  2 bytes of return address from stack");
    TEST_END();
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf("[runtime_accounting] Stage 6.22 §1–§4 / Stage 6.24 / Stage 6.25\n");

    test_free_run_fetch_accounting();
    test_step_and_free_run_agree();
    test_sequence_advances_in_free_run();
    test_log_pc_and_fetch_window();
    test_execute_activity_in_free_run();
    test_active_blocks_match_payload();

    // Stage 6.24 regression tests for instruction-start detection heuristic
    test_624_add_m_hl_ram();
    test_624_add_m_false_positive_self_ref();
    test_624_stack_not_false_positive();
    test_624_call_ret_sequence();
    test_624_jcc_taken_not_taken();
    test_624_boundary_ffff();
    test_624_step_mode_no_double_count();
    test_624_multibyte_no_retrigger();

    // Stage 6.25 tests: sequence field and clearMemoryAccessLog()
    test_625_sequence_monotonic_across_read_and_write();
    test_625_clear_access_log_preserves_map();
    test_625_clear_runtime_map_resets_sequence();
    test_625_stack_writes_and_reads_present();
    test_625_call_ret_stack_writes_and_reads();

    std::printf("\n[runtime_accounting] %d/%d tests passed (%d failed)\n",
                tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
