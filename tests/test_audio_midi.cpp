// test_audio_midi.cpp — tests for the GRAB AUDIO → SAVE MID pipeline.
//
// Part A: pure functions from src/audio_midi.cpp — the VI53 port-trace
//         parser (8253 protocol decode, note segmentation) and the SMF
//         format-1 writer. No emulator involved: input is a synthetic
//         event list, output is checked byte-structure-wise.
// Part B: the real DebugAdapter — grab buffer capture through io.onwrite
//         (port filtering, frame stamps, buffer lifecycle).
// Part C: golden reference comparison — real ROMs (PUTUP / RISEOUT attract
//         mode) captured through the full pipeline and checked against the
//         score-derived .mid files.
//
// Headless: novideo/nosound, no emulation thread — port writes are injected
// via DebugAdapter::writeIoPort() and frames are advanced with executeFrame().

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "options.h"
#include "debugger_types.h"
#include "debug_adapter.h"
#include "audio_midi.h"

// Defined by the application layer; debug_adapter.cpp HAL references it.
class DebugBackend;
DebugBackend *g_adapter_backend = nullptr;

// ---------------------------------------------------------------------------
// Test helpers (same harness style as test_sound_window.cpp)
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
            long long _e = (long long)(exp); \
            long long _a = (long long)(act); \
            if (_e != _a) { \
                printf("  \033[41;97m FAIL \033[0m %s: expected %lld, got %lld (line %d)\n", \
                       msg, _e, _a, __LINE__); \
                _test_ok = false; \
            } else { \
                printf("  \033[46;30m ok \033[0m %s = %lld\n", msg, _a); \
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

static AudioPortEvent ev(int frame, uint8_t port, uint8_t value)
{
    AudioPortEvent e;
    e.frame = static_cast<uint64_t>(frame);
    e.port  = port;
    e.value = value;
    return e;
}

// ---------------------------------------------------------------------------
// Minimal SMF reader — validates our writer's output and the golden files.
// Explicit-status files only (both the writer here and the py references
// always emit a status byte), which keeps the reader honest and small.
// ---------------------------------------------------------------------------

struct SmfNote {
    int track;
    uint32_t tick;
    int key;
    bool on;
};

static bool readU32BE(const std::vector<uint8_t> &d, size_t off, uint32_t &out)
{
    if (off + 4 > d.size()) return false;
    out = (uint32_t(d[off]) << 24) | (uint32_t(d[off+1]) << 16) |
          (uint32_t(d[off+2]) << 8) | d[off+3];
    return true;
}

struct SmfParsed {
    int format = 0;
    int ntrks  = 0;
    int division = 0;
    std::vector<std::string> trackNames;
    std::vector<SmfNote> notes;
};

static bool parseSmf(const std::vector<uint8_t> &d, SmfParsed &p)
{
    if (d.size() < 14 || memcmp(d.data(), "MThd", 4) != 0) return false;
    uint32_t hdrLen;
    if (!readU32BE(d, 4, hdrLen) || hdrLen < 6) return false;
    p.format   = (d[8] << 8) | d[9];
    p.ntrks    = (d[10] << 8) | d[11];
    p.division = (d[12] << 8) | d[13];

    size_t off = 8 + hdrLen;   // 'MThd'(4) + len(4) + hdrLen bytes of data
    for (int t = 0; t < p.ntrks && off + 8 <= d.size(); ++t) {
        if (memcmp(d.data() + off, "MTrk", 4) != 0) return false;
        uint32_t len;
        if (!readU32BE(d, off + 4, len)) return false;
        size_t i = off + 8, end = i + len;
        if (end > d.size()) return false;
        uint32_t pos = 0;
        uint8_t running = 0;                    // last channel status byte
        p.trackNames.push_back(std::string());
        while (i < end) {
            // delta (vlq)
            uint32_t delta = 0; uint8_t b;
            do { if (i >= end) return false; b = d[i++]; delta = (delta << 7) | (b & 0x7F); }
            while (b & 0x80);
            pos += delta;
            if (i >= end) break;
            uint8_t st;
            if (d[i] >= 0x80) { st = d[i++]; if (st < 0xF0) running = st; }
            else { st = running; }               // running status
            if (st == 0xFF) {                       // meta
                if (i >= end) break;
                uint8_t mt = d[i++];
                uint32_t l = 0;
                do { if (i >= end) return false; b = d[i++]; l = (l << 7) | (b & 0x7F); }
                while (b & 0x80);
                if (mt == 0x03 && i + l <= end)
                    p.trackNames[t] = std::string(reinterpret_cast<const char*>(&d[i]), l);
                i += l;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {         // sysex
                uint32_t l = 0;
                do { if (i >= end) return false; b = d[i++]; l = (l << 7) | (b & 0x7F); }
                while (b & 0x80);
                i += l;
                continue;
            }
            uint8_t kind = st & 0xF0;
            if (i >= end) break;
            if (kind == 0xC0 || kind == 0xD0) {
                i += 1;                             // program/channel: 1 byte
            } else {
                if (i + 1 >= end) break;
                uint8_t d1 = d[i], d2 = d[i + 1];
                i += 2;
                if (kind == 0x90) p.notes.push_back({t, pos, d1, d2 > 0});
                else if (kind == 0x80) p.notes.push_back({t, pos, d1, false});
            }
        }
        off = end;
    }
    return true;
}

static std::vector<uint8_t> readFileBytes(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

// note-on keys of one track, in tick order
static std::vector<int> trackPitchSequence(const SmfParsed &p, int track)
{
    std::vector<int> out;
    for (const auto &n : p.notes)
        if (n.track == track && n.on) out.push_back(n.key);
    return out;
}

// ---------------------------------------------------------------------------
// Part A — pure parser
// ---------------------------------------------------------------------------

static void test_freq_to_midi()
{
    TEST_BEGIN("audio_midi: frequency -> MIDI note");
    using namespace audiomidi;
    CHECK_EQ(69, freqToMidi(440.0), "A4 440 Hz = 69");
    CHECK_EQ(69, freqToMidi(1497600.0 / 3409), "VI53 div 3409 -> 69 (A4)");
    CHECK_EQ(76, freqToMidi(1497600.0 / 2275), "VI53 div 2275 -> 76 (E5)");
    CHECK_EQ(-1, freqToMidi(0.0), "0 Hz rejected");
    CHECK_EQ(-1, freqToMidi(1497600.0), "1.5 MHz out of MIDI range");
    TEST_END();
}

static void test_parser_basic_melody()
{
    TEST_BEGIN("audio_midi: parser — start two notes, stop both");
    using namespace audiomidi;
    // The exact snd_backends.rom sequences (verified live via io_trace):
    // A4 on ctr0, E5 on ctr1; stop via mode-0 CW + zero pair.
    std::vector<AudioPortEvent> events = {
        ev(10, 0x08, 0x36), ev(10, 0x0B, 0x51), ev(10, 0x0B, 0x0D),
        ev(10, 0x08, 0x76), ev(10, 0x0A, 0xE3), ev(10, 0x0A, 0x08),
        ev(40, 0x08, 0x30), ev(40, 0x0B, 0x00), ev(40, 0x0B, 0x00),
        ev(40, 0x08, 0x70), ev(40, 0x0A, 0x00), ev(40, 0x0A, 0x00),
    };
    auto segs = vi53Segments(events);
    CHECK_EQ(2, segs.size(), "two note segments");
    if (segs.size() == 2) {
        CHECK_EQ(0, segs[0].voice, "seg0 is voice 0 (Channel 1)");
        CHECK_EQ(69, segs[0].midiNote, "seg0 pitch = A4");
        CHECK_EQ(10, segs[0].startFrame, "seg0 starts at the load frame");
        CHECK_EQ(40, segs[0].endFrame, "seg0 ends at the stop CW frame");
        CHECK_EQ(1, segs[1].voice, "seg1 is voice 1 (Channel 2)");
        CHECK_EQ(76, segs[1].midiNote, "seg1 pitch = E5");
        CHECK_EQ(40, segs[1].endFrame, "seg1 ends at its stop CW frame");
    }
    TEST_END();
}

static void test_parser_repeats_and_reopen()
{
    TEST_BEGIN("audio_midi: parser — pitch refresh sustains, re-articulation after a rest splits");
    using namespace audiomidi;
    // (a) The player refreshes the same divider: bare reload, then a full
    // CW+data re-program. The square wave never stops and the pitch never
    // changes, so this is ONE sustained note, not three.
    std::vector<AudioPortEvent> events = {
        ev(10, 0x08, 0x36), ev(10, 0x0B, 0x51), ev(10, 0x0B, 0x0D),  // A4
        ev(15, 0x0B, 0x51), ev(15, 0x0B, 0x0D),                      // bare reload
        ev(20, 0x08, 0x36), ev(20, 0x0B, 0x51), ev(20, 0x0B, 0x0D),  // CW re-program
        ev(30, 0x08, 0x30), ev(30, 0x0B, 0x00), ev(30, 0x0B, 0x00),  // stop
    };
    auto segs = vi53Segments(events);
    CHECK_EQ(1, segs.size(), "contiguous same-pitch writes collapse to one note");
    if (segs.size() == 1) {
        CHECK_EQ(10, segs[0].startFrame, "note starts at the first load");
        CHECK_EQ(30, segs[0].endFrame, "note ends at the real stop");
        CHECK_EQ(69, segs[0].midiNote, "pitch A4");
    }
    // (b) Same pitch again AFTER a rest is a new articulation: the silence
    // breaks the merge, so two segments come out.
    std::vector<AudioPortEvent> reArtic = {
        ev(10, 0x08, 0x36), ev(10, 0x0B, 0x51), ev(10, 0x0B, 0x0D),
        ev(20, 0x08, 0x30), ev(20, 0x0B, 0x00), ev(20, 0x0B, 0x00),   // silence
        ev(25, 0x08, 0x36), ev(25, 0x0B, 0x51), ev(25, 0x0B, 0x0D),   // A4 again
        ev(35, 0x08, 0x30), ev(35, 0x0B, 0x00), ev(35, 0x0B, 0x00),
    };
    auto s2 = vi53Segments(reArtic);
    CHECK_EQ(2, s2.size(), "rest between equal pitches keeps two notes");
    if (s2.size() == 2) {
        CHECK_EQ(20, s2[0].endFrame, "first note ends at the silence");
        CHECK_EQ(25, s2[1].startFrame, "second note starts after the rest");
    }
    TEST_END();
}

static void test_parser_mode_gating()
{
    TEST_BEGIN("audio_midi: parser — only square-wave mode 3 makes notes");
    using namespace audiomidi;
    // mode 2 (rate generator, DC out in this emulator): no note
    std::vector<AudioPortEvent> m2 = {
        ev(10, 0x08, 0x34), ev(10, 0x0B, 0x51), ev(10, 0x0B, 0x0D),
    };
    CHECK(vi53Segments(m2).empty(), "mode 2 load produces no note");
    // mode field 111 folds to 3 (emulator: (w>>1)&3): note
    std::vector<AudioPortEvent> m7 = {
        ev(10, 0x08, 0x3E), ev(10, 0x0B, 0x51), ev(10, 0x0B, 0x0D),
    };
    auto segs = vi53Segments(m7);
    CHECK_EQ(1, segs.size(), "mode 7 folded to 3 produces a note");
    if (!segs.empty()) CHECK_EQ(69, segs[0].midiNote, "folded-mode note pitch = A4");
    // zero divider = silence convention
    std::vector<AudioPortEvent> z = {
        ev(10, 0x08, 0x36), ev(10, 0x0B, 0x00), ev(10, 0x0B, 0x00),
    };
    CHECK(vi53Segments(z).empty(), "zero divider produces no note");
    TEST_END();
}

static void test_parser_latch_schemes()
{
    TEST_BEGIN("audio_midi: parser — LSB-only / MSB-only loading schemes");
    using namespace audiomidi;
    // CW 0x1E: ctr0, latch=01 (LSB only), mode 3
    std::vector<AudioPortEvent> lsb = { ev(10, 0x08, 0x1E), ev(10, 0x0B, 0xFF) };
    auto s1 = vi53Segments(lsb);
    CHECK_EQ(1, s1.size(), "LSB-only pair completes in one byte");
    if (!s1.empty()) CHECK_EQ(114, s1[0].midiNote, "LSB 0xFF -> div 255 -> F#7 (114)");
    // CW 0x26: ctr0, latch=10 (MSB only), mode 3
    std::vector<AudioPortEvent> msb = { ev(10, 0x08, 0x26), ev(10, 0x0B, 0x0D) };
    auto s2 = vi53Segments(msb);
    CHECK_EQ(1, s2.size(), "MSB-only pair completes in one byte");
    if (!s2.empty()) CHECK_EQ(69, s2[0].midiNote, "MSB 0x0D -> div 3328 -> note 69");
    TEST_END();
}

static void test_parser_subframe_modulation()
{
    TEST_BEGIN("audio_midi: parser — several loads in one frame make ONE note");
    using namespace audiomidi;
    // Real case from ROTORS.ROM: its swoop/rotor channels reload 2-3
    // dividers per frame without reprogramming the control word. On a 20 ms
    // grid those are not separate pitches — the frame must stay one note and
    // take the newest divider, otherwise the export becomes a machine-gun of
    // 1-frame overlapping notes.
    std::vector<AudioPortEvent> ev;
    // One CW for counter 0 (mode 3, LSB+MSB), then a continuous stream of
    // within-frame re-loads.
    ev.push_back(AudioPortEvent{ 10, 0x08, 0x36 });
    ev.push_back(AudioPortEvent{ 10, 0x0B, 0xB8 });
    ev.push_back(AudioPortEvent{ 10, 0x0B, 0x14 });   // div 0x14B8 = 282 Hz
    ev.push_back(AudioPortEvent{ 10, 0x0B, 0xB8 });
    ev.push_back(AudioPortEvent{ 10, 0x0B, 0x2C });   // same frame: 0x2CB8 = 130.8 Hz
    // Next frame: one more re-load, then the tone is cancelled.
    ev.push_back(AudioPortEvent{ 14, 0x0B, 0xD0 });
    ev.push_back(AudioPortEvent{ 14, 0x0B, 0x1D });   // 0x1DD0 = 196 Hz
    ev.push_back(AudioPortEvent{ 20, 0x08, 0x30 });   // mode 0 = stop
    ev.push_back(AudioPortEvent{ 20, 0x0B, 0x00 });
    ev.push_back(AudioPortEvent{ 20, 0x0B, 0x00 });

    auto segs = vi53Segments(ev);
    CHECK_EQ(2, segs.size(),
             "two notes: the frame with two loads is not split in two");
    if (segs.size() >= 2) {
        CHECK_EQ(10, segs[0].startFrame, "first note opens on its frame");
        CHECK_EQ(14, segs[0].endFrame, "and ends when the frame changes, not mid-frame");
        CHECK_EQ(48, segs[0].midiNote,      // C3, the last divider of frame 10
                 "within-frame note takes the LAST divider");
        CHECK_EQ(14, segs[1].startFrame, "second note starts on the next loaded frame");
        CHECK_EQ(55, segs[1].midiNote, "second note pitch G3 (196 Hz)");
        CHECK_EQ(20, segs[1].endFrame, "and runs to the cancel");
    }
    TEST_END();
}

static void test_parser_tail_and_latch_command()
{
    TEST_BEGIN("audio_midi: parser — sustained tail, latch command keeps note");
    using namespace audiomidi;
    // Note started at 10, nothing else on that voice until stream end at 55:
    // the counter free-runs until the capture stops.
    std::vector<AudioPortEvent> events = {
        ev(10, 0x08, 0x36), ev(10, 0x0B, 0x51), ev(10, 0x0B, 0x0D),
        ev(55, 0x08, 0x76), ev(55, 0x0A, 0xE3), ev(55, 0x0A, 0x08),
    };
    auto segs = vi53Segments(events);
    CHECK_EQ(2, segs.size(), "two voices opened");
    for (const auto &s : segs) {
        if (s.voice == 0) CHECK_EQ(55, s.endFrame, "voice 0 sustains to the stream end");
        else              CHECK(s.endFrame >= s.startFrame + 1, "voice 1 tail is at least 1 frame");
    }
    // Latch-output command (latch bits 00) must NOT close or disturb a note
    std::vector<AudioPortEvent> lc = {
        ev(10, 0x08, 0x36), ev(10, 0x0B, 0x51), ev(10, 0x0B, 0x0D),
        ev(20, 0x08, 0x00),                       // ctr0 latch command
        ev(30, 0x08, 0x30), ev(30, 0x0B, 0x00), ev(30, 0x0B, 0x00),
    };
    auto s2 = vi53Segments(lc);
    CHECK_EQ(1, s2.size(), "latch command does not split the note");
    if (!s2.empty()) {
        CHECK_EQ(10, s2[0].startFrame, "note keeps its original start");
        CHECK_EQ(30, s2[0].endFrame,   "note closes only at the real stop CW");
    }
    TEST_END();
}

// ---------------------------------------------------------------------------
// Part A — SMF writer
// ---------------------------------------------------------------------------

static void test_vlq()
{
    TEST_BEGIN("audio_midi: SMF variable-length quantities");
    using namespace audiomidi;
    std::vector<uint8_t> e0 = {0x00};
    std::vector<uint8_t> e1 = {0x7F};
    std::vector<uint8_t> e2 = {0x81, 0x80, 0x00};   // 16384 (three bytes)
    std::vector<uint8_t> e3 = {0xC0, 0x00};         // 8192
    CHECK(vlq(0) == e0, "vlq(0)");
    CHECK(vlq(0x7F) == e1, "vlq(127)");
    CHECK(vlq(16384) == e2, "vlq(16384)");
    CHECK(vlq(8192) == e3, "vlq(8192)");
    TEST_END();
}

static void test_smf_writer_structure()
{
    TEST_BEGIN("audio_midi: SMF writer — structure round-trips through a parser");
    using namespace audiomidi;
    Config cfg;
    std::vector<NoteSegment> segs = {
        {0, 10, 40, 69},
        {1, 10, 40, 76},
    };
    auto smf = writeSmf(segs, cfg, "test melody");

    SmfParsed p;
    CHECK(parseSmf(smf, p), "written file parses as SMF");
    CHECK_EQ(1, p.format, "format 1");
    CHECK_EQ(3, p.ntrks, "template + 2 voice tracks");
    CHECK_EQ(96, p.division, "ppq = 96 (1 frame = 1 quarter)");
    if (p.trackNames.size() == 3) {
        CHECK(p.trackNames[0] == "test melody", "template track carries the title");
        CHECK(p.trackNames[1] == "VI53 ch 1", "voice track naming");
    }
    // quarter = one frame at 50 fps -> 20000 us
    size_t tempoPos = 0;
    bool tempoOk = false;
    for (size_t i = 0; i + 5 < smf.size(); ++i) {
        if (smf[i] == 0xFF && smf[i+1] == 0x51 && smf[i+2] == 0x03) {
            uint32_t us = (smf[i+3] << 16) | (smf[i+4] << 8) | smf[i+5];
            tempoOk = (us == 20000);
            tempoPos = i;
            break;
        }
    }
    CHECK(tempoOk, "tempo meta = 20000 us/quarter (50 fps)");
    (void)tempoPos;

    // The grab is shifted so the earliest note (frame 10) lands on tick 0;
    // voice0 then runs 0..(40-10)*96 = 2880.
    auto v0 = trackPitchSequence(p, 1);   // first voice track after template
    CHECK_EQ(1, v0.size(), "voice track 1 has one note-on");
    if (!v0.empty()) CHECK_EQ(69, v0[0], "voice track 1 pitch A4");
    bool on69 = false, off69 = false;
    for (const auto &n : p.notes) {
        if (n.track == 1 && n.key == 69 && n.on && n.tick == 0) on69 = true;
        if (n.track == 1 && n.key == 69 && !n.on && n.tick == 2880) off69 = true;
    }
    CHECK(on69, "earliest note starts the file at tick 0");
    CHECK(off69, "note-off keeps the frame-grid duration");
    auto v2 = trackPitchSequence(p, 2);
    CHECK_EQ(1, v2.size(), "voice track 2 has one note-on");
    if (!v2.empty()) CHECK_EQ(76, v2[0], "voice track 2 pitch E5");
    TEST_END();
}

static void test_smf_time_base()
{
    TEST_BEGIN("audio_midi: SMF writer — a late grab starts at tick 0, not after silence");
    using namespace audiomidi;
    // A grab taken minutes into a session carries absolute frame numbers
    // (the board frame counter is never zeroed). The melody must still start
    // at the beginning of the file, keeping the spacing between notes.
    std::vector<NoteSegment> segs = {
        {0, 90000, 90012, 69},      // A4, 12 frames
        {0, 90020, 90030, 76},      // E5, 8 frames after a 8-frame gap
    };
    auto smf = writeSmf(segs, Config(), "late grab");
    SmfParsed p;
    CHECK(parseSmf(smf, p), "file parses");
    int firstOn = -1, secondOn = -1, firstOff = -1;
    for (const auto &n : p.notes) {
        if (n.track != 1) continue;
        if (n.on && n.key == 69) firstOn = (int)n.tick;
        if (n.on && n.key == 76) secondOn = (int)n.tick;
        if (!n.on && n.key == 69) firstOff = (int)n.tick;
    }
    CHECK_EQ(0, firstOn, "first note-on sits at tick 0");
    CHECK_EQ(12 * 96, firstOff, "duration keeps the frame-grid length");
    CHECK_EQ(20 * 96, secondOn, "gap between notes preserved (20 frames from base)");
    TEST_END();
}

static void test_smf_empty_segments()
{
    TEST_BEGIN("audio_midi: SMF writer — empty grab still produces a valid file");
    using namespace audiomidi;
    auto smf = writeSmf({}, Config(), "silent");
    SmfParsed p;
    CHECK(parseSmf(smf, p), "parses");
    CHECK_EQ(1, p.format, "format 1");
    CHECK_EQ(1, p.ntrks, "template track only");
    CHECK(p.notes.empty(), "no notes");
    TEST_END();
}

// ---------------------------------------------------------------------------
// Part B — DebugAdapter grab capture
// ---------------------------------------------------------------------------

static void test_grab_capture_and_filtering(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: audio grab captures VI53 ports only");

    adapter.setAudioGrabEnabled(true);
    CHECK(adapter.isAudioGrabEnabled(), "grab flag readable through the target");

    // Noise writes that must NOT be captured.
    adapter.writeIoPort(0x01, 0x01);      // PIA port C
    adapter.writeIoPort(0x00, 0x02);      // PIA BSR
    adapter.writeIoPort(0x14, 0x07);      // AY latch
    adapter.writeIoPort(0x15, 0x00);      // AY data
    CHECK_EQ(0, adapter.audioGrabEventCount(), "non-VI53 writes ignored");

    // One VI53 note start: CW + LSB + MSB.
    adapter.writeIoPort(0x08, 0x36);
    adapter.writeIoPort(0x0B, 0x51);
    adapter.writeIoPort(0x0B, 0x0D);
    CHECK_EQ(3, adapter.audioGrabEventCount(), "three VI53 writes captured");

    auto events = adapter.audioGrabEvents();
    CHECK_EQ(0x36, events[0].value, "control word byte");
    CHECK_EQ(0x0B, events[1].port, "counter port 0x0B");

    // Re-enabling starts a fresh buffer.
    adapter.setAudioGrabEnabled(true);
    CHECK_EQ(0, adapter.audioGrabEventCount(), "re-arming the grab clears the buffer");

    // A completed melody+stop survives while the grab is OFF (export window).
    adapter.setAudioGrabEnabled(true);
    adapter.writeIoPort(0x08, 0x36);
    adapter.writeIoPort(0x0B, 0x51);
    adapter.writeIoPort(0x0B, 0x0D);
    adapter.setAudioGrabEnabled(false);
    adapter.writeIoPort(0x08, 0x30);       // after stop: not recorded
    CHECK(!adapter.isAudioGrabEnabled(), "grab off");
    CHECK_EQ(3, adapter.audioGrabEventCount(), "buffer survives disabling for export");

    adapter.setAudioGrabEnabled(false);
    adapter.setAudioGrabEnabled(true);     // re-arm clears
    adapter.setAudioGrabEnabled(false);
    TEST_END();
}

static void test_grab_frames_and_end_to_end(DebugAdapter &adapter)
{
    TEST_BEGIN("DebugAdapter: grab frames notes; end-to-end to SMF bytes");

    adapter.reset(false);                  // detach boot, page 0 = NOP field
    adapter.setAudioGrabEnabled(true);

    adapter.writeIoPort(0x08, 0x36);       // A4 on ctr0
    adapter.writeIoPort(0x0B, 0x51);
    adapter.writeIoPort(0x0B, 0x0D);
    for (int i = 0; i < 30; ++i) adapter.executeFrame();
    adapter.writeIoPort(0x08, 0x30);       // stop
    adapter.writeIoPort(0x0B, 0x00);
    adapter.writeIoPort(0x0B, 0x00);
    adapter.setAudioGrabEnabled(false);

    auto events = adapter.audioGrabEvents();
    CHECK_EQ(6, events.size(), "six VI53 writes captured");
    CHECK(events.back().frame > events.front().frame,
          "frame stamps advance with executeFrame()");

    auto segs = audiomidi::vi53Segments(events);
    CHECK_EQ(1, segs.size(), "one note reconstructed");
    if (segs.size() == 1) {
        CHECK_EQ(69, segs[0].midiNote, "pitch A4");
        CHECK(segs[0].endFrame - segs[0].startFrame >= 25,
              "duration tracks the ~30 frames of execution");
    }

    auto smf = audiomidi::writeSmf(segs, audiomidi::Config(), "adapter e2e");
    SmfParsed p;
    CHECK(parseSmf(smf, p), "e2e output parses as SMF");
    CHECK_EQ(1, trackPitchSequence(p, 1).size(), "e2e voice track has the A4 note");
    TEST_END();
}

// Note: a keyboard-driven run of snd_backends.rom is deliberately NOT tested
// here. That ROM steps its melody from the frame interrupt handler
// (frame_handler -> sound_tick), and its main loop parks the CPU in HLT
// between frames; delivering RST 7 to a halted CPU only happens on the
// emulation-thread path, not through the headless executeFrame() loop used
// by these tests (verified: PC stays on the HLT opcode, IFF set, no VI53
// writes). Real-ROM coverage lives in Part C, which runs PUTUP/RISEOUT
// attract music through the same grab -> parser -> SMF pipeline.

// ---------------------------------------------------------------------------
// Part C — golden reference vs score-derived MID (guarded)
// ---------------------------------------------------------------------------

// The two ROMs play their title theme automatically (attract mode), so a
// frame loop + grab is all that is needed. The golden .mid files were
// exported from the scores in the ROMs themselves (music2midi.py), so they
// carry the melody's pitches and their order — the ground truth for our
// divider -> frequency -> note conversion.
//
// What is compared: the sequence of PITCH CHANGES (consecutive equal pitches
// collapsed) of the captured voice against the reference track, allowing the
// capture to start with extra material (attract-mode intro jingles) and the
// theme to begin at a later note. What is deliberately NOT compared: note
// durations and the grouping of repeated pitches — the reference uses score
// ticks, we quantise on the VBlank frame grid, and the ROM refreshes a
// divider several times per step (which our parser sustains as one note).
static std::vector<int> pitchChanges(const std::vector<int> &seq)
{
    std::vector<int> out;
    for (int p : seq)
        if (out.empty() || out.back() != p) out.push_back(p);
    return out;
}

static void runGoldenCompare(DebugAdapter &adapter,
                             const std::string &rom,
                             const std::string &mid,
                             const std::string &voiceTag,   // substring of track name
                             int ourVoice,
                             int notesToCheck)
{
    if (access(rom.c_str(), R_OK) != 0 || access(mid.c_str(), R_OK) != 0) {
        printf("\n\033[0;35m=== TEST: golden compare %s ===\033[0m\n", voiceTag.c_str());
        printf("  \033[43;30m SKIP \033[0m ROM or reference .mid not present\n");
        return;
    }

    TEST_BEGIN(("golden compare: " + rom.substr(rom.rfind('/') + 1) +
                " vs " + mid.substr(mid.rfind('/') + 1)).c_str());

    // Reference pitch sequence from the named track of the golden MID.
    auto midBytes = readFileBytes(mid);
    SmfParsed ref;
    CHECK(parseSmf(midBytes, ref), "reference .mid parses");
    int refTrack = -1;
    for (size_t t = 0; t < ref.trackNames.size(); ++t) {
        if (ref.trackNames[t].find(voiceTag) != std::string::npos) { refTrack = (int)t; break; }
    }
    CHECK(refTrack >= 0, "reference track found by name");
    // Harness note: TEST_BEGIN/TEST_END open a do{}..while(0) block, so no
    // early return is allowed here — guard the comparison instead.
    if (refTrack >= 0) {
    auto refRle = pitchChanges(trackPitchSequence(ref, refTrack));
    CHECK((int)refRle.size() >= notesToCheck, "reference long enough");

    // Live capture from the ROM.
    CHECK(adapter.loadRom(rom, 0), "ROM loaded");
    adapter.setAudioGrabEnabled(true);
    for (int i = 0; i < 1500; ++i) adapter.executeFrame();   // 30 s of attract music
    adapter.setAudioGrabEnabled(false);

    auto segs = audiomidi::vi53Segments(adapter.audioGrabEvents());
    std::vector<int> ourSeq;
    for (const auto &s : segs)
        if (s.voice == ourVoice) ourSeq.push_back(s.midiNote);
    auto ourRle = pitchChanges(ourSeq);

    printf("      %zu captured voice-%d notes -> %zu pitch changes; reference has %zu\n",
           ourSeq.size(), ourVoice, ourRle.size(), refRle.size());
    printf("      GOLD: "); for (size_t i = 0; i < refRle.size() && i < 16; ++i) printf("%d ", refRle[i]); printf("\n");
    printf("      OURS: "); for (size_t i = 0; i < ourRle.size() && i < 16; ++i) printf("%d ", ourRle[i]); printf("\n");

    CHECK((int)ourRle.size() >= notesToCheck, "capture long enough to compare");

    // Align: the theme may start after intro jingles, so search the first
    // window of our pitch-change stream that reproduces the reference.
    int alignAt = -1;
    for (int o = 0; alignAt < 0 && o + notesToCheck <= (int)ourRle.size(); ++o) {
        bool same = true;
        for (int i = 0; i < notesToCheck; ++i)
            if (ourRle[o + i] != refRle[i]) { same = false; break; }
        if (same) alignAt = o;
    }
    if (alignAt < 0) {
        printf("      no window of %d pitch changes matches the reference\n", notesToCheck);
    } else {
        printf("      theme aligns at captured pitch change #%d, %d/%d match\n",
               alignAt, notesToCheck, notesToCheck);
    }
    CHECK(alignAt >= 0,
          "captured pitch-change sequence reproduces the score-derived reference");

    // The export chain must also hold for the real capture, not only for
    // synthetic segments.
    auto smf = audiomidi::writeSmf(segs, audiomidi::Config(), "golden");
    SmfParsed round;
    CHECK(parseSmf(smf, round), "captured melody exports to a parseable SMF");
    CHECK_EQ(1, round.format, "exported SMF is format 1");
    CHECK(round.notes.size() >= ourSeq.size(), "exported SMF carries the notes");
    bool startsAtZero = false;
    for (const auto &n : round.notes)
        if (n.on && n.tick == 0) { startsAtZero = true; break; }
    CHECK(startsAtZero, "real ROM export begins at tick 0 (frame base shifted)");
    }
    TEST_END();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("\033[0;36m=== Audio MIDI Grab Tests ===\033[0m\n");

    // --- Part A: pure functions ---
    test_freq_to_midi();
    test_parser_basic_melody();
    test_parser_repeats_and_reopen();
    test_parser_mode_gating();
    test_parser_subframe_modulation();
    test_parser_latch_schemes();
    test_parser_tail_and_latch_command();
    test_vlq();
    test_smf_writer_structure();
    test_smf_time_base();
    test_smf_empty_segments();

    // --- Part B: real DebugAdapter ---
    printf("\n  Setting up headless DebugAdapter...\n");
    Options.novideo = true;
    Options.nosound = true;
    Options.pc = 0;

    static DebugAdapter adapter;   // leaked deliberately at exit
    adapter.init();
    adapter.bindHal();
    printf("  DebugAdapter ready\n");

    test_grab_capture_and_filtering(adapter);
    test_grab_frames_and_end_to_end(adapter);

    // --- Part C: golden references (guarded) ---
    runGoldenCompare(adapter,
        "/home/alexey/Projects/vector-games/roms/redesign/riseout/src/riseout.rom",
        "/home/alexey/Projects/vector-games/roms/redesign/riseout/riseout_title.mid",
        "cnt0", 0, 16);
    runGoldenCompare(adapter,
        "/home/alexey/Projects/vector-games/roms/redesign/putup/src/putup.rom",
        "/home/alexey/Projects/vector-games/roms/redesign/putup/putup_title.mid",
        "ch0", 0, 16);

    adapter.shutdown();

    printf("\n\033[0;36m=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", \033[41;97m %d FAILED \033[0m\033[0;36m", tests_failed);
    }
    printf(" ===\033[0m\n\n");

    return tests_failed > 0 ? 1 : 0;
}
