#pragma once

#include <cstdint>
#include "idebug_backend.h"

// ---------------------------------------------------------------------------
// Sound Window
//
// Displays sound controls (Mute) and a scrolling "sound log" showing
// output level over time for 8 independent channels, in this exact order:
//
//   STANDARD VECTOR SOUND
//     0. Standard Noise Channel — PIA1 Port C bit 0 (tape-out beeper).
//        Bar shows switching ACTIVITY (toggle rate), not volume.
//     i8253
//     1-3. i8253 Channel 1..3 — bar shows output frequency
//     AY
//     4.   AY Noise Channel — AY-3-8912 noise generator (activity/amplitude
//          derived from AY registers R6/R7/R8-10 only)
//     5-7. AY Channel A/B/C — AY tone channels (amplitude)
//
// The Standard Noise and AY Noise channels are strictly separate sources:
// Standard Noise comes from PIA PC0 transitions measured in the DebugAdapter
// (io.onwrite), AY Noise comes from AY registers only. Neither influences
// the other.
//
// Bars scroll right-to-left while emulation runs; frozen when paused.
// ---------------------------------------------------------------------------

class SoundWindow
{
public:
    SoundWindow() {}

    // Channel indices (UI order — see class comment)
    static constexpr int NUM_CHANNELS  = 8;
    static constexpr int CH_STD_NOISE  = 0;  // Standard Noise Channel
    static constexpr int CH_TIMER_BASE = 1;  // i8253 Channel 1..3 (1..3)
    static constexpr int CH_AY_NOISE   = 4;  // AY Noise Channel
    static constexpr int CH_AY_BASE    = 5;  // AY Channel A/B/C (5..7)

    // Static channel metadata (names/sections) — exposed for tests.
    static const char *channelName(int ch);
    static const char *channelSection(int ch);

    // Pure snapshot → per-channel activity/level mapping. No ImGui/emulator
    // access; used by render() and directly unit-testable.
    struct Analysis
    {
        bool  active[NUM_CHANNELS] = {};
        float level[NUM_CHANNELS]  = {};  // 0..1 bar height
    };
    static Analysis analyzeSnapshot(const SoundSnapshot &snap);

    // Render the window. Call every frame.
    void render(IDebugBackend &backend);

    // Check if window is visible
    bool isVisible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    bool &getVisibleRef() { return visible_; }

private:
    bool visible_ = true;
    bool muted_ = false;
    bool visualize_ = false;  // off by default: no level tracking

    // Sound log ring buffer
    // Channel order: Std Noise, i8253 1-3, AY Noise, AY A, AY B, AY C
    static constexpr int SOUND_LOG_CAPACITY = 1024;

    float soundLog_[NUM_CHANNELS][SOUND_LOG_CAPACITY] = {};
    int   soundLogWrite_ = 0;    // next write position
    int   soundLogCount_ = 0;    // number of valid samples (<= capacity)
};
