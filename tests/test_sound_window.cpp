// Sound window tests — 8-channel layout, source mapping and independence.
//
// Part A: pure SoundWindow::analyzeSnapshot / channel metadata tests using
//         handcrafted SoundSnapshot structs (no emulator required).
// Part B: integration with the REAL DebugAdapter — standard noise toggling
//         is measured from actual PIA1 Port C bit 0 writes routed through
//         the io.onwrite hook, i8253 counter writes are tracked per port,
//         and AY writes must never influence the Standard Noise channel.
//
// Headless: sound/video disabled, no emulation thread — port writes are
// injected directly via DebugAdapter::writeIoPort().

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>

#include "options.h"
#include "debugger_types.h"
#include "debug_adapter.h"
#include "i8080.h"
#include "sound_window.h"

// CPU accessors (i8080_iff/i8080_pc/i8080_regs_sp) live in this namespace.
using namespace i8080cpu;

// Defined by the application layer; debug_adapter.cpp HAL references it.
class DebugBackend;
DebugBackend *g_adapter_backend = nullptr;

// ---------------------------------------------------------------------------
// Test helpers
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
            unsigned long long _e = (unsigned long long)(exp); \
            unsigned long long _a = (unsigned long long)(act); \
            if (_e != _a) { \
                printf("  \033[41;97m FAIL \033[0m %s: expected %llu, got %llu (line %d)\n", \
                       msg, _e, _a, __LINE__); \
                _test_ok = false; \
            } else { \
                printf("  \033[46;30m ok \033[0m %s = %llu\n", msg, _a); \
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
// Part A — pure snapshot analysis
// ---------------------------------------------------------------------------

// Helper: fill snapshot fields from raw AY mixer/amp registers the same way
// the DebugAdapter does (keeps Part A independent from the emulator).
static void applyMixer(SoundSnapshot &s)
{
    uint8_t mixer = s.registers[7];
    s.toneAEnabled   = !(mixer & 0x01);
    s.toneBEnabled   = !(mixer & 0x02);
    s.toneCEnabled   = !(mixer & 0x04);
    s.noiseAEnabled  = !(mixer & 0x08);
    s.noiseBEnabled  = !(mixer & 0x10);
    s.noiseCEnabled  = !(mixer & 0x20);
}

static void test_channel_order_and_labels()
{
    TEST_BEGIN("Sound window: 8 channels, exact names and UI order");

    using SW = SoundWindow;
    CHECK_EQ(8, SW::NUM_CHANNELS, "NUM_CHANNELS == 8");

    static const char *expectedNames[8] = {
        "Tape Out (PC0)",
        "i8253 Channel 1",
        "i8253 Channel 2",
        "i8253 Channel 3",
        "AY Noise Channel",
        "AY Channel A",
        "AY Channel B",
        "AY Channel C",
    };
    static const char *expectedSections[8] = {
        "Tape Out (PC0)",
        "i8253", "i8253", "i8253",
        "AY", "AY", "AY", "AY",
    };
    for (int ch = 0; ch < 8; ++ch) {
        CHECK(strcmp(SW::channelName(ch), expectedNames[ch]) == 0,
              expectedNames[ch]);
        CHECK(strcmp(SW::channelSection(ch), expectedSections[ch]) == 0,
              "section assignment");
    }
    // Ambiguous "Noise Channel" (without a source prefix) must be gone
    CHECK(strcmp(SW::channelName(3), "Noise Channel") != 0,
          "no ambiguous bare 'Noise Channel' label");

    TEST_END();
}

static void test_std_noise_source_is_not_ay_noise()
{
    TEST_BEGIN("Sound window: Standard Noise source != AY Noise source");

    using SW = SoundWindow;

    // Case 1: AY noise generator active, standard PC0 noise idle
    SoundSnapshot s1{};
    s1.available = true;
    s1.registers[6] = 10;            // AY noise period
    s1.registers[7] = 0x37;          // tone A/B/C off, noise A enabled
    s1.registers[8] = 0x0F;          // amp A = 15
    applyMixer(s1);
    // standardNoise left at defaults: togglesSinceLast == 0
    SW::Analysis a1 = SW::analyzeSnapshot(s1);
    CHECK(a1.active[SW::CH_AY_NOISE], "AY Noise active from AY registers");
    CHECK(a1.level[SW::CH_AY_NOISE] > 0.0f, "AY Noise level > 0");
    CHECK(!a1.active[SW::CH_STD_NOISE], "Standard Noise idle with AY noise active");
    CHECK(a1.level[SW::CH_STD_NOISE] == 0.0f, "Standard Noise level == 0 (not fed by AY)");

    // Case 2: standard PC0 noise active, AY completely quiet
    SoundSnapshot s2{};
    s2.available = true;
    s2.registers[7] = 0xFF;          // all AY outputs disabled
    applyMixer(s2);
    s2.standardNoise.togglesSinceLast = 1000;
    s2.standardNoise.toggleRateHz     = 5000.0;
    s2.standardNoise.dirty            = true;
    SW::Analysis a2 = SW::analyzeSnapshot(s2);
    CHECK(a2.active[SW::CH_STD_NOISE], "Standard Noise active from PC0 toggles");
    CHECK(a2.level[SW::CH_STD_NOISE] > 0.0f, "Standard Noise level > 0");
    CHECK(!a2.active[SW::CH_AY_NOISE], "AY Noise idle with PC0 toggling");
    CHECK(a2.level[SW::CH_AY_NOISE] == 0.0f, "AY Noise level == 0 (not fed by PC0)");

    TEST_END();
}

static void test_timer_channels_not_swapped()
{
    TEST_BEGIN("Sound window: i8253 Channel 1..3 mapping and levels");

    using SW = SoundWindow;
    SoundSnapshot s{};
    s.available = true;
    s.registers[7] = 0xFF;           // AY off
    applyMixer(s);
    s.timerChannels[0].loadValue = 10000; // Ch1: 150 Hz
    s.timerChannels[0].sounding = true;
    s.timerChannels[1].loadValue = 1000;  // Ch2: 1500 Hz
    s.timerChannels[1].sounding = true;
    s.timerChannels[2].loadValue = 100;   // Ch3: 15 kHz
    s.timerChannels[2].sounding = true;

    SW::Analysis a = SW::analyzeSnapshot(s);
    CHECK(a.active[SW::CH_TIMER_BASE + 0], "i8253 Channel 1 active");
    CHECK(a.active[SW::CH_TIMER_BASE + 1], "i8253 Channel 2 active");
    CHECK(a.active[SW::CH_TIMER_BASE + 2], "i8253 Channel 3 active");
    // Higher frequency → taller bar (existing logarithmic model)
    CHECK(a.level[1] < a.level[2] && a.level[2] < a.level[3],
          "bar height follows counter load value per channel");

    // Only counter 1 (Channel 2) written
    SoundSnapshot s2{};
    s2.available = true;
    s2.registers[7] = 0xFF;
    applyMixer(s2);
    s2.timerChannels[1].loadValue = 500;
    s2.timerChannels[1].sounding = true;
    SW::Analysis a2 = SW::analyzeSnapshot(s2);
    CHECK(!a2.active[1] && a2.active[2] && !a2.active[3],
          "single-counter sounding maps to exactly one channel");

    TEST_END();
}

static void test_ay_tone_channels_not_swapped()
{
    TEST_BEGIN("Sound window: AY Channel A/B/C amplitude mapping");

    using SW = SoundWindow;
    SoundSnapshot s{};
    s.available = true;
    s.registers[7] = 0x38;           // tone A/B/C enabled, noise disabled
    s.registers[8]  = 0x03;          // amp A = 3
    s.registers[9]  = 0x07;          // amp B = 7
    s.registers[10] = 0x0F;          // amp C = 15
    applyMixer(s);

    SW::Analysis a = SW::analyzeSnapshot(s);
    CHECK(a.active[SW::CH_AY_BASE + 0], "AY Channel A active");
    CHECK(a.active[SW::CH_AY_BASE + 1], "AY Channel B active");
    CHECK(a.active[SW::CH_AY_BASE + 2], "AY Channel C active");
    CHECK(a.level[5] < a.level[6] && a.level[6] < a.level[7],
          "A < B < C follows amplitude assignment");

    TEST_END();
}

static void test_ay_disabled_others_unaffected()
{
    TEST_BEGIN("Sound window: AY disabled — Standard Noise and i8253 still show");

    using SW = SoundWindow;
    SoundSnapshot s{};
    s.available = true;
    // AY: no register activity at all (amps zero, R7=0 means tone enabled but
    // with amp 0 the channels stay inactive)
    s.timerChannels[2].loadValue = 500;   // i8253 Channel 3
    s.timerChannels[2].sounding = true;
    s.standardNoise.togglesSinceLast = 40;
    s.standardNoise.toggleRateHz = 2400.0;
    s.standardNoise.dirty = true;

    SW::Analysis a = SW::analyzeSnapshot(s);
    CHECK(a.active[SW::CH_STD_NOISE], "Standard Noise active with AY disabled");
    CHECK(a.active[SW::CH_TIMER_BASE + 2], "i8253 Channel 3 active with AY disabled");
    CHECK(!a.active[SW::CH_AY_NOISE], "AY Noise inactive");
    CHECK(!a.active[SW::CH_AY_BASE + 0] && !a.active[SW::CH_AY_BASE + 1] &&
          !a.active[SW::CH_AY_BASE + 2], "AY tone channels inactive");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Part B — integration with the real DebugAdapter (PIA/i8253/AY writes)
// ---------------------------------------------------------------------------

// Consume a snapshot so the next one only reports changes after this call.
static SoundSnapshot takeSnapshot(DebugAdapter &adapter)
{
    return adapter.soundSnapshot();
}

static void test_adapter_pc0_transitions(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: PC0 transitions counted (same level, 0->1->0)");

    SoundSnapshot s = takeSnapshot(adapter);
    CHECK(s.available, "snapshot available");
    CHECK(s.standardNoise.lastLevel == 0 || s.standardNoise.lastLevel == 1,
          "PC0 mirror seeded to a definite level at init");

    // Write port 0x01 keeping bit 0 at its current level, changing bit 1.
    // A write is NOT a toggle: togglesSinceLast must stay 0.
    const uint8_t same = (s.standardNoise.lastLevel ? 0x03 : 0x02);
    adapter.writeIoPort(0x01, same);
    s = takeSnapshot(adapter);
    CHECK_EQ(0, s.standardNoise.togglesSinceLast,
             "PC0 x -> x write adds no toggle");
    adapter.writeIoPort(0x01, same);
    s = takeSnapshot(adapter);
    CHECK_EQ(0, s.standardNoise.togglesSinceLast,
             "repeated same-level write adds no toggle");

    // Flip bit 0 (bit 1 kept) → exactly one transition
    adapter.writeIoPort(0x01, same ^ 0x01);
    s = takeSnapshot(adapter);
    CHECK_EQ(1, s.standardNoise.togglesSinceLast,
             "PC0 flip counts one transition");

    // Flip back: 0 -> 1 -> 0 pattern totals two transitions across the pair
    adapter.writeIoPort(0x01, same);
    s = takeSnapshot(adapter);
    CHECK_EQ(1, s.standardNoise.togglesSinceLast,
             "PC0 flip back counts one more transition");
    CHECK_EQ(s.standardNoise.lastLevel, same & 1,
             "lastLevel follows PC0 state");

    TEST_END();
}

static void test_adapter_bsr_semantics(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: BSR SET/RESET decoded semantically");

    // Force a known state: PC0 = 0 via direct write, then consume.
    adapter.writeIoPort(0x01, 0x02);   // bit0=0, bit1=1
    SoundSnapshot s = takeSnapshot(adapter);
    (void)s;

    // BSR SET bit 0 (value 0b0000_0001): PC0 0 -> 1 → one transition
    adapter.writeIoPort(0x00, 0x01);
    s = takeSnapshot(adapter);
    CHECK_EQ(1, s.standardNoise.togglesSinceLast, "BSR SET bit0 on 0-level toggles");
    CHECK_EQ(1, s.standardNoise.lastLevel, "BSR SET leaves PC0 = 1");

    // BSR SET bit 0 again: PC0 already 1 → NO transition (write != toggle)
    adapter.writeIoPort(0x00, 0x01);
    s = takeSnapshot(adapter);
    CHECK_EQ(0, s.standardNoise.togglesSinceLast, "BSR SET bit0 on 1-level: no toggle");

    // BSR RESET bit 0 (value 0b0000_0000): PC0 1 -> 0 → one transition
    adapter.writeIoPort(0x00, 0x00);
    s = takeSnapshot(adapter);
    CHECK_EQ(1, s.standardNoise.togglesSinceLast, "BSR RESET bit0 on 1-level toggles");

    // BSR RESET bit 0 again → no transition
    adapter.writeIoPort(0x00, 0x00);
    s = takeSnapshot(adapter);
    CHECK_EQ(0, s.standardNoise.togglesSinceLast, "BSR RESET bit0 on 0-level: no toggle");

    // BSR on OTHER bits must never count: bit1 SET (0x03), bit1 RESET (0x02),
    // bit2 SET (0x05), bit3 SET (0x09)
    adapter.writeIoPort(0x00, 0x03);
    adapter.writeIoPort(0x00, 0x02);
    adapter.writeIoPort(0x00, 0x05);
    adapter.writeIoPort(0x00, 0x09);
    s = takeSnapshot(adapter);
    CHECK_EQ(0, s.standardNoise.togglesSinceLast,
             "BSR writes to PC1..PC3 produce no PC0 toggles");

    // Direct writes changing only upper bits keep the counter at zero
    adapter.writeIoPort(0x01, 0x02);
    adapter.writeIoPort(0x01, 0x06);   // PC1 changed, PC0 same
    adapter.writeIoPort(0x01, 0x0A);   // PC3 changed, PC0 same
    adapter.writeIoPort(0x01, 0xFE);   // all high, PC0 same (0)
    s = takeSnapshot(adapter);
    CHECK_EQ(0, s.standardNoise.togglesSinceLast,
             "upper-bit changes on port 0x01 produce no PC0 toggles");

    TEST_END();
}

static void test_adapter_ay_independent_from_standard_noise(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: AY writes never feed the Standard Noise counter");

    // Run a real guest program issuing OUT to the AY ports so the writes
    // go through the normal io.commit() path (board.single_step commits
    // pending writes for opcode 0xD3 — same as during normal execution).
    adapter.reset(false);            // BLKSBR: detach boot, PC=0
    static const uint8_t prog[] = {
        0x3E, 0x06, 0xD3, 0x15,      // MVI A,6   / OUT 15h  (reg 6)
        0x3E, 0x0A, 0xD3, 0x14,      // MVI A,10  / OUT 14h  → R6 = 10
        0x3E, 0x07, 0xD3, 0x15,      // MVI A,7   / OUT 15h  (reg 7)
        0x3E, 0x37, 0xD3, 0x14,      // MVI A,37h / OUT 14h  → noise A on, tone off
        0x3E, 0x08, 0xD3, 0x15,      // MVI A,8   / OUT 15h  (reg 8)
        0x3E, 0x0F, 0xD3, 0x14,      // MVI A,0Fh / OUT 14h  → amp A = 15
    };
    for (size_t i = 0; i < sizeof(prog); ++i) {
        adapter.writeMemory(static_cast<uint16_t>(i), prog[i]);
    }
    takeSnapshot(adapter);           // consume counts from the reset path
    for (int i = 0; i < 12; ++i) {   // exactly the 12 instructions above
        adapter.stepInstruction();
    }

    SoundSnapshot s = takeSnapshot(adapter);
    CHECK_EQ(0, s.standardNoise.togglesSinceLast,
             "AY port writes (real OUTs) produce zero PC0 toggles");
    CHECK(s.standardNoise.lastLevel == 0,
          "PC0 level unchanged by AY writes (still 0)");
    CHECK(s.ayDirty, "AY dirty flag set by 0x14/0x15 writes");
    CHECK_EQ(10, s.registers[6], "AY R6 noise period readable");
    CHECK(s.noiseAEnabled && !s.toneAEnabled, "AY R7 mixer decoded");

    SoundWindow::Analysis a = SoundWindow::analyzeSnapshot(s);
    CHECK(a.active[SoundWindow::CH_AY_NOISE], "AY Noise channel driven by AY data");
    CHECK(!a.active[SoundWindow::CH_STD_NOISE],
          "Standard Noise stays idle while AY writes");
    CHECK(a.level[SoundWindow::CH_AY_NOISE] > 0.0f &&
          a.level[SoundWindow::CH_STD_NOISE] == 0.0f,
          "levels strictly separated: AY>0, Standard==0");

    // And now a real PC0 toggle with AY registers left untouched
    takeSnapshot(adapter);              // consume ayDirty
    adapter.writeIoPort(0x01, 0x03);    // PC0 0 -> 1
    SoundSnapshot s2 = takeSnapshot(adapter);
    CHECK_EQ(1, s2.standardNoise.togglesSinceLast,
             "PC0 toggle counted");
    CHECK(!s2.ayDirty, "ayDirty stays clear without AY writes");

    SoundWindow::Analysis a2 = SoundWindow::analyzeSnapshot(s2);
    CHECK(a2.active[SoundWindow::CH_STD_NOISE],
          "Standard Noise active from PC0 only");
    // AY noise state itself unchanged (R6/R7/R8 the same as before)
    CHECK_EQ(10, s2.registers[6], "AY R6 unchanged by PC0 write");

    TEST_END();
}

static void test_adapter_i8253_port_mapping(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: i8253 counters map to Channels 1..3 correctly");

    takeSnapshot(adapter);   // consume

    // Vector-06C port map (vio.h): 0x08 = control word,
    // 0x0B → counter 0 (Channel 1), 0x0A → counter 1, 0x09 → counter 2.
    // Ch1: CW 36h (ctr0, LSB+MSB, mode3), load 10000 = 0x2710
    adapter.writeIoPort(0x08, 0x36);
    adapter.writeIoPort(0x0B, 0x10);
    adapter.writeIoPort(0x0B, 0x27);
    // Ch2: CW 76h, load 1000 = 0x03E8
    adapter.writeIoPort(0x08, 0x76);
    adapter.writeIoPort(0x0A, 0xE8);
    adapter.writeIoPort(0x0A, 0x03);
    // Ch3: CW B6h, load 100 = 0x0064
    adapter.writeIoPort(0x08, 0xB6);
    adapter.writeIoPort(0x09, 0x64);
    adapter.writeIoPort(0x09, 0x00);

    SoundSnapshot s = takeSnapshot(adapter);
    CHECK(s.timerChannels[0].dirty && s.timerChannels[0].loadValue == 10000,
          "Channel 1 ← port 0x0B (load 10000)");
    CHECK(s.timerChannels[1].dirty && s.timerChannels[1].loadValue == 1000,
          "Channel 2 ← port 0x0A (load 1000)");
    CHECK(s.timerChannels[2].dirty && s.timerChannels[2].loadValue == 100,
          "Channel 3 ← port 0x09 (load 100)");
    CHECK_EQ(3, s.timerChannels[0].mode, "Channel 1 mode 3");
    CHECK_EQ(3, s.timerChannels[2].mode, "Channel 3 mode 3");
    // All three counters got a mode-3 divider load → all sustained-sounding.
    CHECK(s.timerChannels[0].sounding && s.timerChannels[1].sounding &&
          s.timerChannels[2].sounding,
          "mode-3 divider load makes all three counters sound");
    // Standard noise must be untouched by timer traffic
    CHECK_EQ(0, s.standardNoise.togglesSinceLast,
             "i8253 writes produce no PC0 toggles");

    // Dirty flags are per-snapshot: the next snapshot reports zero delta
    s = takeSnapshot(adapter);
    CHECK(!s.timerChannels[0].dirty && !s.timerChannels[1].dirty &&
          !s.timerChannels[2].dirty,
          "dirty flags cleared after snapshot (measurements per interval)");
    // ... but `sounding` persists with no further writes — that is what keeps
    // the note bar lit from start to cancel instead of flashing on the write.
    CHECK(s.timerChannels[0].sounding && s.timerChannels[1].sounding &&
          s.timerChannels[2].sounding,
          "sounding persists across snapshots (sustained tone, not an event)");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Sustained tone: a VI53 counter sounds from the moment a mode-3 divider is
// loaded until it is reprogrammed — the bar must stay lit for the whole note,
// not just flash on the write frame. Mirrors the ROM's vi53_set_channel()
// (cancel = control word only) and vi53_set_channel_m0() (cancel = mode 0 +
// two zero bytes) helpers exercised by snd_backends.rom.
// ---------------------------------------------------------------------------

static void test_adapter_i8253_sustained_tone(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: i8253 tone sustains from note start until cancel");

    takeSnapshot(adapter);   // consume

    // Note start on counter 0 (Channel 1), exactly vi53_set_channel(0, 3409):
    // control word 0x36 (ctr0, LSB+MSB, mode 3), then divider 3409 = 0x0D51
    // (440 Hz). The control word alone disables; the completed load re-arms.
    adapter.writeIoPort(0x08, 0x36);
    adapter.writeIoPort(0x0B, 0x51);   // LSB
    adapter.writeIoPort(0x0B, 0x0D);   // MSB → load complete

    SoundSnapshot s = takeSnapshot(adapter);
    CHECK(s.timerChannels[0].sounding, "Channel 1 sounding right after note start");
    CHECK(s.timerChannels[0].dirty, "Channel 1 dirty on the write frame");
    CHECK_EQ(3409, s.timerChannels[0].loadValue, "Channel 1 divider = 3409 (440 Hz)");

    SoundWindow::Analysis a = SoundWindow::analyzeSnapshot(s);
    CHECK(a.active[SoundWindow::CH_TIMER_BASE + 0], "bar active while the tone sounds");
    CHECK(a.level[SoundWindow::CH_TIMER_BASE + 0] > 0.0f, "bar level > 0 while sounding");

    // The note keeps sounding across snapshots with NO further writes: unlike
    // `dirty` (cleared every snapshot), `sounding` is persistent state.
    s = takeSnapshot(adapter);
    CHECK(!s.timerChannels[0].dirty, "dirty cleared on the next snapshot");
    CHECK(s.timerChannels[0].sounding, "tone still sounding with no new writes");
    s = takeSnapshot(adapter);
    CHECK(s.timerChannels[0].sounding, "tone sustains across many snapshots");

    // Cancel variant A (music.c vi53_set_channel(0,0)): a mode-3 control word
    // with NO data write reprograms the counter → SetMode disables it.
    adapter.writeIoPort(0x08, 0x36);
    s = takeSnapshot(adapter);
    CHECK(!s.timerChannels[0].sounding, "a control word alone cancels the tone");
    a = SoundWindow::analyzeSnapshot(s);
    CHECK(!a.active[SoundWindow::CH_TIMER_BASE + 0], "bar goes dark after cancel");
    CHECK(a.level[SoundWindow::CH_TIMER_BASE + 0] == 0.0f, "bar level 0 after cancel");

    // Restart, then cancel variant B (sound.c vi53_set_channel_m0(0,0)):
    // mode-0 control word + two zero data bytes → silence.
    adapter.writeIoPort(0x08, 0x36);
    adapter.writeIoPort(0x0B, 0x51);
    adapter.writeIoPort(0x0B, 0x0D);
    s = takeSnapshot(adapter);
    CHECK(s.timerChannels[0].sounding, "tone restarted by a fresh mode-3 load");
    adapter.writeIoPort(0x08, 0x30);   // ctr0, mode 0 (OUT=0, silence)
    adapter.writeIoPort(0x0B, 0x00);
    adapter.writeIoPort(0x0B, 0x00);
    s = takeSnapshot(adapter);
    CHECK(!s.timerChannels[0].sounding, "mode-0 + zero divider silences the tone");

    TEST_END();
}

// ---------------------------------------------------------------------------
// Regression: loading a second ROM must silence a note stuck from the first
// one. The bootloader (boots.bin 0x0000-0x0010) re-initializes PIA/PPI/i8253
// after a real reset; ROM load skips it, so DebugAdapter::loadRom replays it.
// ---------------------------------------------------------------------------

static void test_loadrom_silences_stuck_note(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: loadRom re-inits sound ports (no stuck note)");

    // --- ROM 1 leaves the machine mid-melody: counter 0 free-running in
    // square-wave mode with a real tone value, tape-out level high.
    adapter.reset(false);            // BLKSBR: detach boot, PC=0
    static const uint8_t melody[] = {
        0x3E, 0x36, 0xD3, 0x08,      // MVI A,36h / OUT 08h → ctr0 mode 3, LSB+MSB
        0x3E, 0x10, 0xD3, 0x0B,      // MVI A,10h / OUT 0Bh → load lo
        0x3E, 0x27, 0xD3, 0x0B,      // MVI A,27h / OUT 0Bh → load hi (10000)
        0x3E, 0x01, 0xD3, 0x01,      // MVI A,01h / OUT 01h → PC0 (tape-out) high
    };
    for (size_t i = 0; i < sizeof(melody); ++i) {
        adapter.writeMemory(static_cast<uint16_t>(i), melody[i]);
    }
    takeSnapshot(adapter);           // consume reset-path counts
    for (int i = 0; i < 10; ++i) {   // 5 MVI + 5 OUT
        adapter.stepInstruction();
    }

    SoundSnapshot s = takeSnapshot(adapter);
    CHECK_EQ(3, s.timerChannels[0].mode, "before reload: ctr0 stuck in mode 3");
    CHECK_EQ(10000, s.timerChannels[0].loadValue, "before reload: ctr0 tone loaded");
    CHECK(s.timerChannels[0].sounding, "before reload: ctr0 stuck note is sounding");
    CHECK_EQ(1, s.standardNoise.lastLevel, "before reload: tape-out level high");

    // --- Load ROM 2: must replay the boot ROM's port initialization.
    const std::string rom2 = "/tmp/v06c_test_stuck_note_second.rom";
    {
        std::ofstream f(rom2, std::ios::binary);
        const uint8_t ret = 0xC9;    // harmless RET at 0100
        f.write(reinterpret_cast<const char*>(&ret), 1);
    }
    bool ok = adapter.loadRom(rom2, 0);
    CHECK(ok, "second ROM loaded");

    s = takeSnapshot(adapter);
    // boots.bin control words: A8h/68h/28h → 3-bit mode field = 4 for all
    // three counters — out of square-wave (3) one-shot-rate (2) modes.
    CHECK_EQ(4, s.timerChannels[0].mode, "after reload: ctr0 out of square mode");
    CHECK_EQ(4, s.timerChannels[1].mode, "after reload: ctr1 out of square mode");
    CHECK_EQ(4, s.timerChannels[2].mode, "after reload: ctr2 out of square mode");
    CHECK(!s.timerChannels[0].sounding && !s.timerChannels[1].sounding &&
          !s.timerChannels[2].sounding,
          "after reload: boot control words silenced every stuck note");
    CHECK_EQ(0, s.standardNoise.lastLevel, "after reload: tape-out silenced");

    // Latched port state readable through the I/O layer:
    CHECK_EQ(0x9B, adapter.readIoPort(0x04), "PPI2 control word = 9Bh (boot value)");
    CHECK_EQ(0x00, adapter.readIoPort(0x03), "PIA1 port A cleared by boot CW write");
    CHECK_EQ(0x00, adapter.readIoPort(0x02), "PIA1 port B (border/mode) cleared");

    std::remove(rom2.c_str());
    TEST_END();
}

// ---------------------------------------------------------------------------
// Regression: loading a ROM while the previous program sits with interrupts
// enabled must not hand them to the new one. i8080_init() (called by
// Board::reset) clears only flags and PC, so IFF, the EI hold-off and the
// inte pin used to survive — with page 0 freshly zeroed, every vblank then
// pushed RST 7's return address and the new ROM drowned (SP walking down).
// ---------------------------------------------------------------------------

static void test_loadrom_disables_inherited_interrupts(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: loadRom clears inherited IFF/INTE (no RST 7 storm)");

    // Old program: EI at 0000 followed by NOPs. The EI hold-off expires two
    // instructions later, which is exactly the state a ROM is in when the user
    // loads another one on top of it.
    adapter.reset(false);                 // BLKSBR: detach boot ROM, PC=0
    adapter.writeMemory(0x0000, 0xFB);    // EI
    for (int i = 1; i <= 4; ++i) adapter.writeMemory(static_cast<uint16_t>(i), 0x00);
    for (int i = 0; i < 3; ++i) adapter.stepInstruction();
    CHECK(i8080_iff(), "old program brought IFF up");

    const std::string rom = "/tmp/v06c_test_inherited_interrupts.rom";
    {
        std::ofstream f(rom, std::ios::binary);
        // JMP 0100 — a self-loop the NOP slide from 0000 lands in. It touches
        // no stack, so SP must not move unless something else pushes into it.
        const uint8_t spin[] = { 0xC3, 0x00, 0x01 };
        f.write(reinterpret_cast<const char*>(spin), sizeof(spin));
    }
    CHECK(adapter.loadRom(rom, 0), "ROM loaded over the EI-ing program");

    CHECK(!i8080_iff(), "load cleared IFF");
    CHECK_EQ(0x0000, i8080_pc(), "PC=0000 (the DI in loadRom did not move it)");
    uint16_t spAfterLoad = static_cast<uint16_t>(i8080_regs_sp());
    CHECK_EQ(0xc300, spAfterLoad, "SP=0xc300 after load");

    // With IFF or inte left inherited, each of these frames fires RST 7 ->
    // CALL 0038 into the zeroed page 0 and pushes two bytes; ten frames walk
    // SP down to 0xc2e8 while the spin loop itself never touches the stack.
    for (int i = 0; i < 10; ++i) adapter.executeFrame();
    CHECK_EQ(spAfterLoad, i8080_regs_sp(),
             "10 frames of the spin loop pushed nothing — no interrupt storm");

    std::remove(rom.c_str());
    adapter.reset(false);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Audio output gating: the device is silent unless BOTH the frame loop is
// producing samples and the user has not muted the machine. Options.nosound
// (set in main) keeps Soundnik::pause() away from SDL, so this exercises the
// state machine only.
// ---------------------------------------------------------------------------

static void test_audio_output_gate(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: audio output paused = muted || !frameLoopActive");

    adapter.setMuted(false);
    adapter.setAudioEmulationActive(false);
    CHECK(adapter.isAudioOutputPaused(), "paused machine is silent");

    adapter.setAudioEmulationActive(true);
    CHECK(!adapter.isAudioOutputPaused(), "running machine with sound on is audible");

    adapter.setMuted(true);
    CHECK(adapter.isAudioOutputPaused(), "mute wins over a running machine");

    adapter.setAudioEmulationActive(false);
    adapter.setMuted(false);
    CHECK(adapter.isAudioOutputPaused(),
          "unmute must not resurrect audio while paused");

    // Both inputs are remembered now, so a transition of one never silently
    // drops the other (the previous setMuted() forwarded straight to SDL).
    adapter.setAudioEmulationActive(true);
    CHECK(!adapter.isAudioOutputPaused(), "gate reopens with the frame loop");
    adapter.setMuted(true);
    adapter.setAudioEmulationActive(false);
    adapter.setAudioEmulationActive(true);
    CHECK(adapter.isAudioOutputPaused(), "mute survives gate toggles");

    adapter.setMuted(false);
    adapter.setAudioEmulationActive(false);
    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("\033[0;36m=== Sound Window Tests ===\033[0m\n");

    // --- Part A: pure analysis ---
    test_channel_order_and_labels();
    test_std_noise_source_is_not_ay_noise();
    test_timer_channels_not_swapped();
    test_ay_tone_channels_not_swapped();
    test_ay_disabled_others_unaffected();

    // --- Part B: real DebugAdapter ---
    printf("\n  Setting up headless DebugAdapter...\n");
    Options.novideo = true;
    Options.nosound = true;
    Options.pc = 0;

    static DebugAdapter adapter;   // leaked deliberately at exit
    adapter.init();
    adapter.bindHal();
    printf("  DebugAdapter ready\n");

    test_adapter_pc0_transitions(adapter);
    test_adapter_bsr_semantics(adapter);
    test_adapter_ay_independent_from_standard_noise(adapter);
    test_adapter_i8253_port_mapping(adapter);
    test_adapter_i8253_sustained_tone(adapter);
    test_loadrom_silences_stuck_note(adapter);
    test_loadrom_disables_inherited_interrupts(adapter);
    test_audio_output_gate(adapter);

    adapter.shutdown();

    printf("\n\033[0;36m=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", \033[41;97m %d FAILED \033[0m\033[0;36m", tests_failed);
    }
    printf(" ===\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
