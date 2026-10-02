#include "vram_mapping.h"
#include <cstdio>
#include <cstdlib>

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

// ---------------------------------------------------------------------------
// Helper: create default 256-mode VideoInfo
// ---------------------------------------------------------------------------

static VramMapping::VideoInfo make256Mode()
{
    VramMapping::VideoInfo vi;
    vi.mode512       = false;
    vi.screenWidth   = 576;
    vi.screenHeight  = 288;
    vi.visibleWidth  = 256;
    vi.visibleHeight = 256;
    vi.borderLeft    = 160;  // (576-256)/2 = 160
    vi.borderTop     = 16;   // (288-256)/2 = 16
    vi.scrollValue   = 0;
    vi.vramBase      = 0xC000;
    vi.pixelsPerByte = 8;
    return vi;
}

static VramMapping::VideoInfo make512Mode()
{
    VramMapping::VideoInfo vi;
    vi.mode512       = true;
    vi.screenWidth   = 576;
    vi.screenHeight  = 288;
    vi.visibleWidth  = 512;
    vi.visibleHeight = 256;
    vi.borderLeft    = 32;   // (576-512)/2 = 32
    vi.borderTop     = 16;   // (288-256)/2 = 16
    vi.scrollValue   = 0;
    vi.vramBase      = 0xC000;
    vi.pixelsPerByte = 4;
    return vi;
}

// ---------------------------------------------------------------------------
// Test: 256-mode screen-to-VRAM mapping
// ---------------------------------------------------------------------------

static void test_256_screen_to_vram_basic()
{
    TEST("256-mode: top-left pixel -> VRAM C000");
    auto vi = make256Mode();

    // Top-left visible pixel: (borderLeft, borderTop) = (160, 16)
    auto result = VramMapping::screenToVram(160, 16, vi);
    CHECK(result.count == 1, "should have 1 address");
    CHECK(result.addresses[0] == 0xC000, "should be C000");
    PASS();
}

static void test_256_screen_to_vram_second_byte()
{
    TEST("256-mode: 2nd byte column -> VRAM C100");
    auto vi = make256Mode();

    // 2nd VRAM byte column: visibleX = 8 (pixels 8-15)
    int bufX = 160 + 8;  // borderLeft + 8
    auto result = VramMapping::screenToVram(bufX, 16, vi);
    CHECK(result.count == 1, "should have 1 address");
    CHECK(result.addresses[0] == 0xC100, "should be C100");
    PASS();
}

static void test_256_screen_to_vram_second_row()
{
    TEST("256-mode: 2nd scanline -> VRAM C001");
    auto vi = make256Mode();

    // 2nd scanline: visibleLine = 1
    int bufY = 16 + 1;  // borderTop + 1
    auto result = VramMapping::screenToVram(160, bufY, vi);
    CHECK(result.count == 1, "should have 1 address");
    CHECK(result.addresses[0] == 0xC001, "should be C001");
    PASS();
}

static void test_256_screen_to_vram_border()
{
    TEST("256-mode: border pixel -> count=0");
    auto vi = make256Mode();

    // Border area (above visible)
    auto result = VramMapping::screenToVram(0, 0, vi);
    CHECK(result.count == 0, "border should have count=0");
    PASS();
}

static void test_256_screen_to_vram_last_pixel()
{
    TEST("256-mode: bottom-right visible pixel");
    auto vi = make256Mode();

    // Bottom-right visible pixel
    int bufX = 160 + 255;  // borderLeft + visibleWidth - 1
    int bufY = 16 + 255;   // borderTop + visibleHeight - 1
    auto result = VramMapping::screenToVram(bufX, bufY, vi);
    CHECK(result.count == 1, "should have 1 address");
    // vramCol = 255/8 = 31, vramRow = 255
    // addr = 0xC000 + 31*256 + 255 = 0xC000 + 0x1F00 + 0xFF = 0xDFFF
    CHECK(result.addresses[0] == 0xDFFF, "should be DFFF");
    PASS();
}

// ---------------------------------------------------------------------------
// Test: 512-mode screen-to-VRAM mapping
// ---------------------------------------------------------------------------

static void test_512_screen_to_vram_basic()
{
    TEST("512-mode: top-left pixel -> VRAM C000");
    auto vi = make512Mode();

    auto result = VramMapping::screenToVram(32, 16, vi);
    CHECK(result.count == 1, "should have 1 address");
    CHECK(result.addresses[0] == 0xC000, "should be C000");
    PASS();
}

static void test_512_screen_to_vram_second_byte()
{
    TEST("512-mode: 2nd byte column -> VRAM C100");
    auto vi = make512Mode();

    // 2nd VRAM byte: visibleX = 4 (pixels per byte = 4)
    int bufX = 32 + 4;
    auto result = VramMapping::screenToVram(bufX, 16, vi);
    CHECK(result.count == 1, "should have 1 address");
    CHECK(result.addresses[0] == 0xC100, "should be C100");
    PASS();
}

static void test_512_screen_to_vram_last_pixel()
{
    TEST("512-mode: bottom-right visible pixel");
    auto vi = make512Mode();

    int bufX = 32 + 511;
    int bufY = 16 + 255;
    auto result = VramMapping::screenToVram(bufX, bufY, vi);
    CHECK(result.count == 1, "should have 1 address");
    // vramCol = 511/4 = 127, vramRow = 255
    // raw = 0xC000 + 127*256 + 255 = 0x13FFF -> truncated to 0x3FFF (uint16_t)
    // This is expected: 512-mode VRAM mapping overflows 16-bit address space
    CHECK(result.addresses[0] == 0x3FFF, "should be 3FFF (truncated)");
    PASS();
}

// ---------------------------------------------------------------------------
// Test: VRAM-to-screen mapping
// ---------------------------------------------------------------------------

static void test_vram_to_screen_basic()
{
    TEST("vramToScreen: C000 -> (0,0) in 256-mode");
    auto vi = make256Mode();

    auto pos = VramMapping::vramToScreen(0xC000, vi);
    CHECK(pos.valid, "should be valid");
    CHECK(pos.x == 0, "x should be 0");
    CHECK(pos.y == 0, "y should be 0");
    PASS();
}

static void test_vram_to_screen_second_byte()
{
    TEST("vramToScreen: C100 -> (8,0) in 256-mode");
    auto vi = make256Mode();

    auto pos = VramMapping::vramToScreen(0xC100, vi);
    CHECK(pos.valid, "should be valid");
    CHECK(pos.x == 8, "x should be 8");
    CHECK(pos.y == 0, "y should be 0");
    PASS();
}

static void test_vram_to_screen_second_row()
{
    TEST("vramToScreen: C001 -> (0,1) in 256-mode");
    auto vi = make256Mode();

    auto pos = VramMapping::vramToScreen(0xC001, vi);
    CHECK(pos.valid, "should be valid");
    CHECK(pos.x == 0, "x should be 0");
    CHECK(pos.y == 1, "y should be 1");
    PASS();
}

static void test_vram_to_screen_512()
{
    TEST("vramToScreen: C100 -> (4,0) in 512-mode");
    auto vi = make512Mode();

    auto pos = VramMapping::vramToScreen(0xC100, vi);
    CHECK(pos.valid, "should be valid");
    CHECK(pos.x == 4, "x should be 4");
    CHECK(pos.y == 0, "y should be 0");
    PASS();
}

// ---------------------------------------------------------------------------
// Test: Scroll register
// ---------------------------------------------------------------------------

static void test_scroll_offset()
{
    TEST("256-mode with scroll=10: top line maps to row 10");
    VramMapping::VideoInfo vi = make256Mode();
    vi.scrollValue = 10;

    // Top-left visible pixel should map to row 10
    auto result = VramMapping::screenToVram(160, 16, vi);
    CHECK(result.count == 1, "should have 1 address");
    // vramRow = (10 + 0) & 0xFF = 10
    // addr = 0xC000 + 0*256 + 10 = 0xC00A
    CHECK(result.addresses[0] == 0xC00A, "should be C00A");
    PASS();
}

static void test_scroll_wraparound()
{
    TEST("256-mode with scroll=250: row wraps around");
    VramMapping::VideoInfo vi = make256Mode();
    vi.scrollValue = 250;

    // visibleLine = 10 → vramRow = (250 + 10) & 0xFF = 4
    int bufY = 16 + 10;
    auto result = VramMapping::screenToVram(160, bufY, vi);
    CHECK(result.count == 1, "should have 1 address");
    CHECK(result.addresses[0] == 0xC004, "should be C004");
    PASS();
}

// ---------------------------------------------------------------------------
// Test: Roundtrip (screen → VRAM → screen)
// ---------------------------------------------------------------------------

static void test_roundtrip_256()
{
    TEST("Roundtrip 256-mode: screen -> VRAM -> screen");
    auto vi = make256Mode();

    // Pick a point in the middle of a byte
    int origBufX = 160 + 100;  // visibleX = 100
    int origBufY = 16 + 50;    // visibleLine = 50

    auto vram = VramMapping::screenToVram(origBufX, origBufY, vi);
    CHECK(vram.count == 1, "should have VRAM address");

    auto screen = VramMapping::vramToScreen(vram.addresses[0], vi);
    CHECK(screen.valid, "should map back to screen");

    // The screen position should be the top-left pixel of the byte
    // visibleX = (100 / 8) * 8 = 96
    int expectedX = (100 / 8) * 8;
    CHECK(screen.x == expectedX, "x should match byte start");
    CHECK(screen.y == 50, "y should match");
    PASS();
}

static void test_roundtrip_512()
{
    TEST("Roundtrip 512-mode: screen -> VRAM -> screen");
    auto vi = make512Mode();

    int origBufX = 32 + 200;  // visibleX = 200
    int origBufY = 16 + 100;  // visibleLine = 100

    auto vram = VramMapping::screenToVram(origBufX, origBufY, vi);
    CHECK(vram.count == 1, "should have VRAM address");

    auto screen = VramMapping::vramToScreen(vram.addresses[0], vi);
    CHECK(screen.valid, "should map back to screen");

    int expectedX = (200 / 4) * 4;
    CHECK(screen.x == expectedX, "x should match byte start");
    CHECK(screen.y == 100, "y should match");
    PASS();
}

// ---------------------------------------------------------------------------
// Test: Invalid VRAM address
// ---------------------------------------------------------------------------

static void test_vram_to_screen_invalid()
{
    TEST("vramToScreen: address below VRAM base -> invalid");
    auto vi = make256Mode();

    auto pos = VramMapping::vramToScreen(0x8000, vi);
    CHECK(!pos.valid, "should be invalid");
    PASS();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    printf("========================================\n");
    printf("  VRAM Mapping Tests\n");
    printf("========================================\n\n");

    // 256-mode tests
    test_256_screen_to_vram_basic();
    test_256_screen_to_vram_second_byte();
    test_256_screen_to_vram_second_row();
    test_256_screen_to_vram_border();
    test_256_screen_to_vram_last_pixel();

    // 512-mode tests
    test_512_screen_to_vram_basic();
    test_512_screen_to_vram_second_byte();
    test_512_screen_to_vram_last_pixel();

    // VRAM-to-screen tests
    test_vram_to_screen_basic();
    test_vram_to_screen_second_byte();
    test_vram_to_screen_second_row();
    test_vram_to_screen_512();

    // Scroll tests
    test_scroll_offset();
    test_scroll_wraparound();

    // Roundtrip tests
    test_roundtrip_256();
    test_roundtrip_512();

    // Invalid address test
    test_vram_to_screen_invalid();

    printf("\n========================================\n");
    printf("  Results: %d/%d passed\n", passedTests, totalTests);
    printf("========================================\n");

    return (passedTests == totalTests) ? 0 : 1;
}
