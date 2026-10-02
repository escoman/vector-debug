// Integration tests for correct ROM loading and Reset behavior.
//
// Verifies:
//   1. ROM load address determined by extension (.rom → 0x0100, .rNm → N*0x100)
//   2. PC = 0x0000 after loading any ROM
//   3. Reset preserves ROM data in RAM
//   4. Reset sets PC = 0x0000
//   5. Execution after Reset starts from PC=0000
//
// Uses NoBoardTarget (no SDL/Board dependency).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "memory.h"
#include "i8080.h"
#include "i8080_hal.h"
#include "no_board_target.h"
#include "rom_load_address.h"
#include "backend.h"

using namespace i8080cpu;

// ---------------------------------------------------------------------------
// Minimal test HAL — connects CPU to a Memory instance.
// ---------------------------------------------------------------------------

static Memory *test_memory = nullptr;

int i8080_hal_memory_read_byte(int addr)
{
    return test_memory->read(addr, false);
}

void i8080_hal_memory_write_byte(int addr, int value)
{
    test_memory->write(addr, value, false);
}

int i8080_hal_memory_read_word(int addr, bool stack)
{
    return test_memory->read(addr, stack)
         | (test_memory->read(addr + 1, stack) << 8);
}

void i8080_hal_memory_write_word(int addr, int word, bool stack)
{
    test_memory->write(addr, word & 0xff, stack);
    test_memory->write(addr + 1, word >> 8, stack);
}

int i8080_hal_io_input(int) { return 0xff; }
void i8080_hal_io_output(int, int) {}
void i8080_hal_iff(int) {}

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
        bool _test_ok = true;

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
// Helpers
// ---------------------------------------------------------------------------

// Write a small binary file for testing
static std::string write_temp_rom(const std::string &name,
                                  const std::vector<uint8_t> &data)
{
    std::string path = "/tmp/v06c_test_" + name;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    f.close();
    return path;
}

static void cleanup_temp_file(const std::string &path)
{
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test: .rom loads at 0x0100, PC = 0
// ---------------------------------------------------------------------------

static void test_rom_load_address_and_pc()
{
    TEST_BEGIN(".rom loads at 0x0100, PC=0");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);

    // 3 bytes: AA BB CC
    std::vector<uint8_t> rom_data = { 0xAA, 0xBB, 0xCC };
    std::string path = write_temp_rom("test.rom", rom_data);

    bool ok = target.loadRom(path, 0);  // org=0 → auto-detect
    CHECK(ok, "loadRom succeeded");

    // Verify data at 0x0100
    CHECK_EQ(0xAA, mem.peek(0x0100), "RAM[0100] = AA");
    CHECK_EQ(0xBB, mem.peek(0x0101), "RAM[0101] = BB");
    CHECK_EQ(0xCC, mem.peek(0x0102), "RAM[0102] = CC");

    // Verify PC = 0
    CHECK_EQ(0x0000, i8080_pc(), "PC = 0000");

    cleanup_temp_file(path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test: .r3m loads at 0x0300, PC = 0
// ---------------------------------------------------------------------------

static void test_r3m_load_address_and_pc()
{
    TEST_BEGIN(".r3m loads at 0x0300, PC=0");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);

    std::vector<uint8_t> rom_data = { 0xAA, 0xBB, 0xCC };
    std::string path = write_temp_rom("test.r3m", rom_data);

    bool ok = target.loadRom(path, 0);  // auto-detect
    CHECK(ok, "loadRom succeeded");

    CHECK_EQ(0xAA, mem.peek(0x0300), "RAM[0300] = AA");
    CHECK_EQ(0xBB, mem.peek(0x0301), "RAM[0301] = BB");
    CHECK_EQ(0xCC, mem.peek(0x0302), "RAM[0302] = CC");
    CHECK_EQ(0x0000, i8080_pc(), "PC = 0000");

    cleanup_temp_file(path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test: .r0m loads at 0x0000, PC = 0
// ---------------------------------------------------------------------------

static void test_r0m_load_address_and_pc()
{
    TEST_BEGIN(".r0m loads at 0x0000, PC=0");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);

    std::vector<uint8_t> rom_data = { 0xAA, 0xBB, 0xCC };
    std::string path = write_temp_rom("test.r0m", rom_data);

    bool ok = target.loadRom(path, 0);  // auto-detect
    CHECK(ok, "loadRom succeeded");

    CHECK_EQ(0xAA, mem.peek(0x0000), "RAM[0000] = AA");
    CHECK_EQ(0xBB, mem.peek(0x0001), "RAM[0001] = BB");
    CHECK_EQ(0xCC, mem.peek(0x0002), "RAM[0002] = CC");
    CHECK_EQ(0x0000, i8080_pc(), "PC = 0000");

    cleanup_temp_file(path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test: Reset preserves ROM data and sets PC = 0
// ---------------------------------------------------------------------------

static void test_reset_preserves_rom()
{
    TEST_BEGIN("Reset preserves ROM, PC=0");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);

    // Load .rom at 0x0100
    std::vector<uint8_t> rom_data = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
    std::string path = write_temp_rom("reset_test.rom", rom_data);

    bool ok = target.loadRom(path, 0);
    CHECK(ok, "loadRom succeeded");

    // Verify ROM is at 0x0100
    CHECK_EQ(0xAA, mem.peek(0x0100), "RAM[0100] = AA before reset");
    CHECK_EQ(0xEE, mem.peek(0x0104), "RAM[0104] = EE before reset");

    // Set PC to arbitrary value
    i8080_jump(0x1234);
    CHECK_EQ(0x1234, i8080_pc(), "PC set to 1234");

    // Perform Reset (BLK+СБР)
    target.reset(false);

    // Verify PC = 0
    CHECK_EQ(0x0000, i8080_pc(), "PC = 0000 after reset");

    // Verify ROM data still in RAM
    CHECK_EQ(0xAA, mem.peek(0x0100), "RAM[0100] = AA after reset");
    CHECK_EQ(0xBB, mem.peek(0x0101), "RAM[0101] = BB after reset");
    CHECK_EQ(0xCC, mem.peek(0x0102), "RAM[0102] = CC after reset");
    CHECK_EQ(0xDD, mem.peek(0x0103), "RAM[0103] = DD after reset");
    CHECK_EQ(0xEE, mem.peek(0x0104), "RAM[0104] = EE after reset");

    cleanup_temp_file(path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test: Execution after Reset starts from PC=0
//
// Minimal ROM:
//   0000: JMP 0100        (C3 00 01)
//   0100: MVI A, 42       (3E 42)
//   0102: HLT             (76)
//
// After Load ROM + step, PC should go 0000 → 0003 → ... → 0100
// ---------------------------------------------------------------------------

static void test_execution_from_pc0()
{
    TEST_BEGIN("Execution starts from PC=0000 after load");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);

    // Build a ROM that:
    // 1. Has JMP 0100 at address 0000
    // 2. Has MVI A,42 at address 0100
    // We load as .rom (at 0x0100), so the JMP must be at 0x0000
    // and the program at 0x0100.

    // First, write JMP instruction directly at 0x0000
    mem.write(0x0000, 0xC3, false);  // JMP
    mem.write(0x0001, 0x00, false);  // low byte of 0x0100
    mem.write(0x0002, 0x01, false);  // high byte of 0x0100

    // Now load a small ROM at 0x0100
    std::vector<uint8_t> rom_data = {
        0x3E, 0x42,  // 0100: MVI A, 42
        0x76         // 0102: HLT
    };
    std::string path = write_temp_rom("exec_test.rom", rom_data);

    bool ok = target.loadRom(path, 0);  // auto-detect .rom → 0x0100
    CHECK(ok, "loadRom succeeded");

    // After load, PC should be 0
    CHECK_EQ(0x0000, i8080_pc(), "PC = 0000 after load");

    // The JMP at 0000 was overwritten by init_from_vector (which clears RAM
    // below start_addr). We need to re-write it since .rom loads at 0x0100
    // and init_from_vector clears 0x0000-0xFFFF when start_addr < 65536.
    // Actually, init_from_vector clears the main RAM when start_addr < 65536.
    // So we need to put the JMP back AFTER loading.
    mem.write(0x0000, 0xC3, false);  // JMP
    mem.write(0x0001, 0x00, false);  // low
    mem.write(0x0002, 0x01, false);  // high

    // Verify ROM at 0x0100
    CHECK_EQ(0x3E, mem.peek(0x0100), "RAM[0100] = 3E (MVI A)");
    CHECK_EQ(0x42, mem.peek(0x0101), "RAM[0101] = 42");
    CHECK_EQ(0x76, mem.peek(0x0102), "RAM[0102] = 76 (HLT)");

    // Execute: PC=0000 → JMP 0100 → PC=0100
    target.stepInstruction();  // Execute JMP at 0000
    CHECK_EQ(0x0100, i8080_pc(), "PC = 0100 after JMP");

    // Execute: MVI A, 42
    target.stepInstruction();
    CHECK_EQ(0x0102, i8080_pc(), "PC = 0102 after MVI A");
    CHECK_EQ(0x42, i8080_regs_a(), "A = 42");

    cleanup_temp_file(path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test: Multiple formats produce correct load addresses
// ---------------------------------------------------------------------------

static void test_all_formats()
{
    TEST_BEGIN("All .r0m-.r9m and .rom formats");

    struct TestCase {
        const char *ext;
        uint16_t expected_addr;
    };

    TestCase cases[] = {
        { ".rom", 0x0100 },
        { ".r0m", 0x0000 },
        { ".r1m", 0x0100 },
        { ".r2m", 0x0200 },
        { ".r3m", 0x0300 },
        { ".r4m", 0x0400 },
        { ".r5m", 0x0500 },
        { ".r6m", 0x0600 },
        { ".r7m", 0x0700 },
        { ".r8m", 0x0800 },
        { ".r9m", 0x0900 },
    };

    for (auto &tc : cases) {
        Memory mem;
        test_memory = &mem;
        NoBoardTarget target(mem);

        std::vector<uint8_t> rom_data = { 0xAA, 0xBB };
        std::string name = std::string("test") + tc.ext;
        std::string path = write_temp_rom(name, rom_data);

        bool ok = target.loadRom(path, 0);
        CHECK(ok, name.c_str());

        CHECK_EQ(0xAA, mem.peek(tc.expected_addr), 
                 (std::string("data at ") + std::to_string(tc.expected_addr)).c_str());
        CHECK_EQ(0x0000, i8080_pc(), "PC = 0000");

        cleanup_temp_file(path);
    }

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test: Explicit org overrides auto-detection
// ---------------------------------------------------------------------------

static void test_explicit_org_override()
{
    TEST_BEGIN("Explicit org overrides auto-detect");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);

    // .rom would normally load at 0x0100, but we pass explicit org=0x0500
    std::vector<uint8_t> rom_data = { 0xAA, 0xBB };
    std::string path = write_temp_rom("override.rom", rom_data);

    bool ok = target.loadRom(path, 0x0500);
    CHECK(ok, "loadRom succeeded");

    CHECK_EQ(0xAA, mem.peek(0x0500), "RAM[0500] = AA (explicit org)");
    CHECK_EQ(0x0000, i8080_pc(), "PC = 0000");

    cleanup_temp_file(path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test: DebugBackend integration — loadRom sets state to Paused
// ---------------------------------------------------------------------------

static void test_backend_loadrom_paused()
{
    TEST_BEGIN("DebugBackend: loadRom → Paused state");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);

    std::vector<uint8_t> rom_data = { 0xC3, 0x00, 0x00 };  // JMP 0000
    std::string path = write_temp_rom("backend_test.rom", rom_data);

    bool ok = backend.loadRom(path, 0);
    CHECK(ok, "loadRom succeeded");

    CHECK(backend.isPaused(), "state = Paused after loadRom");
    CHECK_EQ(0x0000, target.getCpuState().pc, "PC = 0000");
    CHECK_EQ(0xC3, mem.peek(0x0100), "RAM[0100] = C3 (.rom at 0x0100)");

    cleanup_temp_file(path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test: DebugBackend reset → Paused, PC=0, ROM preserved
// ---------------------------------------------------------------------------

static void test_backend_reset_preserves_rom()
{
    TEST_BEGIN("DebugBackend: reset → Paused, PC=0, ROM preserved");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);

    std::vector<uint8_t> rom_data = { 0xAA, 0xBB, 0xCC };
    std::string path = write_temp_rom("reset_be_test.rom", rom_data);

    bool ok = backend.loadRom(path, 0);
    CHECK(ok, "loadRom succeeded");

    // Verify ROM loaded
    CHECK_EQ(0xAA, mem.peek(0x0100), "RAM[0100] = AA");

    // Set PC to arbitrary value
    i8080_jump(0x5678);

    // Reset
    backend.reset();

    CHECK(backend.isPaused(), "state = Paused after reset");
    CHECK_EQ(0x0000, target.getCpuState().pc, "PC = 0000 after reset");
    CHECK_EQ(0xAA, mem.peek(0x0100), "RAM[0100] = AA preserved");
    CHECK_EQ(0xBB, mem.peek(0x0101), "RAM[0101] = BB preserved");
    CHECK_EQ(0xCC, mem.peek(0x0102), "RAM[0102] = CC preserved");

    cleanup_temp_file(path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test: replacing the machine belongs to the emulation thread. Loading a ROM
// while frames of the previous one still run used to zero 64 KiB under the
// CPU's feet — the pause flag is only sampled once per frame.
// ---------------------------------------------------------------------------

static void test_backend_loadrom_runs_on_emulation_thread()
{
    TEST_BEGIN("DebugBackend: loadRom executes on the emulation thread");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);

    std::vector<uint8_t> rom_data = { 0xC9 };   // RET
    std::string path = write_temp_rom("thread_test.rom", rom_data);

    // No emulation thread yet (the gui/main.cpp command-line ROM case): the
    // load runs on the calling thread, there is nobody to hand it to.
    CHECK(backend.loadRom(path, 0), "loadRom without an emulation loop succeeded");
    CHECK(target.loadRomThread() == std::this_thread::get_id(),
          "no loop → machine replaced by the calling thread");

    std::thread emu([&]() { backend.runUntilPause(); });
    for (int i = 0; i < 200 && !backend.isEmulationLoopRunning(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(backend.isEmulationLoopRunning(), "emulation loop is up");

    CHECK(backend.loadRom(path, 0), "loadRom with an emulation loop succeeded");
    CHECK(target.loadRomThread() == emu.get_id(),
          "machine replaced on the emulation thread");
    CHECK(target.loadRomThread() != std::this_thread::get_id(),
          "not on the thread that asked for the load");
    CHECK(backend.isPaused(), "load left the machine paused");

    backend.requestQuit();
    emu.join();
    cleanup_temp_file(path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("=== ROM Loading and Reset Integration Tests ===\n");

    test_rom_load_address_and_pc();
    test_r3m_load_address_and_pc();
    test_r0m_load_address_and_pc();
    test_reset_preserves_rom();
    test_execution_from_pc0();
    test_all_formats();
    test_explicit_org_override();
    test_backend_loadrom_paused();
    test_backend_reset_preserves_rom();
    test_backend_loadrom_runs_on_emulation_thread();

    printf("\n=== Results: %d passed, %d failed (of %d) ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
