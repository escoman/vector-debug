// Symbol Database tests — Stage 4.1
//
// Tests for SymbolDatabase: symbols, memory classification, xrefs, call graph.
// No GUI or board dependency — pure data structure tests.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "symbol_database.h"

// ---------------------------------------------------------------------------
// Test framework (same as test_backend.cpp)
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
// Tests: Symbols
// ---------------------------------------------------------------------------

static void test_add_find_symbol()
{
    TEST_BEGIN("add and find symbol");
    SymbolDatabase db;

    CHECK(db.addSymbol(0x0345, "DRAW_SPRITE", SymbolType::Function), "add function");
    CHECK(db.addSymbol(0x4000, "PLAYER_X", SymbolType::Label), "add label");

    const DebugSymbol *sym = db.findSymbol(0x0345);
    CHECK(sym != nullptr, "find function by address");
    CHECK_STR("DRAW_SPRITE", sym->name, "function name");
    CHECK_EQ((unsigned)SymbolType::Function, (unsigned)sym->type, "function type");

    sym = db.findSymbol(0x4000);
    CHECK(sym != nullptr, "find label by address");
    CHECK_STR("PLAYER_X", sym->name, "label name");
    CHECK_EQ((unsigned)SymbolType::Label, (unsigned)sym->type, "label type");

    CHECK(db.findSymbol(0x9999) == nullptr, "not found returns nullptr");
    CHECK_EQ(2u, (unsigned)db.symbolCount(), "symbol count");
    TEST_END();
}

static void test_add_duplicate()
{
    TEST_BEGIN("add duplicate symbol fails");
    SymbolDatabase db;

    CHECK(db.addSymbol(0x0345, "DRAW_SPRITE", SymbolType::Function), "first add");
    CHECK(!db.addSymbol(0x0345, "OTHER", SymbolType::Label), "duplicate rejected");
    CHECK_EQ(1u, (unsigned)db.symbolCount(), "count unchanged");
    TEST_END();
}

static void test_rename_symbol()
{
    TEST_BEGIN("rename symbol");
    SymbolDatabase db;

    db.addSymbol(0x0345, "DRAW_SPRITE", SymbolType::Function);
    CHECK(db.renameSymbol(0x0345, "RENDER_SPRITE"), "rename succeeds");

    const DebugSymbol *sym = db.findSymbol(0x0345);
    CHECK(sym != nullptr, "still found");
    CHECK_STR("RENDER_SPRITE", sym->name, "new name");

    CHECK(!db.renameSymbol(0x9999, "NOPE"), "rename nonexistent fails");
    TEST_END();
}

static void test_remove_symbol()
{
    TEST_BEGIN("remove symbol");
    SymbolDatabase db;

    db.addSymbol(0x0345, "DRAW_SPRITE", SymbolType::Function);
    db.addSymbol(0x4000, "PLAYER_X", SymbolType::Label);

    CHECK(db.removeSymbol(0x0345), "remove succeeds");
    CHECK(db.findSymbol(0x0345) == nullptr, "no longer found");
    CHECK_EQ(1u, (unsigned)db.symbolCount(), "count decremented");

    CHECK(!db.removeSymbol(0x9999), "remove nonexistent fails");
    TEST_END();
}

static void test_comment()
{
    TEST_BEGIN("set and get comment");
    SymbolDatabase db;

    db.addSymbol(0x0345, "DRAW_SPRITE", SymbolType::Function);
    CHECK(db.setComment(0x0345, "Draws a sprite on screen"), "set comment");

    const DebugSymbol *sym = db.findSymbol(0x0345);
    CHECK_STR("Draws a sprite on screen", sym->comment, "comment text");

    CHECK(!db.setComment(0x9999, "nope"), "comment on nonexistent fails");
    TEST_END();
}

static void test_comment_normalizes_breaks()
{
    TEST_BEGIN("setComment strips line breaks");
    SymbolDatabase db;

    db.addSymbol(0x0345, "DRAW_SPRITE", SymbolType::Function);
    CHECK(db.setComment(0x0345, "line1\nline2"), "set multiline comment");
    CHECK_STR("line1 line2", db.findSymbol(0x0345)->comment, "newline -> single space");

    db.setComment(0x0345, "a\r\n   b");
    CHECK_STR("a b", db.findSymbol(0x0345)->comment, "crlf+indent -> one space");

    // Idempotent: text without a break is untouched.
    db.setComment(0x0345, "plain text");
    CHECK_STR("plain text", db.findSymbol(0x0345)->comment, "no break preserved");
    TEST_END();
}

static void test_find_by_name()
{
    TEST_BEGIN("find symbol by name");
    SymbolDatabase db;

    db.addSymbol(0x0345, "DRAW_SPRITE", SymbolType::Function);
    db.addSymbol(0x7A20, "INIT", SymbolType::Function);
    db.addSymbol(0x4000, "PLAYER_X", SymbolType::Label);

    const DebugSymbol *sym = db.findSymbolByName("INIT");
    CHECK(sym != nullptr, "found by name");
    CHECK_EQ(0x7A20u, (unsigned)sym->address, "correct address");

    CHECK(db.findSymbolByName("NONEXISTENT") == nullptr, "not found by name");
    TEST_END();
}

static void test_all_symbols_sorted()
{
    TEST_BEGIN("all symbols sorted by address");
    SymbolDatabase db;

    db.addSymbol(0x7A20, "INIT", SymbolType::Function);
    db.addSymbol(0x0345, "DRAW_SPRITE", SymbolType::Function);
    db.addSymbol(0x4000, "PLAYER_X", SymbolType::Label);

    auto all = db.allSymbols();
    CHECK_EQ(3u, (unsigned)all.size(), "3 symbols");
    CHECK_EQ(0x0345u, (unsigned)all[0].address, "first = 0345");
    CHECK_EQ(0x4000u, (unsigned)all[1].address, "second = 4000");
    CHECK_EQ(0x7A20u, (unsigned)all[2].address, "third = 7A20");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Auto-name and display name
// ---------------------------------------------------------------------------

static void test_auto_name()
{
    TEST_BEGIN("auto-name generation");
    CHECK_STR("sub_7A20", SymbolDatabase::autoName(0x7A20), "auto-name 7A20");
    CHECK_STR("sub_0000", SymbolDatabase::autoName(0x0000), "auto-name 0000");
    CHECK_STR("sub_C000", SymbolDatabase::autoName(0xC000), "auto-name C000");
    TEST_END();
}

static void test_display_name()
{
    TEST_BEGIN("display name resolution");
    SymbolDatabase db;

    // No symbol, not a call target → empty
    CHECK_STR("", db.displayName(0x1234), "unknown address → empty");

    // Mark as call target → auto-name
    db.markCallTarget(0x7A20);
    CHECK_STR("sub_7A20", db.displayName(0x7A20), "call target → auto-name");

    // Add user symbol → user name takes priority
    db.addSymbol(0x7A20, "INIT", SymbolType::Function);
    CHECK_STR("INIT", db.displayName(0x7A20), "user name overrides auto");

    // Remove symbol → falls back to auto-name (still a call target)
    db.removeSymbol(0x7A20);
    CHECK_STR("sub_7A20", db.displayName(0x7A20), "fallback to auto-name");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Memory classification
// ---------------------------------------------------------------------------

static void test_region_basic()
{
    TEST_BEGIN("memory region basic operations");
    SymbolDatabase db;

    CHECK(db.setRegion(0x0000, 0x03FF, MemoryRegionType::Code), "set code region");
    CHECK(db.setRegion(0x4000, 0x40FF, MemoryRegionType::Data), "set data region");

    CHECK_EQ((unsigned)MemoryRegionType::Code, (unsigned)db.classify(0x0000), "0000 = Code");
    CHECK_EQ((unsigned)MemoryRegionType::Code, (unsigned)db.classify(0x0200), "0200 = Code");
    CHECK_EQ((unsigned)MemoryRegionType::Code, (unsigned)db.classify(0x03FF), "03FF = Code");
    CHECK_EQ((unsigned)MemoryRegionType::Unknown, (unsigned)db.classify(0x0400), "0400 = Unknown");
    CHECK_EQ((unsigned)MemoryRegionType::Data, (unsigned)db.classify(0x4000), "4000 = Data");
    CHECK_EQ((unsigned)MemoryRegionType::Data, (unsigned)db.classify(0x40FF), "40FF = Data");
    CHECK_EQ((unsigned)MemoryRegionType::Unknown, (unsigned)db.classify(0x4100), "4100 = Unknown");

    auto regions = db.allRegions();
    CHECK_EQ(2u, (unsigned)regions.size(), "2 regions");
    CHECK_EQ(0x0000u, (unsigned)regions[0].start, "first starts at 0000");
    CHECK_EQ(0x4000u, (unsigned)regions[1].start, "second starts at 4000");
    TEST_END();
}

static void test_region_overlap()
{
    TEST_BEGIN("memory region overlap handling");
    SymbolDatabase db;

    db.setRegion(0x0000, 0x03FF, MemoryRegionType::Code);
    db.setRegion(0x0200, 0x05FF, MemoryRegionType::Data);

    // 0000-01FF should still be Code (trimmed)
    CHECK_EQ((unsigned)MemoryRegionType::Code, (unsigned)db.classify(0x0100), "0100 = Code (trimmed)");
    // 0200-05FF should be Data (overwrites Code)
    CHECK_EQ((unsigned)MemoryRegionType::Data, (unsigned)db.classify(0x0200), "0200 = Data (overlap)");
    CHECK_EQ((unsigned)MemoryRegionType::Data, (unsigned)db.classify(0x0400), "0400 = Data");
    // 0600 should be Unknown
    CHECK_EQ((unsigned)MemoryRegionType::Unknown, (unsigned)db.classify(0x0600), "0600 = Unknown");

    auto regions = db.allRegions();
    CHECK_EQ(2u, (unsigned)regions.size(), "2 regions after overlap");
    TEST_END();
}

static void test_region_remove()
{
    TEST_BEGIN("memory region remove");
    SymbolDatabase db;

    db.setRegion(0x0000, 0x03FF, MemoryRegionType::Code);
    CHECK(db.removeRegion(0x0000), "remove succeeds");
    CHECK_EQ((unsigned)MemoryRegionType::Unknown, (unsigned)db.classify(0x0200), "now Unknown");
    CHECK(!db.removeRegion(0x0000), "remove again fails");
    TEST_END();
}

static void test_region_type_transitions()
{
    TEST_BEGIN("region type transitions: Unknown→Code→Data→Unknown");
    SymbolDatabase db;

    CHECK_EQ((unsigned)MemoryRegionType::Unknown, (unsigned)db.classify(0x1000), "initial Unknown");

    db.setRegion(0x1000, 0x1FFF, MemoryRegionType::Code);
    CHECK_EQ((unsigned)MemoryRegionType::Code, (unsigned)db.classify(0x1000), "now Code");

    db.setRegion(0x1000, 0x1FFF, MemoryRegionType::Data);
    CHECK_EQ((unsigned)MemoryRegionType::Data, (unsigned)db.classify(0x1000), "now Data");

    db.removeRegion(0x1000);
    CHECK_EQ((unsigned)MemoryRegionType::Unknown, (unsigned)db.classify(0x1000), "back to Unknown");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Cross-references
// ---------------------------------------------------------------------------

// Helper: create a memory image for xref testing
// Layout:
//   0x0100: CD 20 02   CALL 0220
//   0x0103: C3 50 01   JMP 0150
//   0x0106: CF         RST 1  (target = 0x0008)
//   0x0107: C7         RST 0  (target = 0x0000)
//   0x0108: 00         NOP
//   ...
//   0x0150: CD 20 02   CALL 0220
//   ...
//   0x0220: C9         RET
static void setup_xref_memory(std::vector<uint8_t> &mem)
{
    mem.resize(0x0300, 0x00);  // zero-filled (NOP)

    // 0x0100: CALL 0220
    mem[0x0100] = 0xCD;
    mem[0x0101] = 0x20;
    mem[0x0102] = 0x02;

    // 0x0103: JMP 0150
    mem[0x0103] = 0xC3;
    mem[0x0104] = 0x50;
    mem[0x0105] = 0x01;

    // 0x0106: RST 1 (target = 0x08)
    mem[0x0106] = 0xCF;

    // 0x0107: RST 0 (target = 0x00)
    mem[0x0107] = 0xC7;

    // 0x0108: NOP
    mem[0x0108] = 0x00;

    // 0x0150: CALL 0220
    mem[0x0150] = 0xCD;
    mem[0x0151] = 0x20;
    mem[0x0152] = 0x02;

    // 0x0220: RET
    mem[0x0220] = 0xC9;
}

static void test_xrefs_call()
{
    TEST_BEGIN("xrefs: CALL detection");
    SymbolDatabase db;
    std::vector<uint8_t> mem;
    setup_xref_memory(mem);

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };

    db.rebuildXrefs(readByte);

    auto refs = db.xrefsTo(0x0220);
    CHECK_EQ(2u, (unsigned)refs.size(), "2 CALLs to 0220");
    CHECK_EQ(0x0100u, (unsigned)refs[0].from, "first from 0100");
    CHECK_EQ(0x0150u, (unsigned)refs[1].from, "second from 0150");
    TEST_END();
}

static void test_xrefs_jmp()
{
    TEST_BEGIN("xrefs: JMP detection");
    SymbolDatabase db;
    std::vector<uint8_t> mem;
    setup_xref_memory(mem);

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };

    db.rebuildXrefs(readByte);

    auto refs = db.xrefsTo(0x0150);
    CHECK_EQ(1u, (unsigned)refs.size(), "1 JMP to 0150");
    CHECK_EQ(0x0103u, (unsigned)refs[0].from, "from 0103");
    TEST_END();
}

static void test_xrefs_rst()
{
    TEST_BEGIN("xrefs: RST detection");
    SymbolDatabase db;
    std::vector<uint8_t> mem;
    setup_xref_memory(mem);

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };

    db.rebuildXrefs(readByte);

    // RST 1 → target 0x0008
    auto refs8 = db.xrefsTo(0x0008);
    CHECK_EQ(1u, (unsigned)refs8.size(), "1 RST to 0008");
    CHECK_EQ(0x0106u, (unsigned)refs8[0].from, "from 0106");

    // RST 0 → target 0x0000
    auto refs0 = db.xrefsTo(0x0000);
    CHECK_EQ(1u, (unsigned)refs0.size(), "1 RST to 0000");
    CHECK_EQ(0x0107u, (unsigned)refs0[0].from, "from 0107");
    TEST_END();
}

// Regression for the callers/xrefs fix: a CALL that the aligned Pass-1 linear
// walk steps over (because a preceding MVI consumes its opcode byte) must still
// be recovered by the exhaustive Pass-2, provided the target is a known symbol.
static void test_xrefs_misaligned_call_recovered()
{
    TEST_BEGIN("xrefs: misaligned CALL recovered by exhaustive pass");
    SymbolDatabase db;
    std::vector<uint8_t> mem;
    mem.resize(0x0300, 0x00);

    // 0x0100: MVI B,d (length 2) -> aligned walk jumps 0x0100 -> 0x0102,
    // skipping the CALL opcode that actually lives at 0x0101.
    mem[0x0100] = 0x06;
    mem[0x0101] = 0xCD;  // CALL 0x0220 (misaligned on purpose)
    mem[0x0102] = 0x20;
    mem[0x0103] = 0x02;

    db.addSymbol(0x0220, "sub_0220", SymbolType::Function);

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };
    db.rebuildXrefs(readByte);

    auto refs = db.xrefsTo(0x0220);
    bool found = false;
    for (const auto &x : refs)
        if (x.from == 0x0101 && x.to == 0x0220) found = true;
    CHECK(found, "exhaustive pass recovered the misaligned CALL at 0x0101");
    TEST_END();
}

// The exhaustive pass must NOT fabricate xrefs from data bytes: a CALL-shaped
// sequence whose target is neither a symbol nor a Code region is rejected.
static void test_xrefs_data_false_positive_suppressed()
{
    TEST_BEGIN("xrefs: implausible target suppressed (no data false positive)");
    SymbolDatabase db;
    std::vector<uint8_t> mem;
    mem.resize(0x0300, 0x00);

    mem[0x0100] = 0x06;
    mem[0x0101] = 0xCD;  // CALL 0x0900 (misaligned, target is undefined)
    mem[0x0102] = 0x00;
    mem[0x0103] = 0x09;

    // A known symbol elsewhere enables the exhaustive pass, but 0x0900 is not
    // a symbol and not in any Code region -> the candidate must be dropped.
    db.addSymbol(0x0220, "sub_0220", SymbolType::Function);

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };
    db.rebuildXrefs(readByte);

    auto refs = db.xrefsTo(0x0900);
    CHECK_EQ(0u, (unsigned)refs.size(), "no xref fabricated for undefined target");
    TEST_END();
}

static void test_xrefs_from()
{
    TEST_BEGIN("xrefs: xrefsFrom");
    SymbolDatabase db;
    std::vector<uint8_t> mem;
    setup_xref_memory(mem);

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };

    db.rebuildXrefs(readByte);

    auto refs = db.xrefsFrom(0x0100);
    CHECK_EQ(1u, (unsigned)refs.size(), "0100 has 1 xref");
    CHECK_EQ(0x0220u, (unsigned)refs[0].to, "to 0220");

    auto refs2 = db.xrefsFrom(0x0107);
    CHECK_EQ(1u, (unsigned)refs2.size(), "0107 has 1 xref");
    CHECK_EQ(0x0000u, (unsigned)refs2[0].to, "to 0000");
    TEST_END();
}

static void test_xrefs_multiple()
{
    TEST_BEGIN("xrefs: multiple references");
    SymbolDatabase db;

    // 3 CALLs to the same target from different addresses
    std::vector<uint8_t> mem(0x1000, 0x00);

    // 0x0100: CALL 0500
    mem[0x0100] = 0xCD; mem[0x0101] = 0x00; mem[0x0102] = 0x05;
    // 0x0200: CALL 0500
    mem[0x0200] = 0xCD; mem[0x0201] = 0x00; mem[0x0202] = 0x05;
    // 0x0300: CALL 0500
    mem[0x0300] = 0xCD; mem[0x0301] = 0x00; mem[0x0302] = 0x05;

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };

    db.rebuildXrefs(readByte);

    auto refs = db.xrefsTo(0x0500);
    CHECK_EQ(3u, (unsigned)refs.size(), "3 references to 0500");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Call graph
// ---------------------------------------------------------------------------

static void test_call_graph_basic()
{
    TEST_BEGIN("call graph: basic edges");
    SymbolDatabase db;
    std::vector<uint8_t> mem;
    setup_xref_memory(mem);

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };

    db.rebuildXrefs(readByte);

    auto edges = db.callGraph();
    // Should include: CALL 0220 (x2), RST 1 (0x08), RST 0 (0x00)
    // Should NOT include: JMP 0150 (it's a jump, not a call)
    CHECK_EQ(4u, (unsigned)edges.size(), "4 call graph edges (2 CALL + 2 RST)");

    // Verify all edges point to call targets
    for (const auto &e : edges) {
        CHECK(db.displayName(e.to) != "", "target has display name");
    }
    TEST_END();
}

static void test_call_graph_recursive()
{
    TEST_BEGIN("call graph: recursive call");
    SymbolDatabase db;
    std::vector<uint8_t> mem(0x1000, 0x00);

    // 0x0500: CALL 0500 (recursive)
    mem[0x0500] = 0xCD; mem[0x0501] = 0x00; mem[0x0502] = 0x05;

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };

    db.rebuildXrefs(readByte);

    auto edges = db.callGraph();
    CHECK_EQ(1u, (unsigned)edges.size(), "1 edge (self-call)");
    CHECK_EQ(0x0500u, (unsigned)edges[0].from, "from 0500");
    CHECK_EQ(0x0500u, (unsigned)edges[0].to, "to 0500");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Clear
// ---------------------------------------------------------------------------

static void test_clear()
{
    TEST_BEGIN("clear all data");
    SymbolDatabase db;

    db.addSymbol(0x0345, "DRAW_SPRITE", SymbolType::Function);
    db.setRegion(0x0000, 0x03FF, MemoryRegionType::Code);
    db.markCallTarget(0x7A20);

    std::vector<uint8_t> mem(0x1000, 0x00);
    mem[0x0100] = 0xCD; mem[0x0101] = 0x20; mem[0x0102] = 0x07;
    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };
    db.rebuildXrefs(readByte);

    db.clear();

    CHECK_EQ(0u, (unsigned)db.symbolCount(), "symbols cleared");
    CHECK_EQ(0u, (unsigned)db.allRegions().size(), "regions cleared");
    CHECK_EQ(0u, (unsigned)db.allXrefs().size(), "xrefs cleared");
    CHECK_STR("", db.displayName(0x7A20), "call targets cleared");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Code region scanning
// ---------------------------------------------------------------------------

static void test_xrefs_code_region_only()
{
    TEST_BEGIN("xrefs: scan only Code regions");
    SymbolDatabase db;

    // Mark 0x0000-0x00FF as Code, rest is Unknown
    db.setRegion(0x0000, 0x00FF, MemoryRegionType::Code);

    std::vector<uint8_t> mem(0x1000, 0x00);

    // CALL 0500 at 0x0050 (in Code region)
    mem[0x0050] = 0xCD; mem[0x0051] = 0x00; mem[0x0052] = 0x05;

    // CALL 0600 at 0x0200 (NOT in Code region)
    mem[0x0200] = 0xCD; mem[0x0201] = 0x00; mem[0x0202] = 0x06;

    auto readByte = [&mem](uint16_t addr) -> uint8_t {
        return (addr < mem.size()) ? mem[addr] : 0x00;
    };

    db.rebuildXrefs(readByte);

    // Should find only the CALL in the Code region
    auto refs500 = db.xrefsTo(0x0500);
    CHECK_EQ(1u, (unsigned)refs500.size(), "CALL in Code region found");

    auto refs600 = db.xrefsTo(0x0600);
    CHECK_EQ(0u, (unsigned)refs600.size(), "CALL outside Code region NOT found");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  Symbol Database Tests (Stage 4.1)\033[0m\n");
    printf("\033[1;33m========================================\033[0m\n");

    // Symbols
    test_add_find_symbol();
    test_add_duplicate();
    test_rename_symbol();
    test_remove_symbol();
    test_comment();
    test_comment_normalizes_breaks();
    test_find_by_name();
    test_all_symbols_sorted();

    // Auto-name / display name
    test_auto_name();
    test_display_name();

    // Memory classification
    test_region_basic();
    test_region_overlap();
    test_region_remove();
    test_region_type_transitions();

    // Cross-references
    test_xrefs_call();
    test_xrefs_jmp();
    test_xrefs_rst();
    test_xrefs_misaligned_call_recovered();
    test_xrefs_data_false_positive_suppressed();
    test_xrefs_from();
    test_xrefs_multiple();

    // Call graph
    test_call_graph_basic();
    test_call_graph_recursive();

    // Clear
    test_clear();

    // Code region scanning
    test_xrefs_code_region_only();

    printf("\n\033[1;33m========================================\033[0m\n");
    printf("\033[1;33m  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", \033[1;31m%d FAILED\033[0m", tests_failed);
    }
    printf("\n\033[1;33m========================================\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
