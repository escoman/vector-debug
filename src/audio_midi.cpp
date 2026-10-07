#include "audio_midi.h"

#include <algorithm>
#include <cmath>
#include <map>

// ---------------------------------------------------------------------------
// audiomidi — see audio_midi.h for the design notes.
// Pure functions: no emulator, no ImGui, no filesystem. Deterministic input
// (the captured event list) → deterministic bytes, so unit tests can replay
// ROM port sequences without booting anything.
// ---------------------------------------------------------------------------

namespace audiomidi {

int freqToMidi(double freq, double a4)
{
    if (freq <= 0.0) return -1;
    // Round to the nearest semitone of the equal-tempered scale.
    double n = 69.0 + 12.0 * std::log2(freq / a4);
    int note = static_cast<int>(std::lround(n));
    if (note < 0 || note > 127) return -1;   // unwritable in SMF
    return note;
}

std::vector<uint8_t> vlq(uint32_t value)
{
    // Standard SMF variable-length quantity: 7 bits per byte, MSB = continue.
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(value & 0x7F));
    value >>= 7;
    while (value) {
        out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Parser: raw port writes → note segments (8253 protocol decoded here)
// ---------------------------------------------------------------------------

namespace {

struct VoiceState {
    int  latch    = 3;    // counter loading scheme from the CW (1=LSB,2=MSB,3=LSB+MSB)
    bool tone     = false; // CW selected square-wave mode 3 (field folds 7→3)
    bool pending  = false; // LSB of a pending LSB+MSB pair
    uint8_t lsb   = 0;
    uint16_t lastLoad = 0; bool haveLast = false; // repeat-load collapsing
    int  openStart = -1;   // currently sounding note (frame), -1 = none
    int  openMidi  = -1;
    double openHz  = 0.0;  // its measured frequency (glides work in cents)
};

// vio.h routes ~port&3 to the chip: 0x0B→ctr0, 0x0A→ctr1, 0x09→ctr2.
// Returns voice index 0..2 or -1 for any other port.
int voiceForDataPort(uint8_t port)
{
    if (port < 0x09 || port > 0x0B) return -1;
    return static_cast<int>((~port) & 3);
}

void closeNote(VoiceState &v, int vi, int frame, std::vector<NoteSegment> &out)
{
    if (v.openStart < 0) return;
    int end = std::max(frame, v.openStart + 1); // never zero-length
    NoteSegment seg{ vi, v.openStart, end, v.openMidi, v.openHz, {} };
    out.push_back(seg);
    v.openStart = -1;
    v.openMidi  = -1;
    v.openHz    = 0.0;
}

} // namespace

std::vector<NoteSegment> vi53Segments(const std::vector<AudioPortEvent> &events,
                                      const Config &cfg)
{
    std::vector<NoteSegment> segs;
    VoiceState voice[3];
    int lastFrame = -1;

    for (const auto &e : events) {
        lastFrame = static_cast<int>(e.frame);

        if (e.port == 0x08) {
            // Control word. ctr select (7-6) == 3 is the read-back command —
            // the Vector ROMs never use it and vio.h maps nothing there.
            int ctr = (e.value >> 6) & 3;
            if (ctr >= 3) continue;
            int latch = (e.value >> 4) & 3;
            if (latch == 0) continue;  // latch-outputs command: keeps running
            VoiceState &v = voice[ctr];
            // SetMode(): the counter stops whatever it was generating — this
            // is the note-off frame (also for a pure re-trigger CW+data pair:
            // the data load below re-opens the note on the same frame).
            closeNote(v, ctr, static_cast<int>(e.frame), segs);
            v.latch    = latch;
            v.tone     = ((e.value >> 1) & 3) == 3; // mode field 011 or 111
            v.pending  = false;
            v.haveLast = false;  // reprogram: same divider again = a new note
            continue;
        }

        int vi = voiceForDataPort(e.port);
        if (vi < 0) continue;
        VoiceState &v = voice[vi];

        uint16_t value;
        if (v.latch == 3) {
            if (!v.pending) { v.pending = true; v.lsb = e.value; continue; }
            v.pending = false;
            value = static_cast<uint16_t>(v.lsb | (e.value << 8));
        } else if (v.latch == 1) {
            value = e.value;                           // LSB only
        } else if (v.latch == 2) {
            value = static_cast<uint16_t>(e.value << 8); // MSB only
        } else {
            continue;                                  // latch==0: no load
        }

        if (value == 0) continue;                      // 0 = silence convention
        if (v.haveLast && v.lastLoad == value) continue; // repeat within step
        v.lastLoad = value; v.haveLast = true;
        if (!v.tone) continue;                         // only mode 3 toggles out

        int midi = freqToMidi(static_cast<double>(cfg.timerHz) / value, cfg.a4Hz);
        if (midi < 0) continue;                        // out of SMF range
        const double hz = static_cast<double>(cfg.timerHz) / value;

        // A data load without an intervening CW that changes the pitch ends
        // the old note where the new divider takes over (rare: ROMs always
        // reprogram via CW first).
        //
        // Exception: several loads inside ONE frame. The grid cannot separate
        // them (closing here would yield a 1-frame note stacked on another
        // note with the same start frame), and the chip does not sound them as
        // discrete pitches either — it glides through them within 20 ms. So the
        // frame keeps its single note and adopts the newest divider as pitch.
        // ROMs that sweep pitch this way (ROTORS rotor/swoop channels reload 2
        // or 3 dividers per frame) otherwise export as a machine-gun burst.
        const int f = static_cast<int>(e.frame);
        if (v.openStart == f) {
            v.openMidi = midi;
            v.openHz = hz;
            continue;
        }
        closeNote(v, vi, f, segs);
        v.openStart = f;
        v.openMidi  = midi;
        v.openHz    = hz;
    }

    // A note still sounding when the capture stopped sustained until the grab
    // ended — the counter free-runs until it is reprogrammed.
    for (int vi = 0; vi < 3; ++vi) {
        VoiceState &v = voice[vi];
        if (v.openStart < 0) continue;
        int end = std::max(v.openStart + cfg.minNoteFrames, lastFrame);
        segs.push_back(NoteSegment{ vi, v.openStart, end, v.openMidi, v.openHz, {} });
    }

    std::sort(segs.begin(), segs.end(), [](const NoteSegment &a, const NoteSegment &b) {
        return a.startFrame != b.startFrame ? a.startFrame < b.startFrame
                                            : a.voice < b.voice;
    });

    // ------------------------------------------------------------------
    // Collapse back-to-back re-programming of the same pitch on the same
    // counter. Melody players commonly refresh the divider every frame (or
    // twice per step); the i8253 square-wave output only restarts its phase,
    // so nothing is heard as a new note — one sustained tone runs until the
    // pitch changes or the channel is silenced. Only segments that touch (no
    // silence in between) merge, so a real re-articulation after a rest still
    // yields two notes. This is the py reference's `last_load` collapsing
    // expressed on segments instead of raw writes.
    // ------------------------------------------------------------------
    {
        std::vector<NoteSegment> merged;
        merged.reserve(segs.size());
        for (int vi = 0; vi < 3; ++vi) {
            int lastIdx = -1;
            for (const auto &s : segs) {          // segs is sorted by startFrame
                if (s.voice != vi) continue;
                if (lastIdx >= 0 && s.midiNote == merged[lastIdx].midiNote &&
                    s.startFrame <= merged[lastIdx].endFrame) {
                    if (s.endFrame > merged[lastIdx].endFrame)
                        merged[lastIdx].endFrame = s.endFrame;
                    continue;
                }
                merged.push_back(s);
                lastIdx = static_cast<int>(merged.size()) - 1;
            }
        }
        segs.swap(merged);
    }

    std::sort(segs.begin(), segs.end(), [](const NoteSegment &a, const NoteSegment &b) {
        return a.startFrame != b.startFrame ? a.startFrame < b.startFrame
                                            : a.voice < b.voice;
    });

    // ------------------------------------------------------------------
    // Optional: fold a staircase of quick small steps back into the slide it
    // sounds like. One held note then carries pitch-bend points at the real
    // measured frequencies, so an effect sweep stops being 30 separate notes.
    // A run qualifies when its notes touch (no rest), are short and move by at
    // most a couple of semitones at a time; anything else keeps the plain
    // note-per-segment representation.
    // ------------------------------------------------------------------
    if (cfg.glideSweeps) {
        std::vector<NoteSegment> glided;
        glided.reserve(segs.size());

        for (int vi = 0; vi < 3; ++vi) {
            std::vector<const NoteSegment *> run;
            auto flush = [&glided, &run, &cfg]() {
                if (static_cast<int>(run.size()) < cfg.glideMinNotes) {
                    for (const NoteSegment *s : run) glided.push_back(*s);
                    run.clear();
                    return;
                }
                NoteSegment held = *run.front();
                held.endFrame = run.back()->endFrame;
                const double base = held.hz > 0.0 ? held.hz : 1.0;
                for (size_t k = 1; k < run.size(); ++k) {
                    const double hz = run[k]->hz > 0.0 ? run[k]->hz : base;
                    const int cents = static_cast<int>(
                        std::lround(1200.0 * std::log2(hz / base)));
                    held.bends.emplace_back(run[k]->startFrame, cents);
                }
                glided.push_back(held);
                run.clear();
            };

            for (const auto &s : segs) {          // segs is sorted by startFrame
                if (s.voice != vi) continue;
                const bool stepOk =
                    run.empty() ||
                    (s.startFrame <= run.back()->endFrame &&
                     std::abs(s.midiNote - run.back()->midiNote) <= cfg.glideMaxStep &&
                     s.endFrame - s.startFrame <= cfg.glideMaxNoteFrames &&
                     run.front()->endFrame - run.front()->startFrame <= cfg.glideMaxNoteFrames);
                if (!stepOk) flush();
                run.push_back(&s);
            }
            flush();
        }
        segs.swap(glided);
    }

    return segs;
}

// ---------------------------------------------------------------------------
// SMF format-1 writer
// ---------------------------------------------------------------------------

namespace {

void pushBE(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void pushBE32(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void pushChunk(std::vector<uint8_t> &out, const char *id,
               const std::vector<uint8_t> &body)
{
    out.insert(out.end(), id, id + 4);
    pushBE32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

// Meta event: delta(0) FF <type> <len> <data...>
std::vector<uint8_t> metaEvent(uint8_t type, const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> ev;
    ev.push_back(0x00);            // delta
    ev.push_back(0xFF);
    ev.push_back(type);
    auto l = vlq(static_cast<uint32_t>(data.size()));
    ev.insert(ev.end(), l.begin(), l.end());
    ev.insert(ev.end(), data.begin(), data.end());
    return ev;
}

std::vector<uint8_t> trackNameEvent(const std::string &name)
{
    return metaEvent(0x03, std::vector<uint8_t>(name.begin(), name.end()));
}

std::vector<uint8_t> endOfTrack()
{
    return metaEvent(0x2F, {});
}

// One track event with its absolute tick and payload bytes.
// `order` breaks ties at the same tick: note-offs sort before note-ons.
struct TickEvent {
    uint32_t tick;
    int order;
    std::vector<uint8_t> bytes;
};

void serializeTrackEvents(std::vector<uint8_t> &body, std::vector<TickEvent> &events)
{
    std::stable_sort(events.begin(), events.end(),
                     [](const TickEvent &a, const TickEvent &b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.order < b.order;
    });
    uint32_t position = 0;
    for (const auto &ev : events) {
        auto d = vlq(ev.tick - position);
        body.insert(body.end(), d.begin(), d.end());
        body.insert(body.end(), ev.bytes.begin(), ev.bytes.end());
        position = ev.tick;
    }
}

} // namespace

std::vector<uint8_t> writeSmf(const std::vector<NoteSegment> &segments,
                              const Config &cfg,
                              const std::string &title)
{
    // One track per sounding voice; MIDI channel = track order.
    std::map<int, std::vector<const NoteSegment*>> byVoice;
    for (const auto &s : segments) byVoice[s.voice].push_back(&s);

    // Frame numbers come from the board's frame counter, which counts from
    // program start (Board::reset() does not zero it). Shifting the whole
    // grab so the earliest note sits at tick 0 keeps the file playable from
    // bar 1 instead of beginning with minutes of silence; inter-note spacing
    // is preserved because every frame is moved by the same base.
    int baseFrame = 0;
    for (const auto &s : segments)
        if (baseFrame == 0 || s.startFrame < baseFrame) baseFrame = s.startFrame;

    const uint32_t tempoUs = static_cast<uint32_t>(
        (1000000.0 / static_cast<double>(cfg.fps)) + 0.5); // quarter = 1 frame

    // --- template track: name, tempo, time signature ---
    std::vector<uint8_t> tmpl;
    auto append = [&tmpl](const std::vector<uint8_t> &ev) {
        tmpl.insert(tmpl.end(), ev.begin(), ev.end());
    };
    append(trackNameEvent(title));
    append(metaEvent(0x51, { static_cast<uint8_t>(tempoUs >> 16),
                             static_cast<uint8_t>(tempoUs >> 8),
                             static_cast<uint8_t>(tempoUs) }));
    append(metaEvent(0x58, { 0x04, 0x02, 0x18, 0x08 }));   // 4/4 like the refs
    append(endOfTrack());

    // --- MThd + template track ---
    std::vector<uint8_t> smf;
    smf.insert(smf.end(), { 'M', 'T', 'h', 'd' });
    pushBE32(smf, 6);                                    // header data length
    pushBE(smf, 1);                                      // format 1
    pushBE(smf, static_cast<uint16_t>(1 + byVoice.size()));
    pushBE(smf, static_cast<uint16_t>(cfg.ppq));
    pushChunk(smf, "MTrk", tmpl);

    int chIdx = 0;
    for (auto &kv : byVoice) {
        const int voice = kv.first;
        std::vector<TickEvent> events;

        TickEvent prog;
        prog.tick = 0; prog.order = -1;
        prog.bytes = { static_cast<uint8_t>(0xC0 | (chIdx & 0x0F)), cfg.program };
        events.push_back(prog);

        // A bend is meaningless without the range it is measured in: announce
        // RPN 0 (pitch bend sensitivity) for this channel, then release the RPN
        // so later data bytes are not read as a parameter change.
        bool anyBend = false;
        for (const NoteSegment *s : kv.second)
            if (!s->bends.empty()) { anyBend = true; break; }
        const int bendRange = std::max(1, cfg.glideBendRange);
        if (anyBend) {
            const uint8_t ch = static_cast<uint8_t>(0xB0 | (chIdx & 0x0F));
            const uint8_t semis = static_cast<uint8_t>(bendRange);
            for (const std::vector<uint8_t> &cc : {
                     std::vector<uint8_t>{ ch, 0x65, 0x00 },
                     std::vector<uint8_t>{ ch, 0x64, 0x00 },
                     std::vector<uint8_t>{ ch, 0x06, semis },
                     std::vector<uint8_t>{ ch, 0x20, 0x00 },
                     std::vector<uint8_t>{ ch, 0x65, 0x7F },
                     std::vector<uint8_t>{ ch, 0x64, 0x7F } }) {
                TickEvent rpn;
                rpn.tick = 0; rpn.order = -1;
                rpn.bytes = cc;
                events.push_back(rpn);
            }
        }

        auto bendEvent = [chIdx, bendRange](uint32_t tick, int cents) {
            // 14-bit bend: 0x2000 center, full scale = +/- glideBendRange
            // semitones. Saturate rather than wrap: an out-of-range sweep must
            // stay at the extreme instead of jumping to the other side.
            double units = static_cast<double>(cents) / (100.0 * bendRange) * 8192.0;
            int v = static_cast<int>(units);
            if (v > 8191) v = 8191;
            if (v < -8192) v = -8192;
            int raw = 8192 + v;
            TickEvent b;
            b.tick = tick; b.order = 0;
            b.bytes = { static_cast<uint8_t>(0xE0 | (chIdx & 0x0F)),
                        static_cast<uint8_t>(raw & 0x7F),
                        static_cast<uint8_t>((raw >> 7) & 0x7F) };
            return b;
        };

        for (const NoteSegment *s : kv.second) {
            TickEvent on;
            on.tick = static_cast<uint32_t>(s->startFrame - baseFrame) * cfg.ppq;
            on.order = 1;
            on.bytes = { static_cast<uint8_t>(0x90 | (chIdx & 0x0F)),
                         static_cast<uint8_t>(s->midiNote),
                         static_cast<uint8_t>(cfg.velocity) };
            events.push_back(on);

            for (const auto &bp : s->bends) {
                const uint32_t t =
                    static_cast<uint32_t>(bp.first - baseFrame) * cfg.ppq;
                if (t < on.tick) continue;      // never bend before the note
                events.push_back(bendEvent(t, bp.second));
            }

            TickEvent off;
            off.tick = static_cast<uint32_t>(s->endFrame - baseFrame) * cfg.ppq;
            off.order = 0;
            off.bytes = { static_cast<uint8_t>(0x80 | (chIdx & 0x0F)),
                          static_cast<uint8_t>(s->midiNote), 0x00 };
            events.push_back(off);

            // Release the bend with the note, otherwise the next one starts
            // detuned by whatever the sweep ended on.
            if (!s->bends.empty())
                events.push_back(bendEvent(off.tick, 0));
        }

        std::vector<uint8_t> body;
        auto name = trackNameEvent("VI53 ch " + std::to_string(voice + 1));
        body.insert(body.end(), name.begin(), name.end());
        serializeTrackEvents(body, events);
        auto eot = endOfTrack();
        body.insert(body.end(), eot.begin(), eot.end());
        pushChunk(smf, "MTrk", body);
        ++chIdx;
    }

    return smf;
}

} // namespace audiomidi
