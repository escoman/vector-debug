// RDB Save Symbol Sync tests — Stage 6.20.2
//
// Regression tests verifying that debug_rename_function and
// debug_set_function_comment also update the RDB so that
// debug_save_rdb persists the changes to disk.
//
// Root cause: RenameSymbol and SetComment command handlers only updated
// SymbolDatabase but not the RDB. Since CreateFunction creates BOTH
// a symbol and an RDB object, subsequent rename/comment operations
// left the RDB unchanged. saveRdb() saw dirty=false and wrote nothing.
//
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

#define CHECK_STR(exp, act, msg) \
        do { \
            const char *_e = (exp); \
            const char *_a = (act); \
            if (strcmp(_e, _a) != 0) { \
                printf("  \033[41;97m FAIL \033[0m %s: expected \"%s\", got \"%s\" (line %d)\n", \
                       msg, _e, _a, __LINE__); \
                _test_ok = false; \
            } else { \
                printf("  \033[46;30m ok \033[0m %s = \"%s\"\n", msg, _a); \
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

static const char *TEMP_ROM = "/tmp/test_rdb_sync.rom";

static void writeTempRom(const char *path)
{
    // Simple test ROM: NOP NOP NOP HLT
    uint8_t data[] = { 0x00, 0x00, 0x00, 0x76 };
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data), sizeof(data));
    f.close();
}

static void cleanupFiles()
{
    std::remove(TEMP_ROM);
    // Remove .rdb that may have been created
    std::string rdbPath = std::string(TEMP_ROM).substr(0, std::string(TEMP_ROM).rfind('.')) + ".rdb";
    std::remove(rdbPath.c_str());
}

// ---------------------------------------------------------------------------
// Test fixture
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

    // Get RDB object name at address (empty string if not found)
    std::string rdbObjectName(uint16_t addr) {
        auto r = api->getRdbObject(addr);
        if (!r.success) return "";
        return r.value.name;
    }

    // Get RDB object comment at address (empty string if not found)
    std::string rdbObjectComment(uint16_t addr) {
        auto r = api->getRdbObject(addr);
        if (!r.success) return "";
        return r.value.comment;
    }

    bool rdbIsDirty() {
        auto r = api->getRdbInfo();
        return r.success && r.value.dirty;
    }
};

// ---------------------------------------------------------------------------
// Test 1: rename_function updates RDB object name
// ---------------------------------------------------------------------------

static void test_rename_updates_rdb()
{
    TEST_BEGIN("rename_function updates RDB object name");

    cleanupFiles();
    writeTempRom(TEMP_ROM);

    Fixture f;
    auto lr = f.api->loadRom(TEMP_ROM, 0);
    CHECK(lr.success, "ROM loaded");

    // Create function at 0x0100 — creates both symbol + RDB object
    auto cr = f.api->createFunction(0x0100, 0);
    CHECK(cr.success, "function created");

    // RDB object should have auto-generated name
    std::string autoName = f.rdbObjectName(0x0100);
    CHECK(!autoName.empty(), "RDB object has auto-name");

    // Rename function
    auto rr = f.api->renameFunction(0x0100, "my_func");
    CHECK(rr.success, "function renamed");

    // RDB object name should be updated
    std::string rdbName = f.rdbObjectName(0x0100);
    CHECK_STR("my_func", rdbName.c_str(), "RDB name updated after rename");

    // RDB should be dirty
    CHECK(f.rdbIsDirty(), "RDB is dirty after rename");

    cleanupFiles();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 2: set_function_comment updates RDB object comment
// ---------------------------------------------------------------------------

static void test_comment_updates_rdb()
{
    TEST_BEGIN("set_function_comment updates RDB object comment");

    cleanupFiles();
    writeTempRom(TEMP_ROM);

    Fixture f;
    auto lr = f.api->loadRom(TEMP_ROM, 0);
    CHECK(lr.success, "ROM loaded");

    // Create function
    auto cr = f.api->createFunction(0x0200, 0);
    CHECK(cr.success, "function created");

    // Set comment
    auto sr = f.api->setFunctionComment(0x0200, "Main loop");
    CHECK(sr.success, "comment set");

    // RDB object comment should be updated
    std::string rdbComment = f.rdbObjectComment(0x0200);
    CHECK_STR("Main loop", rdbComment.c_str(), "RDB comment updated");

    // RDB should be dirty
    CHECK(f.rdbIsDirty(), "RDB is dirty after comment");

    cleanupFiles();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 3: rename + comment → save → file contains updated data
// ---------------------------------------------------------------------------

static void test_save_after_rename_and_comment()
{
    TEST_BEGIN("save_rdb persists rename and comment");

    cleanupFiles();
    writeTempRom(TEMP_ROM);

    Fixture f;
    auto lr = f.api->loadRom(TEMP_ROM, 0);
    CHECK(lr.success, "ROM loaded");

    // Create function
    auto cr = f.api->createFunction(0x0300, 0);
    CHECK(cr.success, "function created");

    // Save (creates file with auto-name)
    auto sv1 = f.api->saveRdb();
    CHECK(sv1.success, "first save succeeded");
    CHECK(!f.rdbIsDirty(), "RDB clean after first save");

    // Rename + set comment
    f.api->renameFunction(0x0300, "updated_name");
    f.api->setFunctionComment(0x0300, "Updated comment");

    // RDB should be dirty
    CHECK(f.rdbIsDirty(), "RDB dirty after rename+comment");

    // Save again
    auto sv2 = f.api->saveRdb();
    CHECK(sv2.success, "second save succeeded");
    CHECK(!f.rdbIsDirty(), "RDB clean after second save");

    // Verify RDB object has updated data
    std::string rdbName = f.rdbObjectName(0x0300);
    std::string rdbComment = f.rdbObjectComment(0x0300);
    CHECK_STR("updated_name", rdbName.c_str(), "RDB name persisted");
    CHECK_STR("Updated comment", rdbComment.c_str(), "RDB comment persisted");

    // Verify file on disk contains updated data
    std::string rdbPath = std::string(TEMP_ROM).substr(0, std::string(TEMP_ROM).rfind('.')) + ".rdb";
    std::ifstream rdbFile(rdbPath);
    CHECK(rdbFile.is_open(), "RDB file exists");
    std::string content((std::istreambuf_iterator<char>(rdbFile)),
                         std::istreambuf_iterator<char>());
    CHECK(content.find("updated_name") != std::string::npos,
          "file contains updated name");
    CHECK(content.find("Updated comment") != std::string::npos,
          "file contains updated comment");

    cleanupFiles();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 4: rename without RDB object does not crash
// ---------------------------------------------------------------------------

static void test_rename_without_rdb_object()
{
    TEST_BEGIN("rename without RDB object does not crash");

    cleanupFiles();
    writeTempRom(TEMP_ROM);

    Fixture f;
    auto lr = f.api->loadRom(TEMP_ROM, 0);
    CHECK(lr.success, "ROM loaded");

    // Manually add a symbol to SymbolDatabase (without RDB object)
    // by using createFunction then removing the RDB object
    auto cr = f.api->createFunction(0x0400, 0);
    CHECK(cr.success, "function created");

    // Remove RDB object
    auto rm = f.api->removeRdbObject(0x0400);
    CHECK(rm.success, "RDB object removed");

    // Save to clear dirty flag
    f.api->saveRdb();
    CHECK(!f.rdbIsDirty(), "RDB clean after save");

    // Rename should still work (symbol exists, RDB object doesn't)
    auto rr = f.api->renameFunction(0x0400, "new_name");
    CHECK(rr.success, "rename succeeded without RDB object");

    // RDB should NOT be dirty (no RDB object to update)
    CHECK(!f.rdbIsDirty(), "RDB not dirty (no RDB object to update)");

    cleanupFiles();
    TEST_END();
}

// ---------------------------------------------------------------------------
// Test 5: set_comment without RDB object does not crash
// ---------------------------------------------------------------------------

static void test_comment_without_rdb_object()
{
    TEST_BEGIN("set_comment without RDB object does not crash");

    cleanupFiles();
    writeTempRom(TEMP_ROM);

    Fixture f;
    auto lr = f.api->loadRom(TEMP_ROM, 0);
    CHECK(lr.success, "ROM loaded");

    // Create function then remove RDB object
    auto cr = f.api->createFunction(0x0500, 0);
    CHECK(cr.success, "function created");

    auto rm = f.api->removeRdbObject(0x0500);
    CHECK(rm.success, "RDB object removed");

    // Save to clear dirty flag
    f.api->saveRdb();
    CHECK(!f.rdbIsDirty(), "RDB clean after save");

    // Set comment should still work
    auto sr = f.api->setFunctionComment(0x0500, "Some comment");
    CHECK(sr.success, "comment set without RDB object");

    // RDB should NOT be dirty
    CHECK(!f.rdbIsDirty(), "RDB not dirty (no RDB object to update)");

    cleanupFiles();
    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("RDB Save Symbol Sync Tests (Stage 6.20.2):\n");

    test_rename_updates_rdb();
    test_comment_updates_rdb();
    test_save_after_rename_and_comment();
    test_rename_without_rdb_object();
    test_comment_without_rdb_object();

    printf("\n========================================\n");
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf("\n========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
