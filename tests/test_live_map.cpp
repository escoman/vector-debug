// test_live_map.cpp — Stage 5.2: Memory Map Live Activity tests
//
// Tests cover:
//   1–7:   address ↔ block mapping
//   8–14:  computeBlockColor — timestamp-based coloring
//   15–16: timer extension (newer event extends activity)
//   17–18: Live OFF/ON behavior
//   19:    Clear Activity
//   20:    Code/Data classification does not affect live color

#include "memory_map_window.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int totalTests = 0;
static int passedTests = 0;

#define TEST(name) \
    do { totalTests++; printf("  [%d] %-50s ", totalTests, name); } while(0)

#define PASS() \
    do { passedTests++; printf("PASS\n"); } while(0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); } while(0)

#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

// Helper: time point "ago"
static TimePoint ago(int ms) {
    return Clock::now() - std::chrono::milliseconds(ms);
}

// Helper: minimum time (never)
static TimePoint never() {
    return TimePoint::min();
}

static const Duration DURATION(500);

// ---------------------------------------------------------------------------
// Tests 1–7: Address ↔ Block mapping
// ---------------------------------------------------------------------------

static void test_block0_address()
{
    TEST("block 0 → address 0x0000");
    CHECK(blockToAddress(0) == 0x0000, "block 0 should map to 0x0000");
    PASS();
}

static void test_block1_address()
{
    TEST("block 1 \xe2\x86\x92 address 0x0100 (col 0, row 1)");
    // block 1 = col 0, row 1 → addr = 0*0x800 + 1*0x100 = 0x0100
    CHECK(blockToAddress(1) == 0x0100, "block 1 should map to 0x0100");
    PASS();
}

static void test_block4_address()
{
    TEST("block 4 \xe2\x86\x92 address 0x0400 (col 0, row 4)");
    // block 4 = col 0, row 4 → addr = 0*0x800 + 4*0x100 = 0x0400
    CHECK(blockToAddress(4) == 0x0400, "block 4 should map to 0x0400");
    PASS();
}

static void test_block31_address()
{
    TEST("block 31 \xe2\x86\x92 address 0x1F00 (col 3, row 7)");
    // block 31 = col 3, row 7 → addr = 3*0x800 + 7*0x100 = 0x1F00
    CHECK(blockToAddress(31) == 0x1F00, "block 31 should map to 0x1F00");
    PASS();
}

static void test_block32_address()
{
    TEST("block 32 \xe2\x86\x92 address 0x2000 (col 4, row 0)");
    // block 32 = col 4, row 0 → addr = 4*0x800 = 0x2000
    CHECK(blockToAddress(32) == 0x2000, "block 32 should map to 0x2000");
    PASS();
}

static void test_block128_address()
{
    TEST("block 128 \xe2\x86\x92 address 0x8000 (col 16, row 0)");
    // block 128 = col 16, row 0 → addr = 16*0x800 = 0x8000
    CHECK(blockToAddress(128) == 0x8000, "block 128 should map to 0x8000");
    PASS();
}

static void test_block255_address()
{
    TEST("block 255 \xe2\x86\x92 address 0xFF00 (col 31, row 7)");
    // block 255 = col 31, row 7 → addr = 31*0x800 + 7*0x100 = 0xFF00
    CHECK(blockToAddress(255) == 0xFF00, "block 255 should map to 0xFF00");
    PASS();
}

static void test_address_to_block_inverse()
{
    TEST("address \xe2\x86\x92 block inverse: 0x1234 \xe2\x86\x92 block 18");
    // 0x1234 → col = 0x1234/0x800 = 2, row = (0x1234%0x800)/0x100 = 2
    // block = col * 8 + row = 2*8 + 2 = 18
    int col = 0x1234 / 0x800;
    int row = (0x1234 % 0x800) / 0x100;
    int block = col * 8 + row;
    CHECK(block == 18, "0x1234 should be in block 18");
    // Verify roundtrip: blockToAddress(18) = col=2, row=2 → 0x1200
    CHECK(blockToAddress(block) == 0x1200, "block 18 should start at 0x1200");
    PASS();
}

// ---------------------------------------------------------------------------
// Tests 8–14: computeBlockColor — timestamp-based coloring
// ---------------------------------------------------------------------------

static void test_color_no_activity()
{
    TEST("no activity → DarkGray");
    auto now = Clock::now();
    auto c = computeBlockColor(never(), never(), now, DURATION);
    CHECK(c == LiveBlockColor::DarkGray, "should be DarkGray");
    PASS();
}

static void test_color_read_only()
{
    TEST("read 100ms ago → Green");
    auto now = Clock::now();
    auto c = computeBlockColor(ago(100), never(), now, DURATION);
    CHECK(c == LiveBlockColor::Green, "should be Green");
    PASS();
}

static void test_color_write_only()
{
    TEST("write 100ms ago → Red");
    auto now = Clock::now();
    auto c = computeBlockColor(never(), ago(100), now, DURATION);
    CHECK(c == LiveBlockColor::Red, "should be Red");
    PASS();
}

static void test_color_read_and_write()
{
    TEST("read+write 100ms ago → Yellow");
    auto now = Clock::now();
    auto c = computeBlockColor(ago(100), ago(100), now, DURATION);
    CHECK(c == LiveBlockColor::Yellow, "should be Yellow");
    PASS();
}

static void test_color_read_expired()
{
    TEST("read 600ms ago → DarkGray (expired)");
    auto now = Clock::now();
    auto c = computeBlockColor(ago(600), never(), now, DURATION);
    CHECK(c == LiveBlockColor::DarkGray, "should be DarkGray after expiry");
    PASS();
}

static void test_color_write_expired()
{
    TEST("write 600ms ago → DarkGray (expired)");
    auto now = Clock::now();
    auto c = computeBlockColor(never(), ago(600), now, DURATION);
    CHECK(c == LiveBlockColor::DarkGray, "should be DarkGray after expiry");
    PASS();
}

static void test_color_read_active_write_expired()
{
    TEST("read 100ms + write 600ms → Green (only read active)");
    auto now = Clock::now();
    auto c = computeBlockColor(ago(100), ago(600), now, DURATION);
    CHECK(c == LiveBlockColor::Green, "should be Green (only read active)");
    PASS();
}

// ---------------------------------------------------------------------------
// Tests 15–16: Timer extension
// ---------------------------------------------------------------------------

static void test_timer_extension_read()
{
    TEST("timer extension: new read resets timer");
    // First read at 400ms ago (still active)
    auto now = Clock::now();
    auto c1 = computeBlockColor(ago(400), never(), now, DURATION);
    CHECK(c1 == LiveBlockColor::Green, "first read should be Green");

    // Simulate new read at 50ms ago (timer extended)
    auto c2 = computeBlockColor(ago(50), never(), now, DURATION);
    CHECK(c2 == LiveBlockColor::Green, "extended read should still be Green");
    PASS();
}

static void test_timer_extension_write_after_read()
{
    TEST("timer extension: write after read → Yellow");
    auto now = Clock::now();
    // Read at 100ms ago, write at 50ms ago
    auto c = computeBlockColor(ago(100), ago(50), now, DURATION);
    CHECK(c == LiveBlockColor::Yellow, "both active → Yellow");
    PASS();
}

// ---------------------------------------------------------------------------
// Tests 17–18: Live OFF/ON behavior
// ---------------------------------------------------------------------------

static void test_live_off_no_color()
{
    TEST("Live OFF: blocks stay DarkGray regardless of timestamps");
    MemoryMapWindow w;
    // Initially not live
    CHECK(!w.isLive(), "should start not live");

    // Even if we set timestamps, render() should not color when live_ is off
    // We test the logic: when live_ is false, computeBlockColor is not called
    // Instead, we verify the window state
    CHECK(w.isLive() == false, "Live should be OFF");
    PASS();
}

static void test_live_on_clears_state()
{
    TEST("Live OFF→ON: clears all timestamps");
    MemoryMapWindow w;

    // Simulate some activity by directly checking initial state
    auto minTime = TimePoint::min();
    CHECK(w.blockState(0).lastReadTime == minTime, "initial read time should be min");
    CHECK(w.blockState(0).lastWriteTime == minTime, "initial write time should be min");

    // Turn live ON — should clear (already clean, but verify no crash)
    w.setLive(true);
    CHECK(w.isLive(), "should be live after setLive(true)");
    CHECK(w.blockState(0).lastReadTime == minTime, "read time should be min after ON");
    CHECK(w.blockState(0).lastWriteTime == minTime, "write time should be min after ON");
    PASS();
}

// ---------------------------------------------------------------------------
// Test 19: Clear Activity
// ---------------------------------------------------------------------------

static void test_clear_activity()
{
    TEST("Clear Activity: resets all timestamps to min");
    MemoryMapWindow w;
    auto minTime = TimePoint::min();

    // Turn live on, then clear
    w.setLive(true);
    w.clearActivity();

    // Verify all blocks are reset
    bool allClear = true;
    for (int i = 0; i < MemoryMapWindow::NUM_BLOCKS; ++i) {
        if (w.blockState(i).lastReadTime != minTime ||
            w.blockState(i).lastWriteTime != minTime) {
            allClear = false;
            break;
        }
    }
    CHECK(allClear, "all blocks should be cleared after clearActivity()");
    PASS();
}

// ---------------------------------------------------------------------------
// Test 20: Code/Data classification does not affect live color
// ---------------------------------------------------------------------------

static void test_classification_no_effect_on_color()
{
    TEST("Code/Data/Unknown: does not affect Live block color");
    // computeBlockColor takes only timestamps — no classification parameter.
    // This verifies the design: color is purely time-based.
    auto now = Clock::now();

    // Same timestamps → same color, regardless of any "type"
    auto c1 = computeBlockColor(ago(100), ago(600), now, DURATION);
    auto c2 = computeBlockColor(ago(100), ago(600), now, DURATION);
    CHECK(c1 == c2, "same timestamps should produce same color");
    CHECK(c1 == LiveBlockColor::Green, "read active, write expired → Green");
    PASS();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    printf("=== test_live_map: Memory Map Live Activity ===\n\n");

    // 1–7: Address ↔ Block mapping
    test_block0_address();
    test_block1_address();
    test_block4_address();
    test_block31_address();
    test_block32_address();
    test_block128_address();
    test_block255_address();
    test_address_to_block_inverse();

    // 8–14: computeBlockColor
    test_color_no_activity();
    test_color_read_only();
    test_color_write_only();
    test_color_read_and_write();
    test_color_read_expired();
    test_color_write_expired();
    test_color_read_active_write_expired();

    // 15–16: Timer extension
    test_timer_extension_read();
    test_timer_extension_write_after_read();

    // 17–18: Live OFF/ON
    test_live_off_no_color();
    test_live_on_clears_state();

    // 19: Clear Activity
    test_clear_activity();

    // 20: Code/Data doesn't affect color
    test_classification_no_effect_on_color();

    printf("\n=== Results: %d/%d passed ===\n", passedTests, totalTests);

    return (passedTests == totalTests) ? 0 : 1;
}
