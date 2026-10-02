// Integration tests for MAP Loader — Stage 6.2
//
// Tests MAP auto-detection and loading through DebugBackend::loadRom().
// Uses real DebugBackend + NoBoardTarget (no SDL/Board dependency).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "memory.h"
#include "i8080.h"
#include "i8080_hal.h"
#include "no_board_target.h"
#include "backend.h"
#include "map_loader.h"
#include "symbol_database.h"

using namespace i8080cpu;

// ---------------------------------------------------------------------------
// Minimal HAL — connects CPU to a Memory instance.
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
            printf("  \033[41;97m FAIL \033[0m %s: expected %d, got %d (line %d)\n", \
                   msg, (int)_e, (int)_a, __LINE__); \
            _test_ok = false; \
        } else { \
            printf("  \033[46;30m ok \033[0m %s\n", msg); \
        } \
    } while(0)

#define CHECK_STR(expected, actual, msg) \
    do { \
        const char *_e = (expected); const char *_a = (actual); \
        if (strcmp(_e, _a) != 0) { \
            printf("  \033[41;97m FAIL \033[0m %s: expected \"%s\", got \"%s\" (line %d)\n", \
                   msg, _e, _a, __LINE__); \
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
// Helpers
// ---------------------------------------------------------------------------

static std::string write_temp_file(const std::string &name, const std::string &content)
{
    std::string path = "/tmp/" + name;
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), content.size());
    return path;
}

static void cleanup_temp_file(const std::string &path)
{
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Test 1: ROM + MAP auto-detection
// ---------------------------------------------------------------------------

static void test_rom_with_map_auto_detect()
{
    TEST_BEGIN("ROM + MAP auto-detection");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);

    // Create a minimal ROM (just a few bytes at 0x0100)
    std::string rom_content(256, '\x00');
    rom_content[0] = 0x31; // LXI SP
    rom_content[1] = 0x00;
    rom_content[2] = 0xC1;
    rom_content[3] = 0x76; // HLT

    std::string rom_path = write_temp_file("test_auto.rom", rom_content);

    // Create a MAP file alongside
    std::string map_content =
        "_main = $0100 ; addr, public, main.c::main::0::1:10\n"
        "_loop = $0103 ; addr, local\n";
    std::string map_path = write_temp_file("test_auto.map", map_content);

    // Load ROM — should auto-detect MAP
    bool ok = backend.loadRom(rom_path, 0);
    CHECK(ok, "loadRom succeeded");

    // Verify MAP symbols in SymbolDatabase
    auto &db = backend.symbolDatabase();
    CHECK_EQ(2u, (unsigned)db.symbolCount(), "2 MAP symbols loaded");

    const DebugSymbol *mainSym = db.findSymbol(0x0100);
    CHECK(mainSym != nullptr, "_main found at 0x0100");
    if (mainSym) {
        CHECK_STR("_main", mainSym->name.c_str(), "name = _main");
        CHECK(mainSym->type == SymbolType::Function, "type = Function (addr+public)");
        CHECK(mainSym->fromMap, "fromMap = true");
        CHECK_STR("main.c", mainSym->sourceFile.c_str(), "sourceFile = main.c");
        CHECK_EQ(10, mainSym->sourceLine, "sourceLine = 10");
    }

    const DebugSymbol *loopSym = db.findSymbol(0x0103);
    CHECK(loopSym != nullptr, "_loop found at 0x0103");
    if (loopSym) {
        CHECK_STR("_loop", loopSym->name.c_str(), "name = _loop");
        CHECK(loopSym->type == SymbolType::Label, "type = Label (local)");
        CHECK(loopSym->fromMap, "fromMap = true");
    }

    cleanup_temp_file(rom_path);
    cleanup_temp_file(map_path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 2: ROM without MAP — no error
// ---------------------------------------------------------------------------

static void test_rom_without_map()
{
    TEST_BEGIN("ROM without MAP — no error, no symbols");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);

    std::string rom_content(4, '\x00');
    rom_content[0] = 0x76; // HLT
    std::string rom_path = write_temp_file("test_no_map.rom", rom_content);

    // No .map file exists alongside
    bool ok = backend.loadRom(rom_path, 0);
    CHECK(ok, "loadRom succeeded");
    CHECK_EQ(0u, (unsigned)backend.symbolDatabase().symbolCount(), "0 symbols");

    cleanup_temp_file(rom_path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 3: ROM reload clears old MAP symbols
// ---------------------------------------------------------------------------

static void test_rom_reload_clears_map()
{
    TEST_BEGIN("ROM reload clears old MAP symbols");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);

    // First ROM + MAP
    std::string rom1(4, '\x00');
    rom1[0] = 0x76;
    std::string rom1_path = write_temp_file("test_reload_a.rom", rom1);
    std::string map1_content = "_func_a = $0100 ; addr, public\n";
    std::string map1_path = write_temp_file("test_reload_a.map", map1_content);

    backend.loadRom(rom1_path, 0);
    CHECK_EQ(1u, (unsigned)backend.symbolDatabase().symbolCount(), "1 MAP symbol from A");

    // Second ROM + MAP (replaces first)
    std::string rom2(4, '\x00');
    rom2[0] = 0x76;
    std::string rom2_path = write_temp_file("test_reload_b.rom", rom2);
    std::string map2_content =
        "_func_b1 = $0200 ; addr, public\n"
        "_func_b2 = $0300 ; addr, public\n";
    std::string map2_path = write_temp_file("test_reload_b.map", map2_content);

    backend.loadRom(rom2_path, 0);

    // Old MAP symbols should be gone, new ones present
    auto &db = backend.symbolDatabase();
    CHECK_EQ(2u, (unsigned)db.symbolCount(), "2 MAP symbols from B");
    CHECK(db.findSymbol(0x0100) == nullptr, "A's symbol gone");
    CHECK(db.findSymbol(0x0200) != nullptr, "B's symbol present");
    CHECK(db.findSymbol(0x0300) != nullptr, "B's symbol present");

    cleanup_temp_file(rom1_path);
    cleanup_temp_file(map1_path);
    cleanup_temp_file(rom2_path);
    cleanup_temp_file(map2_path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 4: ROM with MAP, then ROM without MAP — old symbols cleared
// ---------------------------------------------------------------------------

static void test_rom_with_map_then_without()
{
    TEST_BEGIN("ROM+MAP → ROM without MAP — old symbols cleared");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);

    // First: ROM + MAP
    std::string rom1(4, '\x00');
    rom1[0] = 0x76;
    std::string rom1_path = write_temp_file("test_map_then.rom", rom1);
    std::string map1_content = "_old_func = $0100 ; addr, public\n";
    std::string map1_path = write_temp_file("test_map_then.map", map1_content);

    backend.loadRom(rom1_path, 0);
    CHECK_EQ(1u, (unsigned)backend.symbolDatabase().symbolCount(), "1 MAP symbol");

    // Second: ROM without MAP
    std::string rom2(4, '\x00');
    rom2[0] = 0x76;
    std::string rom2_path = write_temp_file("test_no_map_then.rom", rom2);
    // No .map file for rom2

    backend.loadRom(rom2_path, 0);
    CHECK_EQ(0u, (unsigned)backend.symbolDatabase().symbolCount(), "0 symbols after reload");

    cleanup_temp_file(rom1_path);
    cleanup_temp_file(map1_path);
    cleanup_temp_file(rom2_path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 5: Old keyboard .map file — format validation, no crash
// ---------------------------------------------------------------------------

static void test_old_keyboard_map_no_crash()
{
    TEST_BEGIN("Old keyboard .map file — format validation, no crash");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);

    std::string rom_content(4, '\x00');
    rom_content[0] = 0x76;
    std::string rom_path = write_temp_file("test_oldkbd.rom", rom_content);

    // Create old-style keyboard mapping .map file
    std::string map_content =
        "[keyboard]\n"
        "key_a = SDL_SCANCODE_A\n"
        "key_enter = SDL_SCANCODE_RETURN\n";
    std::string map_path = write_temp_file("test_oldkbd.map", map_content);

    // Should not crash, should not load any symbols
    bool ok = backend.loadRom(rom_path, 0);
    CHECK(ok, "loadRom succeeded");
    CHECK_EQ(0u, (unsigned)backend.symbolDatabase().symbolCount(), "0 symbols (old format rejected)");

    cleanup_temp_file(rom_path);
    cleanup_temp_file(map_path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 6: User symbols preserved across ROM reload (MAP cleared, user kept)
// ---------------------------------------------------------------------------

static void test_user_symbols_preserved()
{
    TEST_BEGIN("User symbols preserved across ROM reload, MAP symbols cleared");

    Memory mem;
    test_memory = &mem;
    NoBoardTarget target(mem);
    DebugBackend backend(target);

    // Load ROM + MAP
    std::string rom1(4, '\x00');
    rom1[0] = 0x76;
    std::string rom1_path = write_temp_file("test_user_a.rom", rom1);
    std::string map1_content = "_map_func = $0200 ; addr, public\n";
    std::string map1_path = write_temp_file("test_user_a.map", map1_content);

    backend.loadRom(rom1_path, 0);
    CHECK_EQ(1u, (unsigned)backend.symbolDatabase().symbolCount(), "1 MAP symbol");

    // Add user symbol
    backend.symbolDatabase().addSymbol(0x0100, "user_func", SymbolType::Function);
    CHECK_EQ(2u, (unsigned)backend.symbolDatabase().symbolCount(), "2 symbols total");

    // Load another ROM + MAP
    std::string rom2(4, '\x00');
    rom2[0] = 0x76;
    std::string rom2_path = write_temp_file("test_user_b.rom", rom2);
    std::string map2_content = "_new_map = $0300 ; addr, public\n";
    std::string map2_path = write_temp_file("test_user_b.map", map2_content);

    backend.loadRom(rom2_path, 0);

    auto &db = backend.symbolDatabase();
    // User symbol preserved, old MAP symbol cleared, new MAP symbol added
    CHECK(db.findSymbol(0x0100) != nullptr, "user_func preserved");
    CHECK(db.findSymbol(0x0200) == nullptr, "old MAP symbol cleared");
    CHECK(db.findSymbol(0x0300) != nullptr, "new MAP symbol loaded");
    CHECK_EQ(2u, (unsigned)db.symbolCount(), "2 symbols total");

    cleanup_temp_file(rom1_path);
    cleanup_temp_file(map1_path);
    cleanup_temp_file(rom2_path);
    cleanup_temp_file(map2_path);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  MAP Loader Integration Tests (Stage 6.2)\033[0m\n");
    printf("\033[1;33m========================================\033[0m\n");

    test_rom_with_map_auto_detect();
    test_rom_without_map();
    test_rom_reload_clears_map();
    test_rom_with_map_then_without();
    test_old_keyboard_map_no_crash();
    test_user_symbols_preserved();

    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  Results: %d passed, %d failed (of %d)\033[0m\n",
           tests_passed, tests_failed, tests_run);
    printf("\033[1;33m========================================\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
