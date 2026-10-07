#pragma once

#include "debugger_types.h"   // AudioPortEvent

#include <cstdint>
#include <string>
#include <utility>
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

    // Glide sweeps (experimental, off by default). A ROM that walks the
    // divider one or two semitones at a time (ROTORS rotor/swoop effects)
    // really sounds a continuous slide, but the frame grid can only hold one
    // pitch per frame, so the plain reconstruction exports it as a staircase
    // of short notes. When enabled, such a run is merged into ONE held note
    // carrying MIDI pitch-bend events between its real measured frequencies.
    bool glideSweeps    = false;
    int  glideMaxStep   = 2;      // max semitone gap between consecutive notes
    int  glideMinNotes  = 4;      // a run must be at least this long
    int  glideMaxNoteFrames = 12; // steps must be this short (~0.25 s): longer
                                  // notes carry the tune, so they stay separate
    int  glideBendRange = 12;     // semitones of bend range announced via RPN
};

// One reconstructed note. Frames are indices on the capture grid; MIDI ticks
// are frame * ppq. endFrame is exclusive, like the py segment list.
struct NoteSegment {
    int voice;        // 0..2 = counter 0..2 (Sound window "i8253 Channel 1..3")
    int startFrame;
    int endFrame;
    int midiNote;     // 0..127
    // Measured pitch of the note in Hz (0 when unknown). The rounded MIDI note
    // loses the sub-semitone detail that a slide is made of, so the glide pass
    // works on these frequencies instead.
    double hz = 0.0;
    // Pitch-bend points for a merged glide: (frame, cents relative to the
    // frequency the note started on). Empty for ordinary notes.
    std::vector<std::pair<int, int>> bends;
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
