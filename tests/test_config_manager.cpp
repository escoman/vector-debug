// test_config_manager.cpp — ConfigManager unit tests
//
// Tests RecentRoms deduplication, ordering, and save/load roundtrip.

#include "config_manager.h"

#include "imgui.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  [%2d] %-50s ", tests_run, name); \
        fflush(stdout); \
    } while (0)

#define PASS() \
    do { tests_passed++; printf("PASS\n"); } while (0)

#define FAIL(msg) \
    do { tests_failed++; printf("FAIL: %s\n", msg); } while (0)

#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char *kTestDir = "/tmp/test_config_manager";

static void removeDirRecursive(const std::string &path)
{
    std::string cmd = "rm -rf " + path;
    system(cmd.c_str());
}

static void createTestDir()
{
    removeDirRecursive(kTestDir);
    mkdir(kTestDir, 0755);
}

static ImGuiContext *createTestContext()
{
    ImGuiContext *ctx = ImGui::CreateContext();
    return ctx;
}

static void destroyTestContext(ImGuiContext *ctx)
{
    ImGui::DestroyContext(ctx);
}

// Create a fake ROM file so that stat() in loadFromFile() succeeds
static void createFakeRom(const std::string &path)
{
    // Ensure parent directories exist
    std::string dir;
    for (size_t i = 0; i < path.size(); ++i) {
        dir += path[i];
        if (path[i] == '/') {
            mkdir(dir.c_str(), 0755);
        }
    }
    std::ofstream f(path);
    f << "FAKE_ROM";
}

static std::string readFile(const std::string &path)
{
    std::ifstream f(path);
    if (!f.good()) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// Count occurrences of a substring in a string
static int countOccurrences(const std::string &haystack, const std::string &needle)
{
    int count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// Test data — real ROM paths from user's system
// ---------------------------------------------------------------------------

static const std::string kRiseout =
    "/tmp/test_config_manager/roms/riseout.rom";
static const std::string kDt2Lz =
    "/tmp/test_config_manager/roms/dt2_lz.rom";

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The exact scenario from the bug report:
// Add riseout, dt2_lz, riseout, riseout — check for duplicates.
static void test_sequentialAddsWithDuplicates()
{
    TEST("Sequential adds with duplicates (bug report scenario)");
    createTestDir();
    createFakeRom(kRiseout);
    createFakeRom(kDt2Lz);

    ImGuiContext *ctx = createTestContext();
    ConfigManager cm;
    cm.initialize(kTestDir);

    // Exact sequence from bug report
    cm.addRecentRom(kRiseout);
    cm.addRecentRom(kDt2Lz);
    cm.addRecentRom(kRiseout);
    cm.addRecentRom(kRiseout);

    const auto &roms = cm.getRecentRoms();

    // Expected: [riseout, dt2_lz] — riseout at front, dt2_lz second, no dupes
    CHECK(roms.size() == 2, "should have exactly 2 entries");
    CHECK(roms[0] == kRiseout, "first should be riseout.rom");
    CHECK(roms[1] == kDt2Lz, "second should be dt2_lz.rom");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

// addRecentRom must never produce duplicates in the in-memory list
static void test_noDuplicatesInMemory()
{
    TEST("No duplicates in memory after many identical adds");
    createTestDir();
    createFakeRom(kRiseout);

    ImGuiContext *ctx = createTestContext();
    ConfigManager cm;
    cm.initialize(kTestDir);

    for (int i = 0; i < 10; ++i) {
        cm.addRecentRom(kRiseout);
    }

    const auto &roms = cm.getRecentRoms();
    CHECK(roms.size() == 1, "should have exactly 1 entry after 10 identical adds");
    CHECK(roms[0] == kRiseout, "entry should be riseout.rom");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

// Save → load roundtrip must preserve order and not introduce duplicates
static void test_saveLoadRoundtrip()
{
    TEST("Save/load roundtrip preserves order, no duplicates");
    createTestDir();
    createFakeRom(kRiseout);
    createFakeRom(kDt2Lz);

    ImGuiContext *ctx = createTestContext();

    // Session 1: add ROMs and save
    {
        ConfigManager cm;
        cm.initialize(kTestDir);
        cm.addRecentRom(kRiseout);
        cm.addRecentRom(kDt2Lz);
        cm.addRecentRom(kRiseout);  // move riseout to front
        cm.shutdown();  // force save
    }

    // Session 2: load and verify
    {
        ConfigManager cm;
        cm.initialize(kTestDir);
        const auto &roms = cm.getRecentRoms();

        CHECK(roms.size() == 2, "should have 2 entries after reload");
        CHECK(roms[0] == kRiseout, "first should be riseout.rom");
        CHECK(roms[1] == kDt2Lz, "second should be dt2_lz.rom");
    }

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

// Config file must not contain duplicate entries
static void test_noDuplicatesInConfigFile()
{
    TEST("Config file has no duplicate entries");
    createTestDir();
    createFakeRom(kRiseout);
    createFakeRom(kDt2Lz);

    ImGuiContext *ctx = createTestContext();
    ConfigManager cm;
    cm.initialize(kTestDir);

    cm.addRecentRom(kRiseout);
    cm.addRecentRom(kDt2Lz);
    cm.addRecentRom(kRiseout);
    cm.addRecentRom(kRiseout);
    cm.shutdown();  // force save

    std::string content = readFile(std::string(kTestDir) + "/config.ini");

    // riseout.rom should appear exactly once in the RecentRoms line
    int riseoutCount = countOccurrences(content, "riseout.rom");
    CHECK(riseoutCount == 1, "riseout.rom should appear exactly once in config.ini");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

// Loading a config file with pre-existing duplicates must deduplicate
static void test_loadDeduplicatesExistingEntries()
{
    TEST("Loading config with pre-existing duplicates deduplicates");
    createTestDir();
    createFakeRom(kRiseout);
    createFakeRom(kDt2Lz);

    // Manually write a config.ini with duplicates (simulating old buggy state)
    {
        std::ofstream f(std::string(kTestDir) + "/config.ini");
        f << "[Config]\n";
        f << "RecentRoms=" << kRiseout << "|" << kRiseout << "|"
          << kRiseout << "|" << kDt2Lz << "\n";
    }

    ImGuiContext *ctx = createTestContext();
    ConfigManager cm;
    cm.initialize(kTestDir);

    const auto &roms = cm.getRecentRoms();
    CHECK(roms.size() == 2, "should deduplicate on load: expect 2 entries");
    CHECK(roms[0] == kRiseout, "first should be riseout.rom");
    CHECK(roms[1] == kDt2Lz, "second should be dt2_lz.rom");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

// After loading duplicates and adding a new ROM, old dupes must be gone
static void test_addAfterLoadedDuplicatesCleansUp()
{
    TEST("Add after loading duplicates cleans up completely");
    createTestDir();
    createFakeRom(kRiseout);
    createFakeRom(kDt2Lz);

    std::string thirdRom = "/tmp/test_config_manager/roms/third.rom";
    createFakeRom(thirdRom);

    // Config with 3 copies of riseout
    {
        std::ofstream f(std::string(kTestDir) + "/config.ini");
        f << "[Config]\n";
        f << "RecentRoms=" << kRiseout << "|" << kRiseout << "|"
          << kRiseout << "|" << kDt2Lz << "\n";
    }

    ImGuiContext *ctx = createTestContext();
    ConfigManager cm;
    cm.initialize(kTestDir);

    // Add a new ROM — should trigger full cleanup
    cm.addRecentRom(thirdRom);

    const auto &roms = cm.getRecentRoms();
    // Expected: [third, riseout, dt2_lz] — all unique
    CHECK(roms.size() == 3, "should have exactly 3 unique entries");
    CHECK(roms[0] == thirdRom, "first should be third.rom");
    CHECK(roms[1] == kRiseout, "second should be riseout.rom");
    CHECK(roms[2] == kDt2Lz, "third should be dt2_lz.rom");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

// Empty path must be ignored
static void test_emptyPathIgnored()
{
    TEST("Empty path is ignored");
    createTestDir();

    ImGuiContext *ctx = createTestContext();
    ConfigManager cm;
    cm.initialize(kTestDir);

    cm.addRecentRom("");
    cm.addRecentRom("");

    CHECK(cm.getRecentRoms().empty(), "empty paths should not be added");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

// MAX_RECENT_ROMS cap is respected
static void test_maxRecentRomsCap()
{
    TEST("MAX_RECENT_ROMS cap is respected");
    createTestDir();

    // Create 12 fake ROMs
    std::vector<std::string> paths;
    for (int i = 0; i < 12; ++i) {
        std::string path = "/tmp/test_config_manager/roms/rom" +
                           std::to_string(i) + ".rom";
        createFakeRom(path);
        paths.push_back(path);
    }

    ImGuiContext *ctx = createTestContext();
    ConfigManager cm;
    cm.initialize(kTestDir);

    for (const auto &p : paths) {
        cm.addRecentRom(p);
    }

    const auto &roms = cm.getRecentRoms();
    CHECK(static_cast<int>(roms.size()) == ConfigManager::MAX_RECENT_ROMS,
          "should cap at MAX_RECENT_ROMS");

    // Most recent should be first
    CHECK(roms[0] == paths[11], "most recent ROM should be first");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("=== ConfigManager tests ===\n\n");

    test_sequentialAddsWithDuplicates();
    test_noDuplicatesInMemory();
    test_saveLoadRoundtrip();
    test_noDuplicatesInConfigFile();
    test_loadDeduplicatesExistingEntries();
    test_addAfterLoadedDuplicatesCleansUp();
    test_emptyPathIgnored();
    test_maxRecentRomsCap();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
