// ROM Load Breakpoint Reset tests — Stage 6.20.1
//
// Regression tests verifying that breakpoints from a previous ROM session
// are cleared on successful ROM load, and preserved on failed ROM load.
// Uses real DebugBackend + NoBoardTarget.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

#include "agent_api.h"
#include "backend.h"
#include "memory.h"
#include "no_board_target.h"
#include "debug_target.h"
#include "events.h"
#include "options.h"

// ---------------------------------------------------------------------------
// HAL stubs (needed by i8080.cpp)
// ---------------------------------------------------------------------------

static Memory       *hal_memory = nullptr;
static DebugBackend *hal_dbg    = nullptr;
static bool          hal_iff    = false;

int i8080_hal_memory_read_byte(int addr) { return hal_memory->read(addr, false); }
void i8080_hal_memory_write_byte(int addr, int value) { hal_memory->write(addr, value, false); }
int i8080_hal_memory_read_word(int addr, bool stack) {
    return hal_memory->read(addr, stack) | (hal_memory->read(addr + 1, stack) << 8);
}
void i8080_hal_memory_write_word(int addr, int word, bool stack) {
    hal_memory->write(addr, word & 0xff, stack);
    hal_memory->write(addr + 1, word >> 8, stack);
}
int i8080_hal_io_input(int port) { if (hal_dbg) hal_dbg->onIoInput((uint8_t)port, 0xff); return 0xff; }
void i8080_hal_io_output(int port, int value) { if (hal_dbg) hal_dbg->onIoOutput((uint8_t)port, (uint8_t)value); }
void i8080_hal_iff(int on) { hal_iff = (on != 0); }

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
                printf("  \033[41;97m FAIL \033[0m %s: expected %u, got %u (line %d)\n", \
                       msg, _e, _a, __LINE__); \
                _test_ok = false; \
            } else { \
                printf("  \033[46;30m ok \033[0m %s = %u\n", msg, _a); \
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
// Helpers: create temporary ROM files
// ---------------------------------------------------------------------------

static const char *TEMP_ROM_A = "/tmp/test_rom_a.rom";
static const char *TEMP_ROM_B = "/tmp/test_rom_b.rom";

static void writeTempRom(const char *path, const std::vector<uint8_t> &data)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    f.close();
}

static void cleanupTempRoms()
{
    std::remove(TEMP_ROM_A);
    std::remove(TEMP_ROM_B);
}

// Simple test ROM: just a few NOPs + HLT
static std::vector<uint8_t> makeTestRom()
{
    return { 0x00, 0x00, 0x00, 0x76 };  // NOP NOP NOP HLT
}

// ---------------------------------------------------------------------------
// Test fixture: real DebugBackend + NoBoardTarget
// ---------------------------------------------------------------------------

struct Fixture {
    Memory mem;
    NoBoardTarget *target;
    DebugBackend  *backend;
    AgentApi      *api;

    Fixture() {
        Options.novideo = true;
        Options.nosound = true;
        target  = new NoBoardTarget(mem);
        backend = new DebugBackend(*target);
        backend->testSynchronous_ = true;
        backend->reset();
        api = new AgentApi(*backend);

        hal_memory = &mem;
        hal_dbg    = backend;
    }

    ~Fixture() {
        delete api;
        delete backend;
        delete target;
        hal_dbg = nullptr;
    }

    int breakpointCount() {
        auto r = api->listBreakpoints();
        return r.success ? static_cast<int>(r.value.size()) : -1;
    }

    bool hasBreakpointAt(uint16_t addr) {
        auto r = api->listBreakpoints();
        if (!r.success) return false;
        for (const auto &bp : r.value) {
            if (bp.address == addr) return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Test 1: Successful ROM load clears breakpoints
// ---------------------------------------------------------------------------

static void test_successful_rom_load_clears_breakpoints()
{
    TEST_BEGIN("successful ROM load clears breakpoints");

    writeTempRom(TEMP_ROM_A, makeTestRom());
    writeTempRom(TEMP_ROM_B, makeTestRom());

    Fixture f;

    // Load ROM A
    auto lr1 = f.api->loadRom(TEMP_ROM_A, 0);
    CHECK(lr1.success, "ROM A loaded");

    // Add breakpoints
    auto bp1 = f.api->setBreakpoint(0x0100);
    auto bp2 = f.api->setBreakpoint(0x0200);
    CHECK(bp1.success, "breakpoint 0x0100 added");
    CHECK(bp2.success, "breakpoint 0x0200 added");
    CHECK_EQ(2, f.breakpointCount(), "2 breakpoints before ROM B load");

    // Load ROM B
    auto lr2 = f.api->loadRom(TEMP_ROM_B, 0);
    CHECK(lr2.success, "ROM B loaded");

    // Breakpoints must be cleared
    CHECK_EQ(0, f.breakpointCount(), "0 breakpoints after ROM B load");

    cleanupTempRoms();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 2: Failed ROM load preserves breakpoints
// ---------------------------------------------------------------------------

static void test_failed_rom_load_preserves_breakpoints()
{
    TEST_BEGIN("failed ROM load preserves breakpoints");

    writeTempRom(TEMP_ROM_A, makeTestRom());

    Fixture f;

    // Load ROM A
    auto lr1 = f.api->loadRom(TEMP_ROM_A, 0);
    CHECK(lr1.success, "ROM A loaded");

    // Add breakpoint
    auto bp1 = f.api->setBreakpoint(0x0100);
    CHECK(bp1.success, "breakpoint 0x0100 added");
    CHECK_EQ(1, f.breakpointCount(), "1 breakpoint before failed load");

    // Attempt to load nonexistent ROM
    auto lr2 = f.api->loadRom("/tmp/nonexistent_rom_12345.rom", 0);
    CHECK(!lr2.success, "nonexistent ROM load fails");

    // Breakpoint must be preserved
    CHECK_EQ(1, f.breakpointCount(), "1 breakpoint preserved after failed load");
    CHECK(f.hasBreakpointAt(0x0100), "breakpoint at 0x0100 still present");

    cleanupTempRoms();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 3: Multiple breakpoints cleared
// ---------------------------------------------------------------------------

static void test_multiple_breakpoints_cleared()
{
    TEST_BEGIN("multiple breakpoints all cleared on ROM load");

    writeTempRom(TEMP_ROM_A, makeTestRom());
    writeTempRom(TEMP_ROM_B, makeTestRom());

    Fixture f;

    auto lr1 = f.api->loadRom(TEMP_ROM_A, 0);
    CHECK(lr1.success, "ROM A loaded");

    // Add 10 breakpoints
    uint16_t addrs[] = { 0x0000, 0x0100, 0x0200, 0x0300, 0x0400,
                         0x0500, 0x0600, 0x0700, 0x0800, 0x0900 };
    for (int i = 0; i < 10; ++i) {
        auto r = f.api->setBreakpoint(addrs[i]);
        CHECK(r.success, "breakpoint added");
    }
    CHECK_EQ(10, f.breakpointCount(), "10 breakpoints before ROM B");

    // Load ROM B
    auto lr2 = f.api->loadRom(TEMP_ROM_B, 0);
    CHECK(lr2.success, "ROM B loaded");
    CHECK_EQ(0, f.breakpointCount(), "0 breakpoints after ROM B");

    cleanupTempRoms();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 4: New breakpoint works after ROM load
// ---------------------------------------------------------------------------

static void test_new_breakpoint_works_after_rom_load()
{
    TEST_BEGIN("new breakpoint works after ROM load");

    writeTempRom(TEMP_ROM_A, makeTestRom());
    writeTempRom(TEMP_ROM_B, makeTestRom());

    Fixture f;

    // Load ROM A, add breakpoint
    f.api->loadRom(TEMP_ROM_A, 0);
    f.api->setBreakpoint(0x0100);
    CHECK_EQ(1, f.breakpointCount(), "1 breakpoint on ROM A");

    // Load ROM B
    f.api->loadRom(TEMP_ROM_B, 0);
    CHECK_EQ(0, f.breakpointCount(), "0 breakpoints after ROM B load");

    // Add new breakpoint — must work normally
    auto bp = f.api->setBreakpoint(0x0200);
    CHECK(bp.success, "new breakpoint 0x0200 added after ROM B load");
    CHECK_EQ(1, f.breakpointCount(), "1 breakpoint after adding new");
    CHECK(f.hasBreakpointAt(0x0200), "breakpoint at 0x0200 present");
    CHECK(!f.hasBreakpointAt(0x0100), "old breakpoint at 0x0100 not present");

    cleanupTempRoms();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 5: Repeated ROM loads
// ---------------------------------------------------------------------------

static void test_repeated_rom_loads()
{
    TEST_BEGIN("repeated ROM loads clear breakpoints each time");

    writeTempRom(TEMP_ROM_A, makeTestRom());
    writeTempRom(TEMP_ROM_B, makeTestRom());

    Fixture f;

    // Load ROM A, add breakpoint
    f.api->loadRom(TEMP_ROM_A, 0);
    f.api->setBreakpoint(0x0100);
    CHECK_EQ(1, f.breakpointCount(), "1 breakpoint on ROM A");

    // Load ROM B, add breakpoint
    f.api->loadRom(TEMP_ROM_B, 0);
    CHECK_EQ(0, f.breakpointCount(), "0 breakpoints after ROM B");
    f.api->setBreakpoint(0x0200);
    CHECK_EQ(1, f.breakpointCount(), "1 breakpoint on ROM B");

    // Load ROM A again
    f.api->loadRom(TEMP_ROM_A, 0);
    CHECK_EQ(0, f.breakpointCount(), "0 breakpoints after ROM A reload");

    cleanupTempRoms();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 6: Backend breakpoints cleared directly (no AgentApi layer)
// ---------------------------------------------------------------------------

static void test_backend_breakpoints_cleared_directly()
{
    TEST_BEGIN("backend breakpoints_ cleared on loadRom (direct check)");

    writeTempRom(TEMP_ROM_A, makeTestRom());
    writeTempRom(TEMP_ROM_B, makeTestRom());

    Fixture f;

    // Load ROM A via backend directly
    CHECK(f.backend->loadRom(TEMP_ROM_A, 0), "ROM A loaded via backend");

    // Add breakpoints via backend directly
    int id1 = f.backend->addBreakpoint(0x0100);
    int id2 = f.backend->addBreakpoint(0x0200);
    CHECK(id1 >= 0, "breakpoint 1 added");
    CHECK(id2 >= 0, "breakpoint 2 added");
    CHECK_EQ(2u, (unsigned)f.backend->getBreakpoints().size(), "2 breakpoints in backend");

    // Load ROM B via backend
    CHECK(f.backend->loadRom(TEMP_ROM_B, 0), "ROM B loaded via backend");

    // Backend breakpoints must be empty
    CHECK_EQ(0u, (unsigned)f.backend->getBreakpoints().size(), "0 breakpoints in backend after ROM B");

    cleanupTempRoms();
    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("\033[1;33m================================================\n");
    printf("  Stage 6.20.1: ROM Load Breakpoint Reset Tests\n");
    printf("================================================\033[0m\n");

    test_successful_rom_load_clears_breakpoints();
    test_failed_rom_load_preserves_breakpoints();
    test_multiple_breakpoints_cleared();
    test_new_breakpoint_works_after_rom_load();
    test_repeated_rom_loads();
    test_backend_breakpoints_cleared_directly();

    printf("\n\033[1;33m================================================\n");
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", \033[1;31m%d FAILED", tests_failed);
    }
    printf("\n================================================\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
