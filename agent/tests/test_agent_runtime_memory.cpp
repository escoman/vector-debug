// Agent Runtime Memory Analysis tests — Stage 6.20
//
// Unit tests for Runtime Memory Access Map, Access Log, and Memory Snapshots
// using MockAgentBackend.  No Board, SDL, or emulator dependency.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "agent_api.h"
#include "mock_backend_for_agent.h"

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
// Tests: Runtime Memory Access Map
// ---------------------------------------------------------------------------

static void test_access_map_initial_empty()
{
    TEST_BEGIN("access map: initial state all inactive");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getMemoryAccessMap();
    CHECK(r.success, "getMemoryAccessMap succeeds");
    CHECK_EQ(256u, (unsigned)r.value.size(), "256 blocks returned");

    // All blocks should be inactive
    bool allInactive = true;
    for (const auto &blk : r.value) {
        if (blk.read || blk.write || blk.fetch ||
            blk.read_count != 0 || blk.write_count != 0 || blk.fetch_count != 0) {
            allInactive = false;
            break;
        }
    }
    CHECK(allInactive, "all blocks initially inactive");

    // Verify block addresses are correct
    CHECK_EQ(0x0000u, (unsigned)r.value[0].address, "block 0 addr");
    CHECK_EQ(0x0100u, (unsigned)r.value[1].address, "block 1 addr");
    CHECK_EQ(0xFF00u, (unsigned)r.value[255].address, "block 255 addr");
    TEST_END();
}

static void test_access_map_read_tracking()
{
    TEST_BEGIN("access map: read tracking with counters");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Simulate 3 reads in block 0x10 (0x1000..0x10FF)
    mock.simulateRuntimeAccess(0x1000, RuntimeAccessLogEntry::Read, 0x0100, 0x42);
    mock.simulateRuntimeAccess(0x1050, RuntimeAccessLogEntry::Read, 0x0103, 0x55);
    mock.simulateRuntimeAccess(0x10FF, RuntimeAccessLogEntry::Read, 0x0105, 0xAA);

    auto r = api.getMemoryAccessMap();
    CHECK(r.success, "getMemoryAccessMap succeeds");

    // Block 0x10 (index 0x10) should have read=true, count=3
    const auto &blk = r.value[0x10];
    CHECK(blk.read, "block 0x10 read flag set");
    CHECK(!blk.write, "block 0x10 write flag not set");
    CHECK(!blk.fetch, "block 0x10 fetch flag not set");
    CHECK_EQ(3u, (unsigned)blk.read_count, "block 0x10 read count = 3");
    CHECK_EQ(0u, (unsigned)blk.write_count, "block 0x10 write count = 0");
    CHECK_EQ(0u, (unsigned)blk.fetch_count, "block 0x10 fetch count = 0");
    TEST_END();
}

static void test_access_map_write_tracking()
{
    TEST_BEGIN("access map: write tracking with counters");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Simulate writes to block 0xC0 (VRAM area)
    mock.simulateRuntimeAccess(0xC000, RuntimeAccessLogEntry::Write, 0x0206, 0x42);
    mock.simulateRuntimeAccess(0xC001, RuntimeAccessLogEntry::Write, 0x0206, 0x00);

    auto r = api.getMemoryAccessMap();
    CHECK(r.success, "getMemoryAccessMap succeeds");

    const auto &blk = r.value[0xC0];
    CHECK(!blk.read, "block 0xC0 read flag not set");
    CHECK(blk.write, "block 0xC0 write flag set");
    CHECK_EQ(2u, (unsigned)blk.write_count, "block 0xC0 write count = 2");
    TEST_END();
}

static void test_access_map_fetch_tracking()
{
    TEST_BEGIN("access map: fetch tracking with counters");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Simulate instruction fetches at block 0x01 (0x0100..0x01FF)
    mock.simulateRuntimeAccess(0x0100, RuntimeAccessLogEntry::Fetch, 0x0100, 0x31);
    mock.simulateRuntimeAccess(0x0103, RuntimeAccessLogEntry::Fetch, 0x0103, 0x3E);

    auto r = api.getMemoryAccessMap();
    CHECK(r.success, "getMemoryAccessMap succeeds");

    const auto &blk = r.value[0x01];
    CHECK(blk.fetch, "block 0x01 fetch flag set");
    CHECK_EQ(2u, (unsigned)blk.fetch_count, "block 0x01 fetch count = 2");
    TEST_END();
}

static void test_access_map_mixed_types()
{
    TEST_BEGIN("access map: mixed read/write/fetch in same block");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Same 256-byte block: read + write + fetch
    mock.simulateRuntimeAccess(0x2000, RuntimeAccessLogEntry::Read, 0x0100, 0x00);
    mock.simulateRuntimeAccess(0x2010, RuntimeAccessLogEntry::Write, 0x0103, 0xFF);
    mock.simulateRuntimeAccess(0x2020, RuntimeAccessLogEntry::Fetch, 0x0105, 0x00);
    mock.simulateRuntimeAccess(0x2030, RuntimeAccessLogEntry::Read, 0x0108, 0x00);

    auto r = api.getMemoryAccessMap();
    CHECK(r.success, "getMemoryAccessMap succeeds");

    const auto &blk = r.value[0x20];
    CHECK(blk.read, "block 0x20 read flag");
    CHECK(blk.write, "block 0x20 write flag");
    CHECK(blk.fetch, "block 0x20 fetch flag");
    CHECK_EQ(2u, (unsigned)blk.read_count, "read count = 2");
    CHECK_EQ(1u, (unsigned)blk.write_count, "write count = 1");
    CHECK_EQ(1u, (unsigned)blk.fetch_count, "fetch count = 1");
    TEST_END();
}

static void test_access_map_clear()
{
    TEST_BEGIN("access map: clear resets all blocks");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Populate some accesses
    mock.simulateRuntimeAccess(0x1000, RuntimeAccessLogEntry::Read, 0x0100, 0x42);
    mock.simulateRuntimeAccess(0xC000, RuntimeAccessLogEntry::Write, 0x0206, 0x55);
    mock.simulateRuntimeAccess(0x0100, RuntimeAccessLogEntry::Fetch, 0x0100, 0x31);

    // Verify non-empty
    auto r1 = api.getMemoryAccessMap();
    CHECK(r1.value[0x10].read, "block 0x10 has reads before clear");
    CHECK(r1.value[0xC0].write, "block 0xC0 has writes before clear");

    // Clear
    auto clearResult = api.clearMemoryAccessMap();
    CHECK(clearResult.success, "clearMemoryAccessMap succeeds");

    // Verify all cleared
    auto r2 = api.getMemoryAccessMap();
    CHECK(r2.success, "getMemoryAccessMap after clear succeeds");

    bool allInactive = true;
    for (const auto &blk : r2.value) {
        if (blk.read || blk.write || blk.fetch) {
            allInactive = false;
            break;
        }
    }
    CHECK(allInactive, "all blocks inactive after clear");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Runtime Memory Access Log
// ---------------------------------------------------------------------------

static void test_access_log_entries_created()
{
    TEST_BEGIN("access log: entries created with correct data");
    MockAgentBackend mock;
    AgentApi api(mock);

    mock.simulateRuntimeAccess(0x1000, RuntimeAccessLogEntry::Read, 0x0100, 0x42);
    mock.simulateRuntimeAccess(0xC050, RuntimeAccessLogEntry::Write, 0x0206, 0xFF);
    mock.simulateRuntimeAccess(0x0103, RuntimeAccessLogEntry::Fetch, 0x0103, 0x3E);

    auto r = api.getMemoryAccessLog(1000);
    CHECK(r.success, "getMemoryAccessLog succeeds");
    CHECK_EQ(3u, (unsigned)r.value.size(), "3 log entries");

    // Check first entry
    CHECK_EQ(0x1000u, (unsigned)r.value[0].address, "entry 0 address");
    CHECK_EQ((unsigned)RuntimeAccessLogEntry::Read, (unsigned)r.value[0].type, "entry 0 type = Read");
    CHECK_EQ(0x0100u, (unsigned)r.value[0].pc, "entry 0 PC");
    CHECK_EQ(0x42u, (unsigned)r.value[0].value, "entry 0 value");

    // Check second entry
    CHECK_EQ(0xC050u, (unsigned)r.value[1].address, "entry 1 address");
    CHECK_EQ((unsigned)RuntimeAccessLogEntry::Write, (unsigned)r.value[1].type, "entry 1 type = Write");

    // Check third entry
    CHECK_EQ(0x0103u, (unsigned)r.value[2].address, "entry 2 address");
    CHECK_EQ((unsigned)RuntimeAccessLogEntry::Fetch, (unsigned)r.value[2].type, "entry 2 type = Fetch");
    TEST_END();
}

static void test_access_log_max_entries()
{
    TEST_BEGIN("access log: max_entries limits returned count");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Create 10 entries
    for (int i = 0; i < 10; ++i) {
        mock.simulateRuntimeAccess(
            static_cast<uint16_t>(0x1000 + i),
            RuntimeAccessLogEntry::Read,
            static_cast<uint16_t>(0x0100 + i),
            static_cast<uint8_t>(i));
    }

    // Request only 5
    auto r = api.getMemoryAccessLog(5);
    CHECK(r.success, "getMemoryAccessLog(5) succeeds");
    CHECK_EQ(5u, (unsigned)r.value.size(), "returns 5 entries");

    // Should be the LAST 5 entries (most recent)
    CHECK_EQ(0x1005u, (unsigned)r.value[0].address, "first returned = entry 5");
    CHECK_EQ(0x1009u, (unsigned)r.value[4].address, "last returned = entry 9");
    TEST_END();
}

static void test_access_log_empty()
{
    TEST_BEGIN("access log: empty initially");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto r = api.getMemoryAccessLog(1000);
    CHECK(r.success, "getMemoryAccessLog succeeds");
    CHECK_EQ(0u, (unsigned)r.value.size(), "0 entries initially");
    TEST_END();
}

static void test_access_log_cleared_with_map()
{
    TEST_BEGIN("access log: cleared together with access map");
    MockAgentBackend mock;
    AgentApi api(mock);

    mock.simulateRuntimeAccess(0x1000, RuntimeAccessLogEntry::Read, 0x0100, 0x42);
    mock.simulateRuntimeAccess(0xC000, RuntimeAccessLogEntry::Write, 0x0206, 0xFF);

    auto r1 = api.getMemoryAccessLog(1000);
    CHECK_EQ(2u, (unsigned)r1.value.size(), "2 entries before clear");

    api.clearMemoryAccessMap();

    auto r2 = api.getMemoryAccessLog(1000);
    CHECK_EQ(0u, (unsigned)r2.value.size(), "0 entries after clear");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Memory Snapshots
// ---------------------------------------------------------------------------

static void test_snapshot_create_and_get()
{
    TEST_BEGIN("snapshot: create and retrieve");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Memory at 0x0100 has our test program (set by mock constructor)
    auto cr = api.createMemorySnapshot(0x0100, 9);
    CHECK(cr.success, "createMemorySnapshot succeeds");
    CHECK(cr.value != 0, "snapshot ID is non-zero");

    uint32_t snapId = cr.value;

    auto gr = api.getMemorySnapshot(snapId);
    CHECK(gr.success, "getMemorySnapshot succeeds");
    CHECK_EQ(snapId, (unsigned)gr.value.snapshot_id, "snapshot ID matches");
    CHECK_EQ(0x0100u, (unsigned)gr.value.start_address, "start address matches");
    CHECK_EQ(9u, (unsigned)gr.value.data.size(), "data size = 9");

    // Verify actual bytes (LXI SP,0xF800 = 31 00 F8)
    CHECK_EQ(0x31u, (unsigned)gr.value.data[0], "byte 0 = 0x31");
    CHECK_EQ(0x00u, (unsigned)gr.value.data[1], "byte 1 = 0x00");
    CHECK_EQ(0xF8u, (unsigned)gr.value.data[2], "byte 2 = 0xF8");
    TEST_END();
}

static void test_snapshot_full_64k()
{
    TEST_BEGIN("snapshot: full 64K address space");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto cr = api.createMemorySnapshot(0, 65536);
    CHECK(cr.success, "createMemorySnapshot(0, 65536) succeeds");

    auto gr = api.getMemorySnapshot(cr.value);
    CHECK(gr.success, "getMemorySnapshot succeeds");
    CHECK_EQ(65536u, (unsigned)gr.value.data.size(), "65536 bytes");
    TEST_END();
}

static void test_snapshot_invalid_range()
{
    TEST_BEGIN("snapshot: invalid range (overflow 64K)");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Start=0xFF00, size=257 → end = 0x10001 → overflow
    auto cr = api.createMemorySnapshot(0xFF00, 257);
    CHECK(!cr.success, "createMemorySnapshot(0xFF00, 257) fails");
    CHECK(cr.error_code == ErrorCode::InvalidRange, "error code = InvalidRange");
    TEST_END();
}

static void test_snapshot_not_found()
{
    TEST_BEGIN("snapshot: get non-existent ID fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto gr = api.getMemorySnapshot(9999);
    CHECK(!gr.success, "getMemorySnapshot(9999) fails");
    CHECK(gr.error_code == ErrorCode::NotFound, "error code = NotFound");
    TEST_END();
}

static void test_snapshot_compare_identical()
{
    TEST_BEGIN("snapshot: compare identical → no changes");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Create two snapshots of the same area without modifying memory
    auto cr1 = api.createMemorySnapshot(0x0100, 9);
    auto cr2 = api.createMemorySnapshot(0x0100, 9);
    CHECK(cr1.success, "snapshot 1 created");
    CHECK(cr2.success, "snapshot 2 created");

    auto dr = api.compareMemorySnapshots(cr1.value, cr2.value);
    CHECK(dr.success, "compare succeeds");
    CHECK_EQ(0u, (unsigned)dr.value.changed_ranges.size(), "0 changed ranges");
    TEST_END();
}

static void test_snapshot_compare_with_changes()
{
    TEST_BEGIN("snapshot: compare detects changed bytes");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Snapshot before modification
    auto cr1 = api.createMemorySnapshot(0x0100, 9);
    CHECK(cr1.success, "snapshot 1 created");

    // Modify some bytes
    std::vector<uint8_t> newData = {0xAA, 0xBB, 0xCC};
    mock.setMemory(0x0102, newData);  // change bytes at 0x0102..0x0104

    // Snapshot after modification
    auto cr2 = api.createMemorySnapshot(0x0100, 9);
    CHECK(cr2.success, "snapshot 2 created");

    auto dr = api.compareMemorySnapshots(cr1.value, cr2.value);
    CHECK(dr.success, "compare succeeds");
    CHECK_EQ(1u, (unsigned)dr.value.changed_ranges.size(), "1 changed range");

    // The range should be 0x0102..0x0104 (3 contiguous bytes)
    const auto &range = dr.value.changed_ranges[0];
    CHECK_EQ(0x0102u, (unsigned)range.address, "range starts at 0x0102");
    CHECK_EQ(3u, (unsigned)range.size, "range size = 3");
    TEST_END();
}

static void test_snapshot_compare_scattered_changes()
{
    TEST_BEGIN("snapshot: compare merges contiguous, separates gaps");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Snapshot a 16-byte region at 0x2000
    for (int i = 0; i < 16; ++i)
        mock.simulateRuntimeAccess(
            static_cast<uint16_t>(0x2000 + i),
            RuntimeAccessLogEntry::Read);  // just to use the area

    auto cr1 = api.createMemorySnapshot(0x2000, 16);
    CHECK(cr1.success, "snapshot 1 created");

    // Modify bytes: [0..1] changed, [2] same, [3..5] changed, rest same
    std::vector<uint8_t> mods = {0xFF, 0xFF};
    mock.setMemory(0x2000, mods);           // bytes 0,1
    mock.setMemory(0x2003, {0xAA, 0xBB, 0xCC});  // bytes 3,4,5

    auto cr2 = api.createMemorySnapshot(0x2000, 16);
    CHECK(cr2.success, "snapshot 2 created");

    auto dr = api.compareMemorySnapshots(cr1.value, cr2.value);
    CHECK(dr.success, "compare succeeds");
    CHECK_EQ(2u, (unsigned)dr.value.changed_ranges.size(), "2 changed ranges");

    // First range: 0x2000, size 2
    CHECK_EQ(0x2000u, (unsigned)dr.value.changed_ranges[0].address, "range 0 at 0x2000");
    CHECK_EQ(2u, (unsigned)dr.value.changed_ranges[0].size, "range 0 size = 2");

    // Second range: 0x2003, size 3
    CHECK_EQ(0x2003u, (unsigned)dr.value.changed_ranges[1].address, "range 1 at 0x2003");
    CHECK_EQ(3u, (unsigned)dr.value.changed_ranges[1].size, "range 1 size = 3");
    TEST_END();
}

static void test_snapshot_compare_invalid_id()
{
    TEST_BEGIN("snapshot: compare with invalid ID fails");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto cr = api.createMemorySnapshot(0x0100, 9);
    CHECK(cr.success, "snapshot created");

    auto dr = api.compareMemorySnapshots(cr.value, 9999);
    CHECK(!dr.success, "compare with invalid ID fails");
    CHECK(dr.error_code == ErrorCode::NotFound, "error code = NotFound");
    TEST_END();
}

static void test_snapshot_multiple_ids()
{
    TEST_BEGIN("snapshot: multiple snapshots get unique IDs");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto cr1 = api.createMemorySnapshot(0x0000, 256);
    auto cr2 = api.createMemorySnapshot(0x0100, 256);
    auto cr3 = api.createMemorySnapshot(0xC000, 256);

    CHECK(cr1.success, "snapshot 1 created");
    CHECK(cr2.success, "snapshot 2 created");
    CHECK(cr3.success, "snapshot 3 created");

    CHECK(cr1.value != cr2.value, "ID 1 != ID 2");
    CHECK(cr2.value != cr3.value, "ID 2 != ID 3");
    CHECK(cr1.value != cr3.value, "ID 1 != ID 3");

    // All retrievable
    CHECK(api.getMemorySnapshot(cr1.value).success, "get snapshot 1");
    CHECK(api.getMemorySnapshot(cr2.value).success, "get snapshot 2");
    CHECK(api.getMemorySnapshot(cr3.value).success, "get snapshot 3");
    TEST_END();
}

static void test_snapshot_single_byte()
{
    TEST_BEGIN("snapshot: single byte snapshot");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto cr = api.createMemorySnapshot(0x0100, 1);
    CHECK(cr.success, "create 1-byte snapshot");

    auto gr = api.getMemorySnapshot(cr.value);
    CHECK(gr.success, "get snapshot");
    CHECK_EQ(1u, (unsigned)gr.value.data.size(), "size = 1");
    CHECK_EQ(0x31u, (unsigned)gr.value.data[0], "byte = 0x31");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Snapshot invalidation (ROM isolation)
// ---------------------------------------------------------------------------

static void test_snapshot_invalidation()
{
    TEST_BEGIN("snapshot: invalidateAll clears all snapshots");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto cr1 = api.createMemorySnapshot(0x0100, 9);
    auto cr2 = api.createMemorySnapshot(0xC000, 256);
    CHECK(cr1.success && cr2.success, "snapshots created");

    // Directly call backend invalidation (simulates loadRom behavior)
    mock.invalidateAllSnapshots();

    auto gr1 = api.getMemorySnapshot(cr1.value);
    auto gr2 = api.getMemorySnapshot(cr2.value);
    CHECK(!gr1.success, "snapshot 1 invalidated");
    CHECK(!gr2.success, "snapshot 2 invalidated");
    CHECK(gr1.error_code == ErrorCode::NotFound, "snapshot 1 → NotFound");
    CHECK(gr2.error_code == ErrorCode::NotFound, "snapshot 2 → NotFound");
    TEST_END();
}

static void test_snapshot_invalidate_then_recreate()
{
    TEST_BEGIN("snapshot: can create after invalidation");
    MockAgentBackend mock;
    AgentApi api(mock);

    auto cr1 = api.createMemorySnapshot(0x0100, 9);
    CHECK(cr1.success, "first snapshot created");

    mock.invalidateAllSnapshots();

    auto cr2 = api.createMemorySnapshot(0x0100, 9);
    CHECK(cr2.success, "new snapshot after invalidation");
    CHECK(cr2.value != cr1.value, "new ID assigned");

    auto gr = api.getMemorySnapshot(cr2.value);
    CHECK(gr.success, "new snapshot retrievable");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Access map + log together (workflow scenario)
// ---------------------------------------------------------------------------

static void test_workflow_observe_and_snapshot()
{
    TEST_BEGIN("workflow: observe access map → snapshot → modify → compare");
    MockAgentBackend mock;
    AgentApi api(mock);

    // Phase 1: Simulate runtime accesses
    mock.simulateRuntimeAccess(0x0100, RuntimeAccessLogEntry::Fetch, 0x0100, 0x31);
    mock.simulateRuntimeAccess(0xC000, RuntimeAccessLogEntry::Write, 0x0206, 0x42);
    mock.simulateRuntimeAccess(0xC001, RuntimeAccessLogEntry::Write, 0x0206, 0x00);

    // Phase 2: Check access map shows activity
    auto mapR = api.getMemoryAccessMap();
    CHECK(mapR.value[0x01].fetch, "code block fetched");
    CHECK(mapR.value[0xC0].write, "VRAM block written");

    // Phase 3: Take snapshot of VRAM
    auto snap1 = api.createMemorySnapshot(0xC000, 256);
    CHECK(snap1.success, "VRAM snapshot 1 created");

    // Phase 4: More runtime activity (modify VRAM)
    mock.simulateRuntimeAccess(0xC000, RuntimeAccessLogEntry::Write, 0x0206, 0xFF);
    mock.writeMemoryByte(0xC000, 0xFF);

    // Phase 5: Take second snapshot
    auto snap2 = api.createMemorySnapshot(0xC000, 256);
    CHECK(snap2.success, "VRAM snapshot 2 created");

    // Phase 6: Compare
    auto diff = api.compareMemorySnapshots(snap1.value, snap2.value);
    CHECK(diff.success, "compare succeeds");
    CHECK(diff.value.changed_ranges.size() >= 1, "at least 1 changed range");
    CHECK_EQ(0xC000u, (unsigned)diff.value.changed_ranges[0].address, "change at 0xC000");

    // Phase 7: Check log recorded all accesses
    auto logR = api.getMemoryAccessLog(1000);
    CHECK(logR.success, "log retrieved");
    CHECK_EQ(4u, (unsigned)logR.value.size(), "4 total log entries");

    // Phase 8: Clear and verify
    api.clearMemoryAccessMap();
    auto mapAfter = api.getMemoryAccessMap();
    bool allClear = true;
    for (const auto &b : mapAfter.value) {
        if (b.read || b.write || b.fetch) { allClear = false; break; }
    }
    CHECK(allClear, "map cleared");
    auto logAfter = api.getMemoryAccessLog(1000);
    CHECK_EQ(0u, (unsigned)logAfter.value.size(), "log cleared");
    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("\033[1;33m========================================\n");
    printf("  Stage 6.20: Runtime Memory Analysis Tests\n");
    printf("========================================\033[0m\n");

    // Access Map tests
    test_access_map_initial_empty();
    test_access_map_read_tracking();
    test_access_map_write_tracking();
    test_access_map_fetch_tracking();
    test_access_map_mixed_types();
    test_access_map_clear();

    // Access Log tests
    test_access_log_entries_created();
    test_access_log_max_entries();
    test_access_log_empty();
    test_access_log_cleared_with_map();

    // Snapshot tests
    test_snapshot_create_and_get();
    test_snapshot_full_64k();
    test_snapshot_invalid_range();
    test_snapshot_not_found();
    test_snapshot_compare_identical();
    test_snapshot_compare_with_changes();
    test_snapshot_compare_scattered_changes();
    test_snapshot_compare_invalid_id();
    test_snapshot_multiple_ids();
    test_snapshot_single_byte();

    // Invalidation tests
    test_snapshot_invalidation();
    test_snapshot_invalidate_then_recreate();

    // Workflow test
    test_workflow_observe_and_snapshot();

    printf("\n\033[1;33m========================================\n");
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", \033[1;31m%d FAILED", tests_failed);
    }
    printf("\n========================================\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
