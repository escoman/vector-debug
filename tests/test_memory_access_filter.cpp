// Unit tests for Memory Access filter — Stage 6.25
//
// The filter is a pure function `applyMemoryAccessFilter()` — it takes a
// snapshot of RuntimeAccessLogEntry and returns the subset that should be
// rendered.  These tests cover the semantics required by the pipeline doc
// §10.1:
//   - Fetch entries NEVER appear in any mode (Read / Write / All).
//   - Range is inclusive on both ends, applied to `entry.address`, not PC.
//   - Order is preserved (chronological from RingBuffer::snapshot()).
//   - Bounded by maxRows — drop-oldest.
//   - Invalid range (from > to) yields empty result.
//
// The tests link against memory_access_window.cpp, which also pulls in
// ImGui — same pattern used by test_sound_window.  We never call the
// window's render(); ImGui functions stay unreferenced.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "memory_access_window.h"

// ---------------------------------------------------------------------------
// Minimal test framework (matches the style used in test_runtime_accounting)
// ---------------------------------------------------------------------------

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_BEGIN(name) \
    do { \
        tests_run++; \
        printf("\n\033[0;35m=== TEST: %s ===\033[0m\n", name); \
        bool _test_ok = true;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  \033[41;97m FAIL \033[0m %s (line %d)\n", msg, __LINE__); \
            _test_ok = false; \
        } else { \
            printf("  \033[46;30m ok \033[0m %s\n", msg); \
        } \
    } while (0)

#define TEST_END() \
        if (_test_ok) { tests_passed++; printf("  \033[42;30m PASS \033[0m\n"); } \
        else          { tests_failed++; printf("  \033[41;30m FAILED \033[0m\n"); } \
    } while (0)

// ---------------------------------------------------------------------------
// Entry helpers
// ---------------------------------------------------------------------------

static RuntimeAccessLogEntry makeEntry(uint64_t seq,
                                        RuntimeAccessLogEntry::Type type,
                                        uint16_t addr,
                                        uint16_t pc,
                                        uint8_t value = 0)
{
    RuntimeAccessLogEntry e;
    e.sequence = seq;
    e.type     = type;
    e.address  = addr;
    e.pc       = pc;
    e.value    = value;
    return e;
}

static MemoryAccessFilter makeFilter(uint16_t from, uint16_t to,
                                     MemoryAccessFilter::Mode mode)
{
    MemoryAccessFilter f;
    f.from = from;
    f.to   = to;
    f.mode = mode;
    return f;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_empty_log_returns_empty()
{
    TEST_BEGIN("empty_log_returns_empty");
    std::vector<RuntimeAccessLogEntry> log;
    auto out = applyMemoryAccessFilter(log, makeFilter(0, 0xFFFF, MemoryAccessFilter::All), 100);
    CHECK(out.empty(), "empty input -> empty output");
    TEST_END();
}

static void test_range_inclusive_boundaries()
{
    TEST_BEGIN("range_inclusive_boundaries");

    // Range 8000..80FF must include 8000 and 80FF, exclude 7FFF and 8100.
    std::vector<RuntimeAccessLogEntry> log = {
        makeEntry(1, RuntimeAccessLogEntry::Read,  0x7FFF, 0x0100),
        makeEntry(2, RuntimeAccessLogEntry::Read,  0x8000, 0x0100),
        makeEntry(3, RuntimeAccessLogEntry::Read,  0x80FF, 0x0101),
        makeEntry(4, RuntimeAccessLogEntry::Read,  0x8100, 0x0102),
    };
    auto out = applyMemoryAccessFilter(log,
        makeFilter(0x8000, 0x80FF, MemoryAccessFilter::Read), 100);
    CHECK(out.size() == 2, "only 2 in-range entries");
    if (out.size() == 2) {
        CHECK(out[0].address == 0x8000 && out[0].sequence == 2, "8000 included");
        CHECK(out[1].address == 0x80FF && out[1].sequence == 3, "80FF included");
    }
    TEST_END();
}

static void test_mode_read_excludes_write_and_fetch()
{
    TEST_BEGIN("mode_read_excludes_write_and_fetch");

    std::vector<RuntimeAccessLogEntry> log = {
        makeEntry(1, RuntimeAccessLogEntry::Read,  0x8000, 0x0100),
        makeEntry(2, RuntimeAccessLogEntry::Write, 0x8001, 0x0101),
        makeEntry(3, RuntimeAccessLogEntry::Fetch, 0x8002, 0x0102),
        makeEntry(4, RuntimeAccessLogEntry::Read,  0x8003, 0x0103),
    };
    auto out = applyMemoryAccessFilter(log,
        makeFilter(0x8000, 0x80FF, MemoryAccessFilter::Read), 100);
    CHECK(out.size() == 2, "Read mode: exactly 2 reads");
    if (out.size() == 2) {
        CHECK(out[0].sequence == 1, "seq 1 preserved");
        CHECK(out[1].sequence == 4, "seq 4 preserved");
    }
    TEST_END();
}

static void test_mode_write_excludes_read_and_fetch()
{
    TEST_BEGIN("mode_write_excludes_read_and_fetch");

    std::vector<RuntimeAccessLogEntry> log = {
        makeEntry(1, RuntimeAccessLogEntry::Read,  0x8000, 0x0100),
        makeEntry(2, RuntimeAccessLogEntry::Write, 0x8001, 0x0101),
        makeEntry(3, RuntimeAccessLogEntry::Fetch, 0x8002, 0x0102),
        makeEntry(4, RuntimeAccessLogEntry::Write, 0x8003, 0x0103),
    };
    auto out = applyMemoryAccessFilter(log,
        makeFilter(0x8000, 0x80FF, MemoryAccessFilter::Write), 100);
    CHECK(out.size() == 2, "Write mode: exactly 2 writes");
    if (out.size() == 2) {
        CHECK(out[0].sequence == 2, "seq 2 preserved");
        CHECK(out[1].sequence == 4, "seq 4 preserved");
    }
    TEST_END();
}

static void test_mode_all_includes_read_and_write_but_never_fetch()
{
    TEST_BEGIN("mode_all_includes_read_and_write_but_never_fetch");

    std::vector<RuntimeAccessLogEntry> log = {
        makeEntry(1, RuntimeAccessLogEntry::Read,  0x8000, 0x0100),
        makeEntry(2, RuntimeAccessLogEntry::Fetch, 0x8001, 0x0101),  // opcode fetch
        makeEntry(3, RuntimeAccessLogEntry::Write, 0x8002, 0x0102),
        makeEntry(4, RuntimeAccessLogEntry::Fetch, 0x8003, 0x0103),  // immediate operand
        makeEntry(5, RuntimeAccessLogEntry::Read,  0x8004, 0x0104),
    };
    auto out = applyMemoryAccessFilter(log,
        makeFilter(0x8000, 0x80FF, MemoryAccessFilter::All), 100);
    CHECK(out.size() == 3, "All mode: Read+Write only, Fetch never");
    if (out.size() == 3) {
        CHECK(out[0].sequence == 1, "seq 1 preserved");
        CHECK(out[1].sequence == 3, "seq 3 preserved");
        CHECK(out[2].sequence == 5, "seq 5 preserved");
    }
    TEST_END();
}

static void test_preserves_order_by_sequence()
{
    TEST_BEGIN("preserves_order_by_sequence");

    // Interleave reads/writes at addresses that all pass the filter.
    std::vector<RuntimeAccessLogEntry> log;
    for (uint64_t i = 1; i <= 20; ++i) {
        auto type = (i % 2) ? RuntimeAccessLogEntry::Read : RuntimeAccessLogEntry::Write;
        log.push_back(makeEntry(i, type, static_cast<uint16_t>(0x8000 + (i & 0x0F)), 0x0100));
    }
    auto out = applyMemoryAccessFilter(log,
        makeFilter(0x8000, 0x80FF, MemoryAccessFilter::All), 100);
    CHECK(out.size() == 20, "all 20 pass");
    bool monotonic = true;
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i].sequence <= out[i - 1].sequence) { monotonic = false; break; }
    }
    CHECK(monotonic, "output sequences are strictly increasing");
    TEST_END();
}

static void test_bounded_max_rows_keeps_most_recent()
{
    TEST_BEGIN("bounded_max_rows_keeps_most_recent");

    std::vector<RuntimeAccessLogEntry> log;
    for (uint64_t i = 1; i <= 100; ++i) {
        log.push_back(makeEntry(i, RuntimeAccessLogEntry::Read, 0x8000, 0x0100));
    }
    auto out = applyMemoryAccessFilter(log,
        makeFilter(0x8000, 0x80FF, MemoryAccessFilter::All), 10);
    CHECK(out.size() == 10, "output capped at maxRows");
    if (out.size() == 10) {
        CHECK(out.front().sequence == 91, "drop-oldest: first = seq 91");
        CHECK(out.back().sequence  == 100, "drop-oldest: last  = seq 100");
    }
    TEST_END();
}

static void test_invalid_range_from_gt_to_returns_empty()
{
    TEST_BEGIN("invalid_range_from_gt_to_returns_empty");

    std::vector<RuntimeAccessLogEntry> log = {
        makeEntry(1, RuntimeAccessLogEntry::Read, 0x8000, 0x0100),
        makeEntry(2, RuntimeAccessLogEntry::Read, 0x9000, 0x0101),
    };
    MemoryAccessFilter bad = makeFilter(0x9000, 0x8000, MemoryAccessFilter::All);
    CHECK(!bad.isValid(), "from > to -> invalid");
    auto out = applyMemoryAccessFilter(log, bad, 100);
    CHECK(out.empty(), "invalid range yields empty result");
    TEST_END();
}

static void test_fetch_never_appears_even_at_boundary()
{
    TEST_BEGIN("fetch_never_appears_even_at_boundary");

    // Opcode fetches of instructions whose operand bytes fall inside range
    // must not be reported even in All mode.  Regression against §24 ТЗ.
    std::vector<RuntimeAccessLogEntry> log = {
        makeEntry(1, RuntimeAccessLogEntry::Fetch, 0x8000, 0x0500),  // opcode
        makeEntry(2, RuntimeAccessLogEntry::Fetch, 0x8001, 0x0500),  // imm lo
        makeEntry(3, RuntimeAccessLogEntry::Fetch, 0x8002, 0x0500),  // imm hi
        makeEntry(4, RuntimeAccessLogEntry::Read,  0x8003, 0x0510),  // real data read
    };
    auto out = applyMemoryAccessFilter(log,
        makeFilter(0x8000, 0x8003, MemoryAccessFilter::All), 100);
    CHECK(out.size() == 1, "only the real data read survives");
    if (out.size() == 1) {
        CHECK(out[0].sequence == 4, "kept entry is seq 4");
    }
    TEST_END();
}

static void test_zero_maxrows_yields_empty()
{
    TEST_BEGIN("zero_maxrows_yields_empty");

    std::vector<RuntimeAccessLogEntry> log = {
        makeEntry(1, RuntimeAccessLogEntry::Read, 0x8000, 0x0100),
    };
    auto out = applyMemoryAccessFilter(log,
        makeFilter(0x8000, 0x80FF, MemoryAccessFilter::All), 0);
    CHECK(out.empty(), "maxRows == 0 -> empty result");
    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// Stage 6.25: focusOnBlock() is invoked from Memory Map's "To Memory
// Access" context menu item.  It must: (a) set filter_ to [base, base+0xFF],
// (b) make the window visible, (c) queue a Refresh + focus bring-forward
// that render() will consume on the next frame.
static void test_focus_on_block_sets_range_and_pending() {
    TEST_BEGIN("MemoryAccessWindow::focusOnBlock sets range + pending flags");
    MemoryAccessWindow w;
    CHECK(!w.isVisible(), "window starts hidden");
    CHECK(!w.hasPendingRefresh(), "no pending refresh initially");
    CHECK(!w.hasPendingFocus(), "no pending focus initially");

    w.focusOnBlock(0x8100);

    CHECK(w.isVisible(), "focusOnBlock() makes window visible");
    CHECK(w.hasPendingRefresh(), "focusOnBlock() queues a Refresh");
    CHECK(w.hasPendingFocus(), "focusOnBlock() queues a focus bring-forward");
    CHECK(w.filter().from == 0x8100, "filter.from = base");
    CHECK(w.filter().to   == 0x81FF, "filter.to = base + 0xFF");
    CHECK(w.filter().isValid(), "seeded range is valid");

    // Also exercise the wrap-around case (Memory Map block at 0xFF00
    // yields range 0xFF00..0xFFFF which does NOT wrap since base+0xFF
    // stays inside uint16).  A base of 0xFFF0 would yield 0x1000-0xFFEF
    // which is still valid, but let's just check the top block.
    w.focusOnBlock(0xFF00);
    CHECK(w.filter().from == 0xFF00, "top block: from");
    CHECK(w.filter().to   == 0xFFFF, "top block: to");
    TEST_END();
}

int main()
{
    printf("\n\033[1;36m=== Memory Access filter tests (Stage 6.25) ===\033[0m\n");

    test_empty_log_returns_empty();
    test_range_inclusive_boundaries();
    test_mode_read_excludes_write_and_fetch();
    test_mode_write_excludes_read_and_fetch();
    test_mode_all_includes_read_and_write_but_never_fetch();
    test_preserves_order_by_sequence();
    test_bounded_max_rows_keeps_most_recent();
    test_invalid_range_from_gt_to_returns_empty();
    test_fetch_never_appears_even_at_boundary();
    test_zero_maxrows_yields_empty();
    test_focus_on_block_sets_range_and_pending();

    printf("\n=== Summary: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
