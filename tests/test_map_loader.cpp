// Unit tests for Z88DK MAP Loader — Stage 6.2
//
// Tests for MapLoader: format validation, symbol parsing, source location,
// hex boundaries, malformed lines, duplicates, and old keyboard .map rejection.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#include "map_loader.h"
#include "symbol_database.h"

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

#define CHECK_EQ(expected, actual, msg) \
    do { \
        auto _e = (expected); auto _a = (actual); \
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
// Test 1: Public function
// ---------------------------------------------------------------------------

static void test_public_function()
{
    TEST_BEGIN("public function");

    std::string content =
        "_main                    = $0252 ; addr, public, main.c::main::0::1:109\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");
    CHECK_EQ(1, result.parsedCount, "1 symbol parsed");
    CHECK_EQ(0, result.skippedLines, "0 lines skipped");

    CHECK_EQ(1, (int)result.symbols.size(), "1 symbol in vector");
    const auto &sym = result.symbols[0];
    CHECK_STR("_main", sym.name.c_str(), "name = _main");
    CHECK_EQ(0x0252, sym.address, "address = 0x0252");
    CHECK(sym.visibility == MapSymbolVisibility::Public, "visibility = public");
    CHECK(sym.isAddress, "isAddress = true");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 2: Local label
// ---------------------------------------------------------------------------

static void test_local_label()
{
    TEST_BEGIN("local label");

    std::string content =
        "fp_c000_inner            = $01D1 ; addr, local\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");
    CHECK_EQ(1, result.parsedCount, "1 symbol parsed");

    const auto &sym = result.symbols[0];
    CHECK_STR("fp_c000_inner", sym.name.c_str(), "name");
    CHECK_EQ(0x01D1, sym.address, "address = 0x01D1");
    CHECK(sym.visibility == MapSymbolVisibility::Local, "visibility = local");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 3: Source location
// ---------------------------------------------------------------------------

static void test_source_location()
{
    TEST_BEGIN("source location extraction");

    std::string content =
        "_main = $0252 ; addr, public, main.c::main::0::1:109\n"
        "_gfx_set_mode = $02B3 ; addr, public, ../../lib/gfx/mode.c::gfx_set_mode::0::0:39\n"
        "_clr = $0100 ; addr, public, clr.asm:161\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");
    CHECK_EQ(3, (int)result.symbols.size(), "3 symbols parsed");

    // Test 1: main.c::main::0::1:109 → file=main.c, line=109
    CHECK_STR("main.c", result.symbols[0].sourceFile.c_str(), "source file = main.c");
    CHECK_EQ(109, result.symbols[0].sourceLine, "source line = 109");

    // Test 2: ../../lib/gfx/mode.c::gfx_set_mode::0::0:39 → file=../../lib/gfx/mode.c, line=39
    CHECK_STR("../../lib/gfx/mode.c", result.symbols[1].sourceFile.c_str(),
              "source file = ../../lib/gfx/mode.c");
    CHECK_EQ(39, result.symbols[1].sourceLine, "source line = 39");

    // Test 3: clr.asm:161 → file=clr.asm, line=161
    CHECK_STR("clr.asm", result.symbols[2].sourceFile.c_str(), "source file = clr.asm");
    CHECK_EQ(161, result.symbols[2].sourceLine, "source line = 161");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 4: Hexadecimal boundaries
// ---------------------------------------------------------------------------

static void test_hex_boundaries()
{
    TEST_BEGIN("hexadecimal boundaries $0000 and $FFFF");

    std::string content =
        "_start = $0000 ; addr, public\n"
        "_end   = $FFFF ; addr, public\n"
        "_mid   = $C000 ; addr, local\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");
    CHECK_EQ(3, (int)result.symbols.size(), "3 symbols parsed");

    CHECK_EQ(0x0000, result.symbols[0].address, "$0000");
    CHECK_EQ(0xFFFF, result.symbols[1].address, "$FFFF");
    CHECK_EQ(0xC000, result.symbols[2].address, "$C000");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 5: Invalid address (> 16-bit)
// ---------------------------------------------------------------------------

static void test_invalid_address()
{
    TEST_BEGIN("invalid address > $FFFF rejected");

    // $10000 is 17-bit — should be rejected
    std::string content =
        "_huge = $10000 ; addr, public\n"
        "_ok   = $0100 ; addr, public\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded (file is valid Z88DK)");
    // _huge should be skipped, _ok should be parsed
    CHECK_EQ(1, result.parsedCount, "1 symbol parsed");
    CHECK(result.skippedLines >= 1, "at least 1 line skipped");
    CHECK_EQ(0x0100, result.symbols[0].address, "only _ok at $0100");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 6: Malformed lines — parser must not crash
// ---------------------------------------------------------------------------

static void test_malformed_lines()
{
    TEST_BEGIN("malformed lines — no crash, skip gracefully");

    std::string content =
        "\n"                                        // empty line
        "; this is a comment\n"                     // comment
        "random garbage line\n"                     // no = $
        "= $0100 ; no name\n"                       // empty name
        "_valid = $0200 ; addr, public\n"           // valid
        "_noaddr = $0300\n"                         // valid, no comment
        "incomplete = \n"                           // no address
        "another_bad = $GGGG\n"                     // invalid hex
        ;

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded (valid Z88DK format overall)");
    CHECK(result.parsedCount >= 2, "at least 2 valid symbols parsed");
    CHECK(result.skippedLines >= 3, "at least 3 lines skipped");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 7: Duplicate symbols — first wins
// ---------------------------------------------------------------------------

static void test_duplicate_symbols()
{
    TEST_BEGIN("duplicate symbols — parser returns both, DB handles conflict");

    std::string content =
        "_main = $0252 ; addr, public\n"
        "_main = $0252 ; addr, public\n"    // exact duplicate
        "_alt  = $0252 ; addr, local\n";    // different name, same address

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");
    CHECK_EQ(3, result.parsedCount, "parser returns all 3");

    // When loaded into SymbolDatabase, first wins
    SymbolDatabase db;
    int added = 0;
    for (const auto &ms : result.symbols) {
        SymbolType type = (ms.isAddress && ms.visibility == MapSymbolVisibility::Public)
            ? SymbolType::Function : SymbolType::Label;
        if (db.addSymbol(ms.address, ms.name, type)) {
            added++;
        }
    }
    CHECK_EQ(1, added, "only 1 symbol added to DB (first wins)");
    CHECK_STR("_main", db.findSymbol(0x0252)->name.c_str(), "first symbol name wins");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 8: Old keyboard .map file — format validation rejects it
// ---------------------------------------------------------------------------

static void test_old_keyboard_map_rejected()
{
    TEST_BEGIN("old keyboard .map file rejected by format validation");

    // Simulate an old keyboard mapping config file
    std::string content =
        "[keyboard]\n"
        "key_a = SDL_SCANCODE_A\n"
        "key_b = SDL_SCANCODE_B\n"
        "key_enter = SDL_SCANCODE_RETURN\n"
        "\n"
        "[mapping]\n"
        "up = W\n"
        "down = S\n"
        "left = A\n"
        "right = D\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(!result.success, "parse FAILED (as expected)");
    CHECK(!result.errorMessage.empty(), "error message present");
    CHECK_EQ(0, result.parsedCount, "0 symbols parsed");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 9: Empty file
// ---------------------------------------------------------------------------

static void test_empty_file()
{
    TEST_BEGIN("empty file");

    auto result = MapLoader::parseMapContent("");
    CHECK(!result.success, "parse FAILED (empty content)");
    CHECK(!result.errorMessage.empty(), "error message present");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 10: File with only comments and blank lines
// ---------------------------------------------------------------------------

static void test_comments_only()
{
    TEST_BEGIN("comments-only file — no = $ pattern");

    std::string content =
        "; Z88DK MAP file\n"
        "; Generated by zcc\n"
        "\n"
        "; no symbols here\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(!result.success, "parse FAILED (no symbol lines)");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 11: mapPathFromRom
// ---------------------------------------------------------------------------

static void test_map_path_from_rom()
{
    TEST_BEGIN("mapPathFromRom");

    std::string p1 = MapLoader::mapPathFromRom("/path/check_bugs.rom");
    CHECK_STR("/path/check_bugs.map", p1.c_str(), ".rom → .map");

    std::string p2 = MapLoader::mapPathFromRom("/path/game.ROM");
    CHECK_STR("/path/game.map", p2.c_str(), ".ROM → .map");

    std::string p3 = MapLoader::mapPathFromRom("test.r3m");
    CHECK_STR("test.map", p3.c_str(), ".r3m → .map");

    std::string p4 = MapLoader::mapPathFromRom("noext");
    CHECK_STR("noext.map", p4.c_str(), "no ext → +.map");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 12: loadMapFile — nonexistent file
// ---------------------------------------------------------------------------

static void test_load_nonexistent()
{
    TEST_BEGIN("loadMapFile — nonexistent file");

    auto result = MapLoader::loadMapFile("/nonexistent/path.map");
    CHECK(!result.success, "load failed");
    CHECK(!result.errorMessage.empty(), "error message present");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 13: loadMapFile — real file on disk
// ---------------------------------------------------------------------------

static void test_load_from_disk()
{
    TEST_BEGIN("loadMapFile — write and read real file");

    std::string path = "/tmp/test_z88dk.map";
    {
        std::ofstream f(path);
        f << "_main = $0252 ; addr, public, main.c::main::0::1:109\n";
        f << "_loop = $0260 ; addr, local\n";
    }

    auto result = MapLoader::loadMapFile(path);
    CHECK(result.success, "load succeeded");
    CHECK_EQ(2, result.parsedCount, "2 symbols parsed");
    CHECK_EQ(0x0252, result.symbols[0].address, "first addr");
    CHECK_EQ(0x0260, result.symbols[1].address, "second addr");

    // Cleanup
    std::remove(path.c_str());

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 14: loadMapFile — old keyboard file on disk (format validation)
// ---------------------------------------------------------------------------

static void test_load_old_keyboard_from_disk()
{
    TEST_BEGIN("loadMapFile — old keyboard .map rejected from disk");

    std::string path = "/tmp/test_old_keymap.map";
    {
        std::ofstream f(path);
        f << "[keyboard]\n";
        f << "key_a = 4\n";
        f << "key_b = 5\n";
    }

    auto result = MapLoader::loadMapFile(path);
    CHECK(!result.success, "load FAILED (format validation)");
    CHECK(!result.errorMessage.empty(), "error message present");

    // Cleanup
    std::remove(path.c_str());

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 15: Integration with SymbolDatabase
// ---------------------------------------------------------------------------

static void test_symbol_database_integration()
{
    TEST_BEGIN("SymbolDatabase: loadMapSymbols + clearMapSymbols");

    SymbolDatabase db;

    // Add a user-defined symbol
    db.addSymbol(0x0100, "user_func", SymbolType::Function);

    // Simulate MAP loading
    std::string content =
        "_main = $0252 ; addr, public\n"
        "_data = $0300 ; addr, local\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");

    for (const auto &ms : result.symbols) {
        SymbolType type = (ms.isAddress && ms.visibility == MapSymbolVisibility::Public)
            ? SymbolType::Function : SymbolType::Label;
        if (db.addSymbol(ms.address, ms.name, type)) {
            auto *p = const_cast<DebugSymbol*>(db.findSymbol(ms.address));
            if (p) {
                p->fromMap = true;
                p->sourceFile = ms.sourceFile;
                p->sourceLine = ms.sourceLine;
            }
        }
    }

    CHECK_EQ(3u, (unsigned)db.symbolCount(), "3 symbols total");

    // Clear MAP symbols
    db.clearMapSymbols();
    CHECK_EQ(1u, (unsigned)db.symbolCount(), "1 symbol after clearMap");
    CHECK(db.findSymbol(0x0100) != nullptr, "user symbol preserved");
    CHECK(db.findSymbol(0x0252) == nullptr, "MAP symbol removed");
    CHECK(db.findSymbol(0x0300) == nullptr, "MAP symbol removed");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 16: Multiple symbols with various types
// ---------------------------------------------------------------------------

static void test_multiple_symbols()
{
    TEST_BEGIN("multiple symbols with various types");

    std::string content =
        "_main              = $0252 ; addr, public, main.c::main::0::1:109\n"
        "_gfx_set_mode      = $02B3 ; addr, public, mode.c::gfx_set_mode::0::0:39\n"
        "_graph_fill_planes = $018E ; addr, public\n"
        "_plane_fill        = $0228 ; addr, public\n"
        "fp_c000_inner      = $01D1 ; addr, local\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");
    CHECK_EQ(5, result.parsedCount, "5 symbols parsed");
    CHECK_EQ(0, result.skippedLines, "0 lines skipped");

    // Verify all addresses
    CHECK_EQ(0x0252, result.symbols[0].address, "_main addr");
    CHECK_EQ(0x02B3, result.symbols[1].address, "_gfx_set_mode addr");
    CHECK_EQ(0x018E, result.symbols[2].address, "_graph_fill_planes addr");
    CHECK_EQ(0x0228, result.symbols[3].address, "_plane_fill addr");
    CHECK_EQ(0x01D1, result.symbols[4].address, "fp_c000_inner addr");

    // Verify visibility
    CHECK(result.symbols[0].visibility == MapSymbolVisibility::Public, "_main public");
    CHECK(result.symbols[4].visibility == MapSymbolVisibility::Local, "fp_c000_inner local");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 17: Symbol without comment (no "; ..." part)
// ---------------------------------------------------------------------------

static void test_no_comment()
{
    TEST_BEGIN("symbol without comment");

    std::string content =
        "_simple = $0400\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");
    CHECK_EQ(1, result.parsedCount, "1 symbol parsed");
    CHECK_STR("_simple", result.symbols[0].name.c_str(), "name");
    CHECK_EQ(0x0400, result.symbols[0].address, "address");
    CHECK(!result.symbols[0].isAddress, "isAddress = false (no 'addr' tag)");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 18: const entries are skipped (Stage 6.2.1)
// ---------------------------------------------------------------------------

static void test_const_entries_skipped()
{
    TEST_BEGIN("S6.2.1: const entries skipped");

    std::string content =
        "_myconst  = $0042 ; const, public\n"
        "_main     = $0100 ; addr, public\n"
        "_label    = $0103 ; addr, local\n"
        "_value    = $FFFF ; const, public\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");
    CHECK_EQ(2, result.parsedCount, "2 addr symbols parsed");
    CHECK_EQ(2, result.skippedLines, "2 const lines skipped");

    // _myconst should NOT be present
    bool foundMyconst = false;
    bool foundValue = false;
    for (const auto &sym : result.symbols) {
        if (sym.name == "_myconst") foundMyconst = true;
        if (sym.name == "_value") foundValue = true;
    }
    CHECK(!foundMyconst, "_myconst (const) not in symbols");
    CHECK(!foundValue, "_value (const at $FFFF) not in symbols");

    // _main and _label should be present
    CHECK_EQ(0x0100, result.symbols[0].address, "_main at $0100");
    CHECK_EQ(0x0103, result.symbols[1].address, "_label at $0103");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 19: const does not block addr at same address (Stage 6.2.1)
// ---------------------------------------------------------------------------

static void test_const_does_not_block_addr()
{
    TEST_BEGIN("S6.2.1: const does not block addr at same address");

    // Real scenario from check_bugs.map:
    // STACK_TOP = $0100 ; const, local  — should be skipped
    // start     = $0100 ; addr, local   — should be loaded
    std::string content =
        "STACK_TOP = $0100 ; const, local, , startup_asm, , startup.asm:26\n"
        "start     = $0100 ; addr, local, , startup_asm, , startup.asm:34\n";

    auto result = MapLoader::parseMapContent(content);
    CHECK(result.success, "parse succeeded");
    CHECK_EQ(1, result.parsedCount, "1 addr symbol parsed");
    CHECK_EQ(1, result.skippedLines, "1 const line skipped");

    // Load into SymbolDatabase
    SymbolDatabase db;
    for (const auto &ms : result.symbols) {
        SymbolType type = (ms.isAddress && ms.visibility == MapSymbolVisibility::Public)
            ? SymbolType::Function : SymbolType::Label;
        db.addSymbol(ms.address, ms.name, type);
    }

    // Address 0x0100 should be 'start', not STACK_TOP
    const DebugSymbol *sym = db.findSymbol(0x0100);
    CHECK(sym != nullptr, "symbol at 0x0100 exists");
    if (sym) {
        CHECK_STR("start", sym->name.c_str(), "0x0100 is 'start' (not STACK_TOP)");
    }

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 20: Real check_bugs.map (Stage 6.2.1 regression)
// ---------------------------------------------------------------------------

static void test_real_map_check_bugs()
{
    TEST_BEGIN("S6.2.1: real check_bugs.map regression");

    auto result = MapLoader::loadMapFile("/home/alexey/Projects/vector-games/tests/check_bugs/check_bugs.map");

    // If file not found, skip (CI might not have it)
    bool fileNotFound = !result.success &&
        result.errorMessage.find("cannot open") != std::string::npos;
    if (fileNotFound) {
        printf("  SKIP: check_bugs.map not found\n");
    } else {
        CHECK(result.success, "parse succeeded");
        CHECK_EQ(86, result.parsedCount, "86 addr symbols parsed");
        CHECK_EQ(22, result.skippedLines, "22 const lines skipped");

        // Verify no const entries leaked through
        int constCount = 0;
        for (const auto &sym : result.symbols) {
            if (!sym.isAddress) constCount++;
        }
        CHECK_EQ(0, constCount, "no non-addr symbols in output");

        // Load into SymbolDatabase — verify no crash, correct symbols
        SymbolDatabase db;
        int added = 0;
        for (const auto &ms : result.symbols) {
            SymbolType type = (ms.isAddress && ms.visibility == MapSymbolVisibility::Public)
                ? SymbolType::Function : SymbolType::Label;
            if (db.addSymbol(ms.address, ms.name, type)) {
                auto *p = const_cast<DebugSymbol*>(db.findSymbol(ms.address));
                if (p) { p->fromMap = true; p->sourceFile = ms.sourceFile; p->sourceLine = ms.sourceLine; }
                added++;
            }
        }
        CHECK(added > 60, "60+ symbols loaded (86 minus duplicates)");

        // Verify key addresses
        const DebugSymbol *mainSym = db.findSymbol(0x0252);
        CHECK(mainSym != nullptr, "_main found at 0x0252");
        if (mainSym) CHECK_STR("_main", mainSym->name.c_str(), "name = _main");

        const DebugSymbol *startSym = db.findSymbol(0x0100);
        CHECK(startSym != nullptr, "start found at 0x0100");
        if (startSym) CHECK_STR("start", startSym->name.c_str(), "name = start (not STACK_TOP)");
    }

    TEST_END();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  MAP Loader Tests (Stage 6.2)\033[0m\n");
    printf("\033[1;33m========================================\033[0m\n");

    test_public_function();
    test_local_label();
    test_source_location();
    test_hex_boundaries();
    test_invalid_address();
    test_malformed_lines();
    test_duplicate_symbols();
    test_old_keyboard_map_rejected();
    test_empty_file();
    test_comments_only();
    test_map_path_from_rom();
    test_load_nonexistent();
    test_load_from_disk();
    test_load_old_keyboard_from_disk();
    test_symbol_database_integration();
    test_multiple_symbols();
    test_no_comment();
    test_const_entries_skipped();
    test_const_does_not_block_addr();
    test_real_map_check_bugs();

    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  Results: %d passed, %d failed (of %d)\033[0m\n",
           tests_passed, tests_failed, tests_run);
    printf("\033[1;33m========================================\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
