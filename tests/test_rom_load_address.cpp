#include "rom_load_address.h"

#include <cassert>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Unit tests for getRomLoadAddress()
// ---------------------------------------------------------------------------

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(addr, expected, label) do { \
    if ((addr) == (expected)) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        printf("FAIL: %s: got %04x, expected %04x\n", label, (unsigned)(addr), (unsigned)(expected)); \
    } \
} while(0)

static void test_rom_extension()
{
    CHECK(getRomLoadAddress("test.rom"),    0x0100, "test.rom");
    CHECK(getRomLoadAddress("game.rom"),    0x0100, "game.rom");
    CHECK(getRomLoadAddress("/path/to/file.rom"), 0x0100, "/path/to/file.rom");
}

static void test_r0m_through_r9m()
{
    CHECK(getRomLoadAddress("test.r0m"),    0x0000, "test.r0m");
    CHECK(getRomLoadAddress("test.r1m"),    0x0100, "test.r1m");
    CHECK(getRomLoadAddress("test.r2m"),    0x0200, "test.r2m");
    CHECK(getRomLoadAddress("test.r3m"),    0x0300, "test.r3m");
    CHECK(getRomLoadAddress("test.r4m"),    0x0400, "test.r4m");
    CHECK(getRomLoadAddress("test.r5m"),    0x0500, "test.r5m");
    CHECK(getRomLoadAddress("test.r6m"),    0x0600, "test.r6m");
    CHECK(getRomLoadAddress("test.r7m"),    0x0700, "test.r7m");
    CHECK(getRomLoadAddress("test.r8m"),    0x0800, "test.r8m");
    CHECK(getRomLoadAddress("test.r9m"),    0x0900, "test.r9m");
}

static void test_case_insensitive()
{
    CHECK(getRomLoadAddress("TEST.ROM"),    0x0100, "TEST.ROM");
    CHECK(getRomLoadAddress("TEST.R3M"),    0x0300, "TEST.R3M");
    CHECK(getRomLoadAddress("Test.R7m"),    0x0700, "Test.R7m");
    CHECK(getRomLoadAddress("game.Rom"),    0x0100, "game.Rom");
    CHECK(getRomLoadAddress("game.R0M"),    0x0000, "game.R0M");
    CHECK(getRomLoadAddress("game.r1M"),    0x0100, "game.r1M");
}

static void test_rom_vs_r0m_distinct()
{
    // .rom and .r0m must give DIFFERENT addresses
    uint16_t rom_addr = getRomLoadAddress("test.rom");
    uint16_t r0m_addr = getRomLoadAddress("test.r0m");
    assert(rom_addr != r0m_addr);
    CHECK(rom_addr, 0x0100, ".rom != .r0m (rom)");
    CHECK(r0m_addr, 0x0000, ".rom != .r0m (r0m)");
}

static void test_unknown_extension()
{
    CHECK(getRomLoadAddress("test.com"),    0x0000, "test.com (unknown)");
    CHECK(getRomLoadAddress("test.txt"),    0x0000, "test.txt (unknown)");
    CHECK(getRomLoadAddress("test.bin"),    0x0000, "test.bin (unknown)");
}

static void test_no_extension()
{
    CHECK(getRomLoadAddress("test"),        0x0000, "test (no ext)");
    CHECK(getRomLoadAddress("/path/file"),  0x0000, "/path/file (no ext)");
}

static void test_path_with_directories()
{
    CHECK(getRomLoadAddress("/home/user/roms/game.r3m"), 0x0300, "path/game.r3m");
    CHECK(getRomLoadAddress("C:\\roms\\game.ROM"),       0x0100, "C:\\roms\\game.ROM");
}

int main()
{
    printf("=== getRomLoadAddress unit tests ===\n\n");

    test_rom_extension();
    test_r0m_through_r9m();
    test_case_insensitive();
    test_rom_vs_r0m_distinct();
    test_unknown_extension();
    test_no_extension();
    test_path_with_directories();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
