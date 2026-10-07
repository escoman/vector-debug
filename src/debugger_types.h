#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Shared data types for Debugger Core
//
// These types flow across the IDebugTarget / IDebugBackend interfaces.
// Kept in a separate header to avoid circular includes between
// debug_target.h, idebug_backend.h, and backend.h.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// CPU state snapshot
// ---------------------------------------------------------------------------

struct CpuState
{
    uint16_t pc;
    uint16_t sp;

    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t d;
    uint8_t e;
    uint8_t h;
    uint8_t l;

    uint8_t flags;   // S Z AC P CY packed as in F register

    bool iff;        // interrupt flip-flop

    // Stage 3.6 — additional CPU state
    uint32_t cycles;       // cycles of last instruction
    bool     ei_pending;   // EI pending flag
    uint16_t last_pc;      // PC before last step
};

// ---------------------------------------------------------------------------
// Memory snapshot for Inspector (Stage 3.3)
// ---------------------------------------------------------------------------

struct MemorySnapshot
{
    uint16_t start;
    std::vector<uint8_t> data;
};

// ---------------------------------------------------------------------------
// Screen data (IDebugTarget → DebugBackend)
// ---------------------------------------------------------------------------

struct ScreenData
{
    std::vector<uint32_t> pixels;  // ARGB8888
    int width  = 0;
    int height = 0;
};

// ---------------------------------------------------------------------------
// Palette snapshot (16 Vector-06C colors)
// ---------------------------------------------------------------------------

struct PaletteSnapshot
{
    struct Color {
        uint8_t r, g, b;      // decoded 8-bit RGB
        uint8_t rawByte;       // original Vector-06C palette byte (B:7-6 G:5-3 R:2-0)
    };
    Color entries[16];
    int count = 0;  // number of valid entries (0 = not available)
};

// ---------------------------------------------------------------------------
// Sound snapshot (standard Vector noise + AY-3-8912 + i8253 timer state)
// ---------------------------------------------------------------------------

struct TimerChannelState
{
    uint16_t loadValue = 0;   // last loaded counter value
    int      mode      = 0;   // counter mode (0-5)
    bool     dirty     = false; // true if written since last snapshot
    // True while the counter is actively generating its square-wave tone:
    // from the moment a non-zero divider is loaded in mode 3 until the counter
    // is reprogrammed (a mode-set control word) or switched out of tone mode.
    // Unlike `dirty` — a one-shot write event cleared on every snapshot — this
    // is persistent state, so the Sound window can draw a sustained note from
    // start to cancel, exactly like the AY tone channels.
    bool     sounding  = false;
};

// Standard Vector noise channel — the 1-bit tape-out beeper
// (PIA1 Port C bit 0, mixed via "beeper" volume in the core).
// This is NOT the AY noise generator. Measured via the debugger-side
// io.onwrite hook: we only count actual PC0 state transitions
// (old != new), which is a diagnostic activity measurement, not volume.
struct StandardNoiseState
{
    uint32_t togglesSinceLast = 0; // PC0 transitions since previous snapshot
    double   toggleRateHz     = 0; // transitions/sec over the previous interval
    int      lastLevel        = -1; // last observed PC0 state (-1 = unknown)
    bool     dirty            = false; // true if togglesSinceLast > 0
};

struct SoundSnapshot
{
    // AY registers (0-15)
    uint8_t registers[16] = {};

    // Mixer state (register 7)
    bool toneAEnabled = false;
    bool toneBEnabled = false;
    bool toneCEnabled = false;
    bool noiseAEnabled = false;
    bool noiseBEnabled = false;
    bool noiseCEnabled = false;

    // AY was written since last snapshot
    bool ayDirty = false;

    // i8253 timer channels (3 counters)
    TimerChannelState timerChannels[3] = {};

    // Standard Vector noise channel (PIA1 Port C bit 0 / tape-out)
    StandardNoiseState standardNoise = {};

    bool available = false;
};

// ---------------------------------------------------------------------------
// Audio grab event (Sound window "GRAB AUDIO" -> "SAVE MID")
//
// One raw chip-level port write with the frame stamp taken at write time
// (board frame counter — the same grid the emulator sound path runs on).
// The frame number replaces the py reference's VBlank-breakpoint grid, so
// durations are frame-exact and no CPU pausing is involved.
// ---------------------------------------------------------------------------

struct AudioPortEvent
{
    uint64_t frame = 0;   // board.get_frame_no() at the moment of the OUT
    uint8_t  port  = 0;   // VI53 ports only: 0x08 CW, 0x0B/0x0A/0x09 counters
    uint8_t  value = 0;
};

// ---------------------------------------------------------------------------
// Beam / raster state (Stage 6.27: racing-the-beam debugging)
//
// Produced exclusively by DebugAdapter (the only Vector-specific access point)
// from the emulator's own PixelFiller/Board/IO state. MCP/Agent API never
// compute timing themselves.
// ---------------------------------------------------------------------------

struct BeamState
{
    uint64_t frame = 0;            // current frame number (Board::get_frame_no)
    uint32_t vCycleInFrame = 0;    // 0 .. frameVCycles-1
    uint32_t rasterLine = 0;       // 0 .. frameLines-1
    uint32_t vCycleInLine = 0;     // 0 .. lineVCycles-1
    uint32_t rpixel = 0;           // internal video-path horizontal coordinate

    bool visible = false;          // beam inside the visible (non-border) area
    int  visibleX = -1;            // -1 when not visible
    int  visibleY = -1;

    uint32_t frameVCycles = 0;     // timing params read from the video model
    uint32_t lineVCycles  = 0;
    uint32_t frameLines   = 0;

    uint16_t cpuPc = 0;            // instruction currently executing near the beam
    uint8_t  cpuOpcode = 0;

    bool     hasPaletteIndex = false; // false -> paletteIndex under beam unknown
    int      paletteIndex = -1;       // entry OUT 0Ch would write at this beam pos
    uint8_t  paletteValue = 0;        // raw palette byte of that entry
    int      borderIndex = -1;        // separate hardware border index (PB & 0x0f)

    bool running = false;          // emulator was running at snapshot time
    bool available = false;        // target exposes beam state at all
};

// ---------------------------------------------------------------------------
// Raster event (Stage 6.27 P2): an OUT instruction correlated with the exact
// beam position at the moment it executed.
//
// Recorded on the emulation thread inside the io.onwrite hook, where the
// PixelFiller raster position and CPU PC are both live. This is the accurate
// mid-frame path (a plain beam snapshot taken while running can lag).
// ---------------------------------------------------------------------------

struct RasterEvent
{
    uint64_t frame = 0;
    uint32_t vCycle       = 0;   // v_cycle within the frame
    uint32_t rasterLine   = 0;
    uint32_t vCycleInLine = 0;
    uint16_t pc   = 0;           // address of the OUT instruction
    uint8_t  port = 0;
    uint8_t  value = 0;
};

// ---------------------------------------------------------------------------
// Debugger state machine
// ---------------------------------------------------------------------------

enum class DebuggerState
{
    Running,
    Paused,
    Stopped
};

// ---------------------------------------------------------------------------
// Stop reason (Stage 3.7)
// ---------------------------------------------------------------------------

enum class StopReason
{
    None,
    Breakpoint,
    UserPause,
    Step,
    Reset,
    Skip            // stopped after Skip (run until next instruction)
};

// ---------------------------------------------------------------------------
// Breakpoint model (Stage 3.7)
// ---------------------------------------------------------------------------

struct DebuggerBreakpoint
{
    uint16_t address;
    bool     enabled;
};

// ---------------------------------------------------------------------------
// Command result (Stage 5.3.1)
//
// Every state-changing Backend command returns this.
// ---------------------------------------------------------------------------

struct CommandResult
{
    bool success = false;
    std::string error;

    enum Status { Completed, Failed, Timeout, Cancelled };
    Status status = Completed;
};

// ---------------------------------------------------------------------------
// Exit reason (Stage 5.3.1)
//
// How a trace execution ended.  Never present a heuristic as fact.
// ---------------------------------------------------------------------------

enum class ExitReason
{
    Ret,           // RET instruction executed
    CallerReturn,  // PC returned to caller address
    Timeout,       // max instruction count reached
    Breakpoint,    // hit a breakpoint
    Halt,          // HLT instruction
    Unknown
};
