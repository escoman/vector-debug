#include "sound_window.h"

#include "audio_midi.h"

// Dear ImGui
#include "imgui.h"
#include "imgui_internal.h"  // for ImDrawList access

#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <fstream>

// ---------------------------------------------------------------------------
// Channel metadata — UI order is fixed (see sound_window.h):
//   Standard Noise Channel
//   i8253 Channel 1 / 2 / 3
//   AY Noise Channel
//   AY Channel A / B / C
// ---------------------------------------------------------------------------

static const char *kChannelNames[SoundWindow::NUM_CHANNELS] = {
    "Tape Out (PC0)",  // PIA1 Port C bit 0 (tape-out beeper)
    "i8253 Channel 1",
    "i8253 Channel 2",
    "i8253 Channel 3",
    "AY Noise Channel",        // AY-3-8912 noise generator
    "AY Channel A",
    "AY Channel B",
    "AY Channel C",
};

static const char *kChannelSections[SoundWindow::NUM_CHANNELS] = {
    "Tape Out (PC0)",   // 0
    "i8253", "i8253", "i8253",  // 1-3
    "AY", "AY", "AY", "AY",     // 4-7
};

const char *SoundWindow::channelName(int ch)
{
    if (ch < 0 || ch >= NUM_CHANNELS) return "";
    return kChannelNames[ch];
}

const char *SoundWindow::channelSection(int ch)
{
    if (ch < 0 || ch >= NUM_CHANNELS) return "";
    return kChannelSections[ch];
}

// ---------------------------------------------------------------------------
// Channel color table (indexed by channel — UI order)
// ---------------------------------------------------------------------------

static const ImVec4 kChannelColors[SoundWindow::NUM_CHANNELS] = {
    ImVec4(0.3f, 0.9f, 0.9f, 1.0f),  // Standard Noise — cyan
    ImVec4(0.9f, 0.5f, 0.2f, 1.0f),  // i8253 Ch 1 — orange
    ImVec4(0.7f, 0.3f, 0.9f, 1.0f),  // i8253 Ch 2 — purple
    ImVec4(0.5f, 0.8f, 0.5f, 1.0f),  // i8253 Ch 3 — green
    ImVec4(1.0f, 1.0f, 0.3f, 1.0f),  // AY Noise  — yellow
    ImVec4(1.0f, 0.3f, 0.3f, 1.0f),  // AY A      — red
    ImVec4(0.3f, 1.0f, 0.3f, 1.0f),  // AY B      — green
    ImVec4(0.4f, 0.6f, 1.0f, 1.0f),  // AY C      — blue
};

static constexpr float kBarWidth     = 2.0f;
static constexpr float kChartHeight  = 36.0f;
static constexpr float kGridAlpha    = 0.15f;

// Timer clock of the i8253 in this machine (~1.5 MHz, pixel clock / 8)
static constexpr float kTimerClockHz = 1500000.0f;

// Minimum completed counter loads per second for a NON-square-wave i8253
// counter (mode 0/1) to be considered an audible PWM melody: each reload
// produces one output edge, so the reload rate IS the tone activity. Below
// this rate the writes are timing/one-shot traffic, not sound (30 Hz is the
// low end of a musical fundamental). Measured against wall time, so the
// judgement is independent of the GUI frame rate.
static constexpr double kPwmMinLoadsPerSec = 30.0;

// AY amplitude lookup table (matches AY-3-8912 hardware)
static const float kAmpTable[16] = {
    0.0f, 0.0137f, 0.0205f, 0.0291f,
    0.0423f, 0.0618f, 0.0847f, 0.1369f,
    0.1691f, 0.2647f, 0.3527f, 0.4499f,
    0.5704f, 0.6873f, 0.8482f, 1.0f
};

// ---------------------------------------------------------------------------
// Frequency → bar height.
//
// This is the SAME logarithmic mapping the Sound window has always used for
// the i8253 channels (digital on/off sources without amplitude control):
//   30 Hz → 0.3 ... 15 kHz → 1.0 (clamped in between).
//
// For the Standard Noise Channel the value fed in is the measured PC0
// TRANSITION RATE (toggles/sec between snapshots) — i.e. switching
// frequency/ACTIVITY, explicitly not a volume level.
// ---------------------------------------------------------------------------

static float activityLevelFromHz(float hz)
{
    if (hz <= 0.0f) return 0.0f;
    float norm = logf(hz / 30.0f) / logf(500.0f);
    return 0.3f + 0.7f * std::min(1.0f, std::max(0.0f, norm));
}

// ---------------------------------------------------------------------------
// Snapshot → per-channel activity and bar levels (pure function)
// ---------------------------------------------------------------------------

SoundWindow::Analysis SoundWindow::analyzeSnapshot(const SoundSnapshot &snap)
{
    Analysis a{};

    int ampA    = snap.registers[8]  & 0x0F;
    int ampB    = snap.registers[9]  & 0x0F;
    int ampC    = snap.registers[10] & 0x0F;
    int noisePeriod = snap.registers[6] & 0x1F;

    // -----------------------------------------------------------------------
    // 0. Standard Noise Channel — PIA1 Port C bit 0 (tape-out beeper).
    // Activity = PC0 transitions measured since the previous snapshot.
    // Bar height = transition rate through the shared logarithmic frequency
    // mapping (like i8253). This source NEVER reads AY registers.
    // -----------------------------------------------------------------------
    a.active[CH_STD_NOISE] = snap.standardNoise.togglesSinceLast > 0;
    a.level[CH_STD_NOISE] = a.active[CH_STD_NOISE] ?
        activityLevelFromHz(static_cast<float>(snap.standardNoise.toggleRateHz))
        : 0.0f;

    // -----------------------------------------------------------------------
    // 1-3. i8253 channels — two ways a counter produces sound:
    //  * mode 3 square wave: activity from the counter's SUSTAINED tone-output
    //    state (`sounding`), not the one-shot write flag (`dirty`). A VI53
    //    counter keeps generating its square wave from the moment a divider is
    //    loaded (mode 3) until it is reprogrammed, so the bar stays lit for
    //    the whole note — from start to cancel — exactly like the AY tone
    //    channels below.
    //  * mode 0/1 PWM melody: the counter never "sustains" — the ROM reloads
    //    it continuously at audio rate (Arkanoid-style tunes do this), and
    //    each completed load is one output edge. The channel is active while
    //    the measured reload rate is audible, and is flagged `retrigger` so
    //    the render can highlight this special output mode.
    // Bar height still comes from the output frequency (unchanged model).
    // -----------------------------------------------------------------------
    for (int ch = 0; ch < 3; ++ch) {
        const TimerChannelState &t = snap.timerChannels[ch];
        bool sustain = t.sounding;
        bool retrigger = !sustain && (t.mode == 0 || t.mode == 1) &&
                         t.loadValue != 0 &&
                         t.loadsSinceLast > 0 &&
                         t.loadRateHz >= kPwmMinLoadsPerSec;
        bool active = sustain || retrigger;
        int idx = CH_TIMER_BASE + ch;
        a.active[idx] = active;
        a.retrigger[idx] = retrigger;
        if (active) {
            float freq = t.loadValue > 0 ? kTimerClockHz / t.loadValue : 0.0f;
            a.level[idx] = activityLevelFromHz(freq);
        } else {
            a.level[idx] = 0.0f;
        }
    }

    // -----------------------------------------------------------------------
    // 4. AY Noise Channel — AY noise generator only (R6 period, R7 mixer,
    // R8-10 amplitudes). This source NEVER reads standard PC0 data.
    // -----------------------------------------------------------------------
    bool anyNoiseEnabled = (snap.noiseAEnabled && ampA > 0) ||
                           (snap.noiseBEnabled && ampB > 0) ||
                           (snap.noiseCEnabled && ampC > 0);
    a.active[CH_AY_NOISE] = anyNoiseEnabled && noisePeriod > 0;
    if (a.active[CH_AY_NOISE]) {
        int maxNoiseAmp = 0;
        if (snap.noiseAEnabled) maxNoiseAmp = std::max(maxNoiseAmp, ampA);
        if (snap.noiseBEnabled) maxNoiseAmp = std::max(maxNoiseAmp, ampB);
        if (snap.noiseCEnabled) maxNoiseAmp = std::max(maxNoiseAmp, ampC);
        a.level[CH_AY_NOISE] = kAmpTable[maxNoiseAmp];
    } else {
        a.level[CH_AY_NOISE] = 0.0f;
    }

    // -----------------------------------------------------------------------
    // 5-7. AY tone channels — tone output amplitude (existing model).
    // -----------------------------------------------------------------------
    const bool toneActive[3] = {
        ampA > 0 && snap.toneAEnabled,
        ampB > 0 && snap.toneBEnabled,
        ampC > 0 && snap.toneCEnabled,
    };
    const int toneAmp[3] = { ampA, ampB, ampC };
    for (int ch = 0; ch < 3; ++ch) {
        a.active[CH_AY_BASE + ch] = toneActive[ch];
        a.level[CH_AY_BASE + ch] = toneActive[ch] ? kAmpTable[toneAmp[ch]] : 0.0f;
    }

    return a;
}

// ---------------------------------------------------------------------------
// Draw a scrolling sound-log bar chart for one channel
// ---------------------------------------------------------------------------

static void drawSoundLog(const char *name, int channelIdx,
                         const float *ringBuffer, int writePos, int count,
                         bool active, float chartHeight,
                         bool retrigger = false)
{
    ImGui::PushID(channelIdx);

    // Label (dimmed when inactive; amber when the tone is a PWM re-trigger
    // melody — the special non-square-wave output mode)
    ImVec4 textColor = retrigger ? ImVec4(1.0f, 0.78f, 0.2f, 1.0f)
                     : active    ? ImVec4(1, 1, 1, 1)
                                 : ImVec4(0.5f, 0.5f, 0.5f, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    ImGui::Text("%s", name);
    ImGui::PopStyleColor();

    // Allocate space for the bar chart
    ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, chartHeight);
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImDrawList *drawList = ImGui::GetWindowDrawList();

    // Background
    ImU32 bgColor = IM_COL32(20, 20, 20, 255);
    drawList->AddRectFilled(cursorPos,
                            ImVec2(cursorPos.x + canvasSize.x, cursorPos.y + canvasSize.y),
                            bgColor);

    // Horizontal grid lines at 0.25, 0.5, 0.75
    ImU32 gridColor = ImGui::GetColorU32(ImVec4(1, 1, 1, kGridAlpha));
    for (int g = 1; g <= 3; ++g) {
        float gy = cursorPos.y + canvasSize.y - (g * 0.25f) * canvasSize.y;
        drawList->AddLine(ImVec2(cursorPos.x, gy),
                          ImVec2(cursorPos.x + canvasSize.x, gy),
                          gridColor);
    }

    // Bars
    if (count > 0) {
        float availWidth = canvasSize.x;
        int maxBars = static_cast<int>(availWidth / kBarWidth);
        int numVisible = std::min(count, maxBars);
        ImVec4 col = kChannelColors[channelIdx];
        ImU32 barColor = ImGui::GetColorU32(col);

        for (int i = 0; i < numVisible; ++i) {
            // i=0 is the oldest visible sample, i=numVisible-1 is the newest
            // (1024 == SoundWindow::SOUND_LOG_CAPACITY, kept as literal here
            //  because drawSoundLog is a free function)
            int bufIdx = (writePos - numVisible + i + 1024) % 1024;
            float level = ringBuffer[bufIdx];

            if (level > 0.001f) {
                float barHeight = level * canvasSize.y;
                float x0 = cursorPos.x + (availWidth - numVisible * kBarWidth) + i * kBarWidth;
                float y0 = cursorPos.y + canvasSize.y - barHeight;
                float x1 = x0 + kBarWidth - 0.5f;  // small gap between bars
                float y1 = cursorPos.y + canvasSize.y;
                drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), barColor);
            }
        }
    }

    // Border — PWM re-trigger channels get a thick amber frame so the
    // special melody mode is obvious even in a screenshot of the log.
    ImU32 borderColor = ImGui::GetColorU32(ImVec4(0.4f, 0.4f, 0.4f, 0.6f));
    float borderThickness = 1.0f;
    if (retrigger) {
        borderColor = ImGui::GetColorU32(ImVec4(1.0f, 0.78f, 0.2f, 1.0f));
        borderThickness = 2.0f;
    }
    drawList->AddRect(cursorPos,
                      ImVec2(cursorPos.x + canvasSize.x, cursorPos.y + canvasSize.y),
                      borderColor, 0.0f, 0, borderThickness);

    // Invisible dummy to reserve space in the ImGui layout
    ImGui::Dummy(canvasSize);

    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Section header
// ---------------------------------------------------------------------------

static void sectionHeader(const char *title)
{
    ImGui::Text("%s", title);
    ImGui::Separator();
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// SAVE MID: turn the grabbed VI53 port writes into a Standard MIDI File.
// Parsing/writing lives in src/audio_midi.cpp (pure, unit-tested); here we
// only fetch the buffer and land the bytes.
// ---------------------------------------------------------------------------

static void saveGrabbedMid(IDebugBackend &backend, std::string &status,
                           bool glideSweeps, const std::string &path)
{
    const auto events = backend.audioGrabEvents();
    audiomidi::Config cfg;   // defaults = Vector-06C: 1.4976 MHz, 50 fps,
                             // one VBlank frame = one quarter note
    cfg.glideSweeps = glideSweeps;   // experimental: slides instead of staircases
    const auto segs = audiomidi::vi53Segments(events, cfg);
    const auto smf  = audiomidi::writeSmf(segs, cfg, "Vector-06C VI53 grab");

    std::ofstream f(path, std::ios::binary);
    if (f && !smf.empty()) {
        f.write(reinterpret_cast<const char *>(smf.data()),
                static_cast<std::streamsize>(smf.size()));
        int glides = 0;
        for (const auto &s : segs) if (!s.bends.empty()) glides++;
        status = "Saved " + std::to_string(segs.size()) + " notes";
        if (glides > 0) status += " (" + std::to_string(glides) + " glides)";
    } else {
        status = "Save failed: " + path;
    }
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void SoundWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Sound", &visible_)) {
        ImGui::End();
        return;
    }

    // Controls
    if (ImGui::Checkbox("Mute", &muted_)) {
        backend.setMuted(muted_);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Visualize", &visualize_);

    // Audio grab -> MIDI export. GRAB AUDIO records VI53 port writes on the
    // emulation thread; SAVE MID turns the captured buffer into a .mid.
    ImGui::SameLine();
    const bool grabbing = backend.isAudioGrabEnabled();
    if (ImGui::Button(grabbing ? "STOP GRAB" : "GRAB AUDIO")) {
        backend.setAudioGrabEnabled(!grabbing);
        grabStatus_ = grabbing ? "" : "Recording...";
    }
    if (grabbing) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextUnformatted("REC");
        ImGui::PopStyleColor();
    } else if (backend.audioGrabEventCount() > 0) {
        ImGui::SameLine();
        if (ImGui::Button("SAVE MID")) {
            IDebugBackend *be = &backend;   // backend outlives the dialog
            const bool glide = glideSweeps_;
            midSaveDialog_.onFileSelected = [be, this, glide](const std::string &path) {
                saveGrabbedMid(*be, grabStatus_, glide, path);
            };
            midSaveDialog_.showSave("", "Save MIDI File", {".mid"}, "v06c_sound.mid");
        }
    }

    // Experimental export variant. Fast small steps usually are a ROM's sound
    // effect sweeping through the divider, but a legato melody run looks the
    // same on the bus — hence opt-in, so a tune is never rewritten into a bend
    // by surprise.
    ImGui::Checkbox("Glide sweeps (pitch bend)", &glideSweeps_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Merge runs of quick 1-2 semitone steps into one held note\n"
            "with MIDI pitch-bend events instead of separate notes.\n"
            "Helps effect sweeps; may smear a legato melody.");
    }

    // One-line grab status (event count, truncation warning, save result).
    if (grabbing || backend.audioGrabEventCount() > 0 || !grabStatus_.empty()) {
        ImGui::TextDisabled("Grab: %zu VI53 events%s  %s",
                            backend.audioGrabEventCount(),
                            backend.audioGrabOverflowed() ? " (TRUNCATED)" : "",
                            grabStatus_.c_str());
    }
    midSaveDialog_.render();

    ImGui::Separator();
    ImGui::Spacing();

    // Get sound snapshot (register values)
    SoundSnapshot snap = backend.soundSnapshot();

    if (!snap.available) {
        ImGui::TextDisabled("(sound not available)");
        ImGui::End();
        return;
    }

    // Per-channel activity and bar levels
    Analysis a = analyzeSnapshot(snap);

    int periodA = (snap.registers[0] & 0xFF) | ((snap.registers[1] & 0x0F) << 8);
    int periodB = (snap.registers[2] & 0xFF) | ((snap.registers[3] & 0x0F) << 8);
    int periodC = (snap.registers[4] & 0xFF) | ((snap.registers[5] & 0x0F) << 8);
    int ampA    = snap.registers[8]  & 0x0F;
    int ampB    = snap.registers[9]  & 0x0F;
    int ampC    = snap.registers[10] & 0x0F;
    int noisePeriod = snap.registers[6] & 0x1F;

    // -----------------------------------------------------------------------
    // Push levels into the ring buffer (frozen while paused)
    // -----------------------------------------------------------------------

    bool paused = backend.isPaused();

    if (visualize_ && !paused) {
        for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
            soundLog_[ch][soundLogWrite_] = a.level[ch];
        }
        soundLogWrite_ = (soundLogWrite_ + 1) % SOUND_LOG_CAPACITY;
        if (soundLogCount_ < SOUND_LOG_CAPACITY) ++soundLogCount_;
    }

    // -----------------------------------------------------------------------
    // Draw sound log bar charts — fixed order, sources visually separated
    // -----------------------------------------------------------------------

    // STANDARD VECTOR SOUND — Standard Noise Channel only
    sectionHeader(channelSection(CH_STD_NOISE));
    drawSoundLog(channelName(CH_STD_NOISE), CH_STD_NOISE,
                 soundLog_[CH_STD_NOISE], soundLogWrite_, soundLogCount_,
                 a.active[CH_STD_NOISE], kChartHeight);
    ImGui::Spacing();

    // i8253 — three tone channels (PWM re-trigger melodies are tagged and
    // highlighted — see analyzeSnapshot)
    sectionHeader(channelSection(CH_TIMER_BASE));
    for (int ch = 0; ch < 3; ++ch) {
        int idx = CH_TIMER_BASE + ch;
        char label[64];
        if (a.retrigger[idx]) {
            snprintf(label, sizeof(label), "%s  [PWM re-trigger]", channelName(idx));
        } else {
            snprintf(label, sizeof(label), "%s", channelName(idx));
        }
        drawSoundLog(label, idx,
                     soundLog_[idx], soundLogWrite_, soundLogCount_,
                     a.active[idx], kChartHeight, a.retrigger[idx]);
    }
    ImGui::Spacing();

    // AY — noise generator first, then tone channels A/B/C
    sectionHeader(channelSection(CH_AY_NOISE));
    drawSoundLog(channelName(CH_AY_NOISE), CH_AY_NOISE,
                 soundLog_[CH_AY_NOISE], soundLogWrite_, soundLogCount_,
                 a.active[CH_AY_NOISE], kChartHeight);
    for (int ch = 0; ch < 3; ++ch) {
        int idx = CH_AY_BASE + ch;
        drawSoundLog(channelName(idx), idx,
                     soundLog_[idx], soundLogWrite_, soundLogCount_,
                     a.active[idx], kChartHeight + 10);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // -----------------------------------------------------------------------
    // Register / measurement summary
    // -----------------------------------------------------------------------

    // Standard noise — raw diagnostic values, honestly labelled:
    // toggles/sec is a switching ACTIVITY measurement, not volume.
    if (a.active[CH_STD_NOISE]) {
        ImGui::Text("Standard Noise: %u PC0 transitions since last snapshot"
                    "  (~%.0f /sec)",
                    snap.standardNoise.togglesSinceLast,
                    snap.standardNoise.toggleRateHz);
    } else {
        ImGui::Text("Standard Noise: idle");
    }

    // i8253 channels (counter index 0..2 == Channel 1..3)
    for (int ch = 0; ch < 3; ++ch) {
        const TimerChannelState &t = snap.timerChannels[ch];
        int timerPeriod = t.loadValue;
        int idx = CH_TIMER_BASE + ch;
        if (a.retrigger[idx]) {
            // Special output mode: one-shot counter hammered at audio rate
            float freq = timerPeriod > 0 ? kTimerClockHz / timerPeriod : 0.0f;
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.2f, 1.0f),
                               "i8253 Channel %d: mode %d — PWM melody (%.0f re-loads/sec)"
                               "  load=%d  divider freq≈%.0f Hz",
                               ch + 1, t.mode, t.loadRateHz,
                               timerPeriod, freq);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Tone comes from the reload rate, not a self-running\n"
                    "square wave. GRAB AUDIO records these writes too, but\n"
                    "the offline MIDI parser decodes mode-3 notes only.");
            }
        } else if (a.active[idx] && timerPeriod > 0) {
            float freq = kTimerClockHz / timerPeriod;
            ImGui::Text("i8253 Channel %d: load=%d  mode=%d  freq=%.0f Hz  [square wave, sustained]",
                        ch + 1, timerPeriod, t.mode, freq);
        } else {
            ImGui::Text("i8253 Channel %d: idle", ch + 1);
        }
    }
    ImGui::Spacing();

    // AY register summary
    ImGui::Text("Mixer (R7): %02X  Tone: %c%c%c  Noise: %c%c%c  Noise period: %d",
                snap.registers[7],
                snap.toneAEnabled ? 'A' : '.',
                snap.toneBEnabled ? 'B' : '.',
                snap.toneCEnabled ? 'C' : '.',
                snap.noiseAEnabled ? 'A' : '.',
                snap.noiseBEnabled ? 'B' : '.',
                snap.noiseCEnabled ? 'C' : '.',
                noisePeriod);

    ImGui::Text("Amplitude: A=%X B=%X C=%X   Tone periods: A=%d B=%d C=%d",
                ampA, ampB, ampC, periodA, periodB, periodC);

    ImGui::End();
}
