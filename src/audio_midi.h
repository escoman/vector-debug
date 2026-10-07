#pragma once

#include "debugger_types.h"   // AudioPortEvent

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// audio_midi — VI53 port-trace → Standard MIDI File (pure, no ImGui/emulator)
//
// C++ port of the Layer-A logic in vector-games/utils/analyze/porttrace2midi.py:
// the sound on a Vector-06C is generated EXACTLY by the sequence of OUTs into
// the KR580VI53 ports, so a melody can be reconstructed without knowing the
// ROM's score format — decode the counter bytes, f = timer_hz / value.
//
// Improvements over the py reference:
//   * frame stamps come from the emulator's own frame counter taken at write
//     time (the py version had a VBlank breakpoint + MCP drain per frame);
//   * the control word is decoded too: a mode-set CW CLOSES the open note
//     (that is where the emulator's CounterUnit stops toggling `out`), so
//     note-offs land on real cancel frames instead of "until next note";
//   * the counter's latch scheme (LSB+MSB / LSB / MSB) and mode are taken
//     from the CW; only square-wave mode 3 (field bits 011 or 111, the
//     emulator folds 7→3) produces notes.
//
// The output is SMF format 1: a template track (name + tempo + time
// signature) plus one track per sounding VI53 voice, one VBlank frame = one
// quarter note at ppq ticks.
// ---------------------------------------------------------------------------

namespace audiomidi {

struct Config {
    uint32_t timerHz  = 1497600;  // KR580VI53 clock input (16 × 93.6 kHz)
    double   a4Hz     = 440.0;    // MIDI note 69 reference
    int      fps      = 50;       // Vector-06C VBlank rate → frame grid
    int      ppq      = 96;       // ticks per quarter; one quarter = one frame
    int      minNoteFrames = 1;   // shortest emitted note
    int      velocity = 100;      // fixed — the VI53 has no amplitude
    uint8_t  program  = 0;        // MIDI program for every voice track
};

// One reconstructed note. Frames are indices on the capture grid; MIDI ticks
// are frame * ppq. endFrame is exclusive, like the py segment list.
struct NoteSegment {
    int voice;        // 0..2 = counter 0..2 (Sound window "i8253 Channel 1..3")
    int startFrame;
    int endFrame;
    int midiNote;     // 0..127
};

// Frequency → nearest MIDI note number (equal temperament).
int freqToMidi(double freq, double a4 = 440.0);

// Raw captured port writes → note segments, chronologically ordered.
// `events` must be in write order (the grab buffer is). Value-0 loads,
// repeated loads of the same divider, non-tone modes and counter-3/CW
// specials never produce notes.
std::vector<NoteSegment> vi53Segments(const std::vector<AudioPortEvent> &events,
                                      const Config &cfg = {});

// Note segments → SMF format-1 file bytes. Voices with no segments get no
// track. `title` goes into the template track's sequence-name meta.
std::vector<uint8_t> writeSmf(const std::vector<NoteSegment> &segments,
                              const Config &cfg,
                              const std::string &title);

// SMF variable-length quantity (exposed for tests and future writers).
std::vector<uint8_t> vlq(uint32_t value);

} // namespace audiomidi
