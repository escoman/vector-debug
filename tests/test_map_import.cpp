// Unit tests for MAP Import — Stage 6.11
//
// Tests for MapImport: MAP→RDB import and RDB→SymbolDatabase sync.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "map_import.h"
#include "map_loader.h"
#include "rdb_controller.h"
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
        printf("\n=== TEST: %s ===\n", name); \
        const char *_test_name = name; \
        bool _test_ok = true; \
        (void)_test_name;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL %s (line %d)\n", msg, __LINE__); \
            _test_ok = false; \
        } else { \
            printf("  ok %s\n", msg); \
        } \
    } while(0)

#define CHECK_EQ(expected, actual, msg) \
    do { \
        auto _e = (expected); auto _a = (actual); \
        if (_e != _a) { \
            printf("  FAIL %s: expected %d, got %d (line %d)\n", \
                   msg, (int)_e, (int)_a, __LINE__); \
            _test_ok = false; \
        } else { \
            printf("  ok %s\n", msg); \
        } \
    } while(0)

#define CHECK_STR(expected, actual, msg) \
    do { \
        const char *_e = (expected); const char *_a = (actual); \
        if (strcmp(_e, _a) != 0) { \
            printf("  FAIL %s: expected \"%s\", got \"%s\" (line %d)\n", \
                   msg, _e, _a, __LINE__); \
            _test_ok = false; \
        } else { \
            printf("  ok %s\n", msg); \
        } \
    } while(0)

#define TEST_END() \
        if (_test_ok) { \
            tests_passed++; \
            printf(" PASSED %s\n", _test_name); \
        } else { \
            tests_failed++; \
            printf(" FAILED %s\n", _test_name); \
        } \
    } while(0)

// ---------------------------------------------------------------------------
// Helper: parse MAP content and return MapLoadResult
// ---------------------------------------------------------------------------

static MapLoadResult parseMap(const std::string &content) {
    return MapLoader::parseMapContent(content);
}

// ---------------------------------------------------------------------------
// Test 1: Basic MAP → RDB import
// ---------------------------------------------------------------------------

static void test_basic_import()
{
    TEST_BEGIN("basic MAP → RDB import");

    auto mapResult = parseMap(
        "_main   = $0252 ; addr, public, main.c::main::0::1:109\n"
        "_helper = $0300 ; addr, public\n"
        "loop1   = $0260 ; addr, local\n"
    );
    CHECK(mapResult.success, "MAP parsed");

    RdbController rdb;
    rdb.initialize("vector06c", {});

    int added = MapImport::importMapToRdb(mapResult, rdb);
    CHECK_EQ(3, added, "3 objects imported");
    CHECK_EQ(3, (int)rdb.objectCount(), "RDB has 3 objects");

    // Check types
    const RdbObject *mainObj = rdb.getObject(0x0252);
    CHECK(mainObj != nullptr, "main found");
    CHECK(mainObj->type == RdbObjectType::Function, "main is Function");
    CHECK_STR("_main", mainObj->name.c_str(), "name = _main");

    const RdbObject *helperObj = rdb.getObject(0x0300);
    CHECK(helperObj != nullptr, "helper found");
    CHECK(helperObj->type == RdbObjectType::Function, "helper is Function");

    const RdbObject *loopObj = rdb.getObject(0x0260);
    CHECK(loopObj != nullptr, "loop1 found");
    CHECK(loopObj->type == RdbObjectType::Label, "loop1 is Label (local)");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 2: Source location preserved as properties
// ---------------------------------------------------------------------------

static void test_source_location_import()
{
    TEST_BEGIN("source location → RDB properties");

    auto mapResult = parseMap(
        "_main = $0252 ; addr, public, main.c::main::0::1:109\n"
    );
    CHECK(mapResult.success, "MAP parsed");

    RdbController rdb;
    rdb.initialize("vector06c", {});
    MapImport::importMapToRdb(mapResult, rdb);

    const RdbObject *obj = rdb.getObject(0x0252);
    CHECK(obj != nullptr, "object found");

    auto itFile = obj->properties.find("source_file");
    CHECK(itFile != obj->properties.end(), "source_file property exists");
    if (itFile != obj->properties.end()) {
        CHECK_STR("main.c", itFile->second.stringValue.c_str(), "source_file = main.c");
    }

    auto itLine = obj->properties.find("source_line");
    CHECK(itLine != obj->properties.end(), "source_line property exists");
    if (itLine != obj->properties.end()) {
        CHECK_EQ(109, (int)itLine->second.intValue, "source_line = 109");
    }

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 3: No overwrite — existing RDB objects not replaced
// ---------------------------------------------------------------------------

static void test_no_overwrite()
{
    TEST_BEGIN("import does not overwrite existing objects");

    RdbController rdb;
    rdb.initialize("vector06c", {});

    // Pre-populate RDB with a different name
    RdbObject existing;
    existing.address = 0x0252;
    existing.type = RdbObjectType::Function;
    existing.name = "user_renamed";
    rdb.addObject(existing);

    auto mapResult = parseMap(
        "_main = $0252 ; addr, public\n"
        "_new  = $0300 ; addr, public\n"
    );
    CHECK(mapResult.success, "MAP parsed");

    int added = MapImport::importMapToRdb(mapResult, rdb);
    CHECK_EQ(1, added, "only 1 new object imported (address 0x0252 skipped)");

    const RdbObject *obj = rdb.getObject(0x0252);
    CHECK(obj != nullptr, "object at 0x0252 exists");
    CHECK_STR("user_renamed", obj->name.c_str(), "name preserved as user_renamed");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 4: Failed MAP result imports nothing
// ---------------------------------------------------------------------------

static void test_failed_map_imports_nothing()
{
    TEST_BEGIN("failed MAP result imports nothing");

    MapLoadResult failed;
    failed.success = false;

    RdbController rdb;
    rdb.initialize("vector06c", {});

    int added = MapImport::importMapToRdb(failed, rdb);
    CHECK_EQ(0, added, "0 objects imported from failed result");
    CHECK_EQ(0, (int)rdb.objectCount(), "RDB still empty");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 5: RDB → SymbolDatabase sync
// ---------------------------------------------------------------------------

static void test_rdb_to_symboldb_sync()
{
    TEST_BEGIN("RDB → SymbolDatabase sync");

    RdbController rdb;
    rdb.initialize("vector06c", {});

    // Add objects to RDB
    RdbObject func;
    func.address = 0x0100;
    func.type = RdbObjectType::Function;
    func.name = "start";
    func.comment = "Entry point";
    rdb.addObject(func);

    RdbObject label;
    label.address = 0x0200;
    label.type = RdbObjectType::Label;
    label.name = "loop";
    rdb.addObject(label);

    // Sync to SymbolDatabase
    SymbolDatabase db;
    int synced = MapImport::syncRdbToSymbolDatabase(rdb, db);
    CHECK_EQ(2, synced, "2 symbols synced");
    CHECK_EQ(2, (int)db.symbolCount(), "SymbolDatabase has 2 symbols");

    const DebugSymbol *sym1 = db.findSymbol(0x0100);
    CHECK(sym1 != nullptr, "start found in SymbolDatabase");
    if (sym1) {
        CHECK_STR("start", sym1->name.c_str(), "name = start");
        CHECK(sym1->type == SymbolType::Function, "type = Function");
        CHECK(sym1->fromMap, "fromMap = true");
        CHECK_STR("Entry point", sym1->comment.c_str(), "comment preserved");
    }

    const DebugSymbol *sym2 = db.findSymbol(0x0200);
    CHECK(sym2 != nullptr, "loop found in SymbolDatabase");
    if (sym2) {
        CHECK(sym2->type == SymbolType::Label, "type = Label");
    }

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 6: Sync preserves source location from RDB properties
// ---------------------------------------------------------------------------

static void test_sync_source_location()
{
    TEST_BEGIN("sync preserves source location from RDB properties");

    RdbController rdb;
    rdb.initialize("vector06c", {});

    RdbObject obj;
    obj.address = 0x0252;
    obj.type = RdbObjectType::Function;
    obj.name = "_main";
    obj.properties["source_file"] = RdbPropertyValue::fromString("main.c");
    obj.properties["source_line"] = RdbPropertyValue::fromInt(109);
    rdb.addObject(obj);

    SymbolDatabase db;
    MapImport::syncRdbToSymbolDatabase(rdb, db);

    const DebugSymbol *sym = db.findSymbol(0x0252);
    CHECK(sym != nullptr, "symbol found");
    if (sym) {
        CHECK_STR("main.c", sym->sourceFile.c_str(), "sourceFile = main.c");
        CHECK_EQ(109, sym->sourceLine, "sourceLine = 109");
    }

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 7: Sync does not overwrite existing symbols
// ---------------------------------------------------------------------------

static void test_sync_no_overwrite()
{
    TEST_BEGIN("sync does not overwrite existing symbols");

    RdbController rdb;
    rdb.initialize("vector06c", {});

    RdbObject obj;
    obj.address = 0x0100;
    obj.type = RdbObjectType::Function;
    obj.name = "rdb_name";
    rdb.addObject(obj);

    SymbolDatabase db;
    // Pre-populate SymbolDatabase
    db.addSymbol(0x0100, "user_name", SymbolType::Function);

    int synced = MapImport::syncRdbToSymbolDatabase(rdb, db);
    CHECK_EQ(0, synced, "0 symbols synced (address already taken)");

    const DebugSymbol *sym = db.findSymbol(0x0100);
    CHECK(sym != nullptr, "symbol exists");
    if (sym) {
        CHECK_STR("user_name", sym->name.c_str(), "user_name preserved");
    }

    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 8: Full round-trip MAP → RDB → SymbolDatabase
// ---------------------------------------------------------------------------

static void test_full_roundtrip()
{
    TEST_BEGIN("full round-trip: MAP → RDB → SymbolDatabase");

    auto mapResult = parseMap(
        "_main   = $0252 ; addr, public, main.c::main::0::1:109\n"
        "_helper = $0300 ; addr, public\n"
        "loop1   = $0260 ; addr, local\n"
    );
    CHECK(mapResult.success, "MAP parsed");

    RdbController rdb;
    rdb.initialize("vector06c", {});

    int imported = MapImport::importMapToRdb(mapResult, rdb);
    CHECK_EQ(3, imported, "3 objects imported to RDB");

    SymbolDatabase db;
    int synced = MapImport::syncRdbToSymbolDatabase(rdb, db);
    CHECK_EQ(3, synced, "3 symbols synced to SymbolDatabase");
    CHECK_EQ(3, (int)db.symbolCount(), "SymbolDatabase has 3 symbols");

    // Verify Function classification
    const DebugSymbol *mainSym = db.findSymbol(0x0252);
    CHECK(mainSym != nullptr, "_main found");
    if (mainSym) {
        CHECK(mainSym->type == SymbolType::Function, "_main is Function");
        CHECK_STR("main.c", mainSym->sourceFile.c_str(), "sourceFile preserved");
        CHECK_EQ(109, mainSym->sourceLine, "sourceLine preserved");
    }

    // Verify Label classification
    const DebugSymbol *loopSym = db.findSymbol(0x0260);
    CHECK(loopSym != nullptr, "loop1 found");
    if (loopSym) {
        CHECK(loopSym->type == SymbolType::Label, "loop1 is Label");
    }

    TEST_END();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    printf("MAP Import Tests (Stage 6.11):\n");
    printf("========================================\n");

    test_basic_import();
    test_source_location_import();
    test_no_overwrite();
    test_failed_map_imports_nothing();
    test_rdb_to_symboldb_sync();
    test_sync_source_location();
    test_sync_no_overwrite();
    test_full_roundtrip();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed (of %d)\n",
           tests_passed, tests_failed, tests_run);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
