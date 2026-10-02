#include "debug_adapter.h"
#include "backend.h"       // for DebuggerBreakpoint
#include "debug_memory.h"
#include "rom_load_address.h"
#include "i8080.h"
#include "i8080_hal.h"
#include "options.h"
#include "util.h"
#include "globaldefs.h"

#include <cstdio>

using namespace i8080cpu;

// ---------------------------------------------------------------------------
// Static member definitions
// ---------------------------------------------------------------------------

Memory       *DebugAdapter::s_memory = nullptr;
IO           *DebugAdapter::s_io     = nullptr;
Board        *DebugAdapter::s_board  = nullptr;

// ---------------------------------------------------------------------------
// HAL functions — connect the CPU core to emulator components.
//
// These replace src/hal.cpp for the debugger target.
// I/O functions are instrumented via DebugAdapter static accessors.
// The application layer (main.cpp) sets g_backend for I/O tracking.
// ---------------------------------------------------------------------------

// Forward declaration — defined in application layer (main.cpp)
// The application provides a global backend pointer for I/O instrumentation.
class DebugBackend;
extern DebugBackend *g_adapter_backend;

void i8080_hal_bind(Memory &_mem, IO &_io, Board &_board)
{
    DebugAdapter::setHalPointers(&_mem, &_io, &_board);
}

int i8080_hal_memory_read_byte(int addr)
{
    return DebugAdapter::halMemory()->read(addr, false);
}

void i8080_hal_memory_write_byte(int addr, int value)
{
    DebugAdapter::halMemory()->write(addr, value, false);
}

int i8080_hal_memory_read_word(int addr, bool stack)
{
    Memory *mem = DebugAdapter::halMemory();
    return mem->read(addr, stack)
         | (mem->read(addr + 1, stack) << 8);
}

void i8080_hal_memory_write_word(int addr, int word, bool stack)
{
    Memory *mem = DebugAdapter::halMemory();
    mem->write(addr, word & 0xff, stack);
    mem->write(addr + 1, word >> 8, stack);
}

int i8080_hal_io_input(int port)
{
    int value = DebugAdapter::halIo()->input(port);
    if (g_adapter_backend) g_adapter_backend->onIoInput((uint8_t)port, (uint8_t)value);
    return value;
}

void i8080_hal_io_output(int port, int value)
{
    DebugAdapter::halIo()->output(port, value);
    if (g_adapter_backend) g_adapter_backend->onIoOutput((uint8_t)port, (uint8_t)value);
}

void i8080_hal_iff(int on)
{
    DebugAdapter::halBoard()->interrupt(on != 0);
}

// Timer is not needed for the debugger — execution is driven by
// DebugBackend::runUntilPause(), not by SDL events.
void create_timer() {}

uint32_t timer_callback(uint32_t interval, void * param)
{
    Board *board = DebugAdapter::halBoard();
    if (board) board->onframetimer();
    return interval;
}

// ---------------------------------------------------------------------------
// HAL binding
// ---------------------------------------------------------------------------

void DebugAdapter::bindHal()
{
    s_memory = &memory;
    s_io     = &io;
    s_board  = &board;
}

void DebugAdapter::setHalPointers(Memory *mem, IO *io, Board *board)
{
    s_memory = mem;
    s_io     = io;
    s_board  = board;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DebugAdapter::DebugAdapter()
    : tape_player(wav)
    , tw(timer)
    , aw(ay)
    , soundnik(tw, aw)
    , io(memory, keyboard, timer, fdc, ay, tape_player)
    , filler(memory, io, tv)
    , board(memory, io, filler, soundnik, tv, tape_player)
    , rasterEvents_(50000)
{
}

DebugAdapter::~DebugAdapter()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void DebugAdapter::init()
{
    if (initialized_) return;

    filler.init();
    soundnik.init(nullptr);
    tv.init();
    board.init();
    fdc.init();

    // In the main emulator, onframetimer triggers frame execution via the
    // event queue. In the debugger, the emulation thread runs independently,
    // so we set it to a no-op to avoid std::bad_function_call from the
    // SDL audio callback.
    board.onframetimer = []() { /* no-op in debugger */ };

    // The audio device starts out silent; the gate opens when the emulation
    // loop begins producing frames (DebugBackend::executeFramesTarget_ ->
    // setAudioEmulationActive(true)).
    updateAudioPause();

    keyboard.onreset = [this](bool blkvvod) {
        board.reset(blkvvod ?
                Board::ResetMode::BLKVVOD : Board::ResetMode::BLKSBR);
    };

    // Track РУС/LAT LED state — the ROM toggles this via port C bit 3
    io.onruslat = [this](bool rus) {
        ruslatState_ = rus;
    };

    // Track i8253 timer writes and standard-noise (PIA PC0) transitions
    // for Sound window visualization.
    // The io.onwrite hook fires BEFORE the actual write, giving us raw
    // port/value pairs.  We interpret the i8253/PIA protocol here.
    io.onwrite = [this](uint32_t port, uint8_t value) -> void {
        // Stage 6.27 P2: record every OUT with its exact beam position.
        // Runs on the emulation thread, so filler.* and i8080_pc() are live
        // right here — this is the accurate mid-frame correlation.
        {
            RasterEvent ev;
            ev.frame         = static_cast<uint64_t>(board.get_frame_no());
            ev.rasterLine    = static_cast<uint32_t>(filler.rasterLine());
            ev.vCycleInLine  = static_cast<uint32_t>(filler.rasterPixel());
            ev.vCycle        = ev.rasterLine * VideoTiming::lineVCycles + ev.vCycleInLine;
            ev.pc            = static_cast<uint16_t>(i8080_pc());
            ev.port          = static_cast<uint8_t>(port);
            ev.value         = value;
            rasterEvents_.push(ev);
        }

        // Track AY writes (ports 0x14 = data, 0x15 = latch).
        // AY activity must never touch the standard noise counter below.
        if (port == 0x14 || port == 0x15) {
            ayDirty_ = true;
            return;
        }

        // Standard Vector noise channel: PIA1 Port C bit 0 (tape-out beeper).
        // Count ONLY actual PC0 state transitions (old != new) — a write of
        // the same level is not a toggle. BSR writes on port 0x00 are decoded
        // semantically: bit number (3-1) and set/reset (bit 0).
        int newPC0 = -1;  // -1 = this write does not affect PC0
        if (port == 0x01) {
            newPC0 = value & 1;             // direct write: PC := value
        } else if (port == 0x00) {
            if ((value & 0x80) == 0) {
                int bit = (value >> 1) & 7; // BSR: selected bit number
                if (bit == 0) newPC0 = value & 1; // SET/RESET PC0
            } else {
                // CW write: the core re-issues PC := 0 (vio.h case 0x00)
                newPC0 = 0;
            }
        }
        if (newPC0 != -1) {
            if (newPC0 != pc0Mirror_) {
                pc0TogglesTotal_.fetch_add(1, std::memory_order_relaxed);
            }
            pc0Mirror_ = newPC0;
        }

        if (port == 0x08) {
            // Timer control word (core: vio.h maps ~port&3==3 → write_cw)
            int ctr = (value >> 6) & 3;
            if (ctr < 3) {
                int latch = (value >> 4) & 3;
                int mode  = (value >> 1) & 7;
                timerLatchModes_[ctr] = latch;
                timerModes_[ctr] = mode;
                timerWriteStates_[ctr] = 0;
            }
        } else if (port >= 0x09 && port <= 0x0B) {
            // Counter data ports: ~0x0B&3=0, ~0x0A&3=1, ~0x09&3=2
            int ctr = (~port) & 3;
            int latch = timerLatchModes_[ctr];
            if (latch == 3) {
                // LSB then MSB
                if (timerWriteStates_[ctr] == 0) {
                    timerWriteLsb_[ctr] = value;
                    timerWriteStates_[ctr] = 1;
                } else {
                    timerLoadValues_[ctr] = (static_cast<uint16_t>(value) << 8) | timerWriteLsb_[ctr];
                    timerWriteStates_[ctr] = 0;
                    timerDirty_[ctr] = true;  // complete write — mark dirty
                }
            } else if (latch == 1) {
                // LSB only
                timerLoadValues_[ctr] = value;
                timerDirty_[ctr] = true;
            } else if (latch == 2) {
                // MSB only
                timerLoadValues_[ctr] = static_cast<uint16_t>(value) << 8;
                timerDirty_[ctr] = true;
            }
        }
    };

    board.reset(Board::ResetMode::BLKVVOD);

    // Seed the PC0 mirror from the committed reset-time state so the first
    // observed transition is measured against a real level, not "unknown".
    pc0Mirror_ = io.TapeOut();

    initialized_ = true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void DebugAdapter::shutdown()
{
    if (!initialized_) return;

    s_memory = nullptr;
    s_io     = nullptr;
    s_board  = nullptr;

    initialized_ = false;
}

// ---------------------------------------------------------------------------
// IDebugTarget: Memory access
// ---------------------------------------------------------------------------

uint8_t DebugAdapter::readMemory(uint16_t addr)
{
    return memory.read(addr, false);
}

uint8_t DebugAdapter::peekMemory(uint16_t addr)
{
    return DebugMemoryAccess::peek(memory, addr);
}

uint8_t DebugAdapter::readMemoryRaw(uint16_t addr)
{
    return memory.peek(addr, false);
}

void DebugAdapter::writeMemory(uint16_t addr, uint8_t val)
{
    memory.write(addr, val, false);
}

// ---------------------------------------------------------------------------
// IDebugTarget: Memory instrumentation callbacks
// ---------------------------------------------------------------------------

void DebugAdapter::setMemoryCallbacks(MemoryReadCallback onRead,
                                      MemoryWriteCallback onWrite)
{
    memReadCb_  = onRead;
    memWriteCb_ = onWrite;

    if (onRead) {
        // Save previous callbacks for chaining
        prevMemOnRead_  = memory.onread;
        prevMemOnWrite_ = memory.onwrite;

        memory.onread = [this](uint32_t virt, uint32_t phys,
                               bool stack, uint8_t value) {
            // Stage 6.24: pass the CPU's PC at callback time.
            // For RD_BYTE(PC++) the increment is a sequence point before the
            // function call, so i8080_pc() == virt + 1 for instruction fetches.
            uint16_t pc = static_cast<uint16_t>(i8080_pc());
            if (memReadCb_) memReadCb_(virt, phys, stack, value, pc);
            if (prevMemOnRead_) prevMemOnRead_(virt, phys, stack, value);
        };

        memory.onwrite = [this](uint32_t virt, uint32_t phys,
                                bool stack, uint8_t value) {
            if (memWriteCb_) memWriteCb_(virt, phys, stack, value);
            if (prevMemOnWrite_) prevMemOnWrite_(virt, phys, stack, value);
        };
    } else {
        // Clear: restore previous callbacks
        memory.onread  = prevMemOnRead_;
        memory.onwrite = prevMemOnWrite_;
        prevMemOnRead_  = nullptr;
        prevMemOnWrite_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// IDebugTarget: CPU state
// ---------------------------------------------------------------------------

CpuState DebugAdapter::getCpuState()
{
    CpuState s;
    s.pc    = static_cast<uint16_t>(i8080_pc());
    s.sp    = static_cast<uint16_t>(i8080_regs_sp());
    s.a     = static_cast<uint8_t>(i8080_regs_a());
    s.b     = static_cast<uint8_t>(i8080_regs_b());
    s.c     = static_cast<uint8_t>(i8080_regs_c());
    s.d     = static_cast<uint8_t>(i8080_regs_d());
    s.e     = static_cast<uint8_t>(i8080_regs_e());
    s.h     = static_cast<uint8_t>(i8080_regs_h());
    s.l     = static_cast<uint8_t>(i8080_regs_l());
    s.flags = static_cast<uint8_t>(i8080_regs_f());
    s.iff   = i8080_iff();
    s.cycles     = static_cast<uint32_t>(i8080_cycles());
    s.ei_pending = false;
    s.last_pc    = 0;
    return s;
}

void DebugAdapter::writeCpuRegister(int reg, uint16_t val)
{
    // Register encoding matches DebugBackend::RegisterId enum:
    // 0=AF, 1=BC, 2=DE, 3=HL, 4=SP, 5=PC
    switch (reg) {
        case 0: i8080_setreg_a((val >> 8) & 0xFF); i8080_setreg_f(val & 0xFF); break;
        case 1: i8080_setreg_b((val >> 8) & 0xFF); i8080_setreg_c(val & 0xFF); break;
        case 2: i8080_setreg_d((val >> 8) & 0xFF); i8080_setreg_e(val & 0xFF); break;
        case 3: i8080_setreg_h((val >> 8) & 0xFF); i8080_setreg_l(val & 0xFF); break;
        case 4: i8080_setreg_sp(val); break;
        case 5: i8080_jump(val); break;
    }
}

// ---------------------------------------------------------------------------
// IDebugTarget: Execution control
// ---------------------------------------------------------------------------

void DebugAdapter::stepInstruction()
{
    board.single_step(true);  // true = update screen (for Vector Screen snapshot)
}

void DebugAdapter::executeFrame()
{
    board.execute_frame_with_cadence(true, false);
}

void DebugAdapter::reset(bool attachBoot)
{
    // NOTE: board.reset() does not touch PIA registers, so the PC0 mirror
    // stays valid across a reset — no invalidation needed here.
    if (attachBoot) {
        // Reset (полный сброс): attach boot ROM, PC=0, execute bootloader.
        board.reset(Board::ResetMode::BLKVVOD);
        board.interrupt(false);  // сброс INTE + IRQ
    } else {
        // Restart (горячий сброс): detach boot ROM, PC=0, execute from RAM.
        board.reset(Board::ResetMode::BLKSBR);
    }
}

// ---------------------------------------------------------------------------
// IDebugTarget: Debugger control
// ---------------------------------------------------------------------------

void DebugAdapter::debuggerBreak()      { board.debugger_break(); }
void DebugAdapter::debuggerContinue()   { board.debugger_continue(); }
void DebugAdapter::debuggerAttached()   { board.debugger_attached(); }
void DebugAdapter::debuggerDetached()   { board.debugger_detached(); }

void DebugAdapter::setPollCallback(std::function<void()> cb)
{
    board.poll_debugger = cb;
}

// ---------------------------------------------------------------------------
// IDebugTarget: Breakpoints
// ---------------------------------------------------------------------------

void DebugAdapter::syncBreakpoints(const DebuggerBreakpoint *bps, size_t count)
{
    // Build set of new enabled addresses
    std::set<uint16_t> newAddresses;
    for (size_t i = 0; i < count; ++i) {
        if (bps[i].enabled) {
            newAddresses.insert(bps[i].address);
        }
    }
    
    // Remove breakpoints that are in synced but not in new list
    for (uint16_t addr : syncedBreakpoints_) {
        if (newAddresses.find(addr) == newAddresses.end()) {
            // Breakpoint was removed or disabled
            board.remove_breakpoint(0, addr, 1);
        }
    }
    
    // Add breakpoints that are in new list but not in synced
    for (uint16_t addr : newAddresses) {
        if (syncedBreakpoints_.find(addr) == syncedBreakpoints_.end()) {
            // New breakpoint
            board.insert_breakpoint(0, addr, 1);
        }
    }
    
    // Update tracked set
    syncedBreakpoints_ = newAddresses;
}

// ---------------------------------------------------------------------------
// IDebugTarget: Screen
// ---------------------------------------------------------------------------

ScreenData DebugAdapter::screenSnapshot()
{
    ScreenData data;
    uint32_t *pixels = tv.pixels();
    if (!pixels) return data;

    data.width  = DEFAULT_SCREEN_WIDTH;
    data.height = DEFAULT_SCREEN_HEIGHT;
    size_t total = static_cast<size_t>(data.width) * data.height;
    data.pixels.assign(pixels, pixels + total);
    return data;
}

// ---------------------------------------------------------------------------
// Stage 6.27: Beam / raster state (racing-the-beam debugging)
//
// Pure read-only snapshot. Reads ONLY state the video path has already
// computed (PixelFiller debugger accessors, Board frame counter, IO palette).
// Never pauses, steps, re-renders or mutates anything.
// ---------------------------------------------------------------------------

BeamState DebugAdapter::getBeamState()
{
    BeamState s;
    s.available = true;

    // Raw beam position straight from the emulator's own video model.
    const int  rline   = filler.rasterLine();     // 0 .. 311 (wraps at 312)
    const int  rpixelP = filler.rasterPixel();     // 0 .. 767 pixel-times
    const bool vborder = filler.vBorder();

    s.frame         = static_cast<uint64_t>(board.get_frame_no());
    s.rasterLine    = static_cast<uint32_t>(rline);
    s.vCycleInLine  = static_cast<uint32_t>(rpixelP);
    s.rpixel        = static_cast<uint32_t>(rpixelP - 24); // internal coord
    s.vCycleInFrame = static_cast<uint32_t>(rline) * VideoTiming::lineVCycles
                    + static_cast<uint32_t>(rpixelP);

    s.frameVCycles = VideoTiming::frameVCycles;
    s.lineVCycles  = VideoTiming::lineVCycles;
    s.frameLines   = VideoTiming::frameLines;

    // Visible-area coordinates: same geometry the filler uses for the TV
    // framebuffer (bmp_x = raster_pixel - center_offset). Border is NOT
    // visible (ТЗ §8): vertical border or horizontally outside the picture.
    const int bmpX = rpixelP - filler.centerOffset();
    const int bmpY = rline   - filler.firstVisibleLine();
    s.visible = filler.isVisible() && !vborder &&
                bmpX >= 0 && bmpX < filler.scrWidth() && bmpY >= 0;
    if (s.visible) {
        s.visibleX = bmpX;
        s.visibleY = bmpY;
    } else {
        s.visibleX = -1;
        s.visibleY = -1;
    }

    // CPU currently executing "next to" the beam.
    CpuState cpu = getCpuState();
    s.cpuPc      = cpu.pc;
    s.cpuOpcode  = peekMemory(cpu.pc);

    // Palette entry the video path is shifting out at the current beam
    // position — i.e. the entry an OUT 0Ch committing now would rewrite.
    // In border/blanking there is no picture palette index under the beam;
    // the border index is reported separately (ТЗ §12: don't conflate).
    if (!vborder) {
        s.hasPaletteIndex = true;
        s.paletteIndex    = filler.currentColorIndex();
        s.paletteValue    = io.RawPaletteByte(s.paletteIndex);
    } else {
        s.hasPaletteIndex = false;
        s.paletteIndex    = -1;
        s.paletteValue    = 0;
    }
    s.borderIndex = io.BorderIndex();

    return s; // 'running' is filled in by DebugBackend (it owns the state machine)
}

std::vector<RasterEvent> DebugAdapter::getRasterEvents(
    uint64_t frame, uint32_t vCycleStart, uint32_t vCycleEnd,
    int port, uint16_t pc, size_t maxResults)
{
    std::vector<RasterEvent> all = rasterEvents_.snapshot();  // logical (oldest->newest)
    std::vector<RasterEvent> out;
    out.reserve(all.size());
    for (const auto &ev : all) {
        if (frame != 0 && ev.frame != frame) continue;
        if (ev.vCycle < vCycleStart || ev.vCycle > vCycleEnd) continue;
        if (port >= 0 && ev.port != static_cast<uint8_t>(port)) continue;
        if (pc != 0xFFFF && ev.pc != pc) continue;
        out.push_back(ev);
    }
    // Cap to the most recent maxResults entries (tail).
    if (maxResults != static_cast<size_t>(-1) && out.size() > maxResults) {
        out.erase(out.begin(), out.begin() + (out.size() - maxResults));
    }
    return out;
}

void DebugAdapter::clearRasterEvents()
{
    rasterEvents_.clear();
}

PaletteSnapshot DebugAdapter::paletteSnapshot() const
{
    PaletteSnapshot snap;
    snap.count = 16;
    for (int i = 0; i < 16; ++i) {
        uint8_t raw = io.RawPaletteByte(i);
        // Vector-06C palette byte: B:7-6 G:5-3 R:2-0
        int r3 = raw & 0x07;         // 3 bits, 0-7
        int g3 = (raw >> 3) & 0x07;  // 3 bits, 0-7
        int b2 = (raw >> 6) & 0x03;  // 2 bits, 0-3
        snap.entries[i].r = static_cast<uint8_t>(r3 * 255 / 7);
        snap.entries[i].g = static_cast<uint8_t>(g3 * 255 / 7);
        snap.entries[i].b = static_cast<uint8_t>(b2 * 255 / 3);
        snap.entries[i].rawByte = raw;
    }
    return snap;
}

SoundSnapshot DebugAdapter::soundSnapshot() const
{
    SoundSnapshot snap;
    snap.available = true;

    // Read AY registers directly — bypass IO port layer to avoid side effects
    AY &ayRef = const_cast<AY&>(ay);
    for (int i = 0; i < 16; ++i) {
        ayRef.write(1, i);                  // select register
        snap.registers[i] = static_cast<uint8_t>(ayRef.read(0));
    }

    // Parse mixer register (7):
    //   bits 0-2: tone enable (inverted: 0=enabled, 1=disabled)
    //   bits 3-5: noise enable (inverted)
    uint8_t mixer = snap.registers[7];
    snap.toneAEnabled = !(mixer & 0x01);
    snap.toneBEnabled = !(mixer & 0x02);
    snap.toneCEnabled = !(mixer & 0x04);
    snap.noiseAEnabled = !(mixer & 0x08);
    snap.noiseBEnabled = !(mixer & 0x10);
    snap.noiseCEnabled = !(mixer & 0x20);

    // Populate AY dirty flag and clear it
    snap.ayDirty = ayDirty_;
    const_cast<DebugAdapter*>(this)->ayDirty_ = false;

    // Populate i8253 timer channel state (tracked via io.onwrite callback)
    for (int i = 0; i < 3; ++i) {
        snap.timerChannels[i].loadValue = timerLoadValues_[i];
        snap.timerChannels[i].mode      = timerModes_[i];
        snap.timerChannels[i].dirty     = timerDirty_[i];
        const_cast<DebugAdapter*>(this)->timerDirty_[i] = false;
    }

    // Standard Vector noise (PIA PC0): transitions since the PREVIOUS
    // snapshot plus the measured rate over the elapsed wall interval.
    // The atomic counter is written by the emulation thread; the delta and
    // the interval bookkeeping below are only touched from this (snapshot)
    // thread, so there is no read/write race.
    const auto now = std::chrono::steady_clock::now();
    uint64_t total = pc0TogglesTotal_.load(std::memory_order_relaxed);
    snap.standardNoise.togglesSinceLast =
        static_cast<uint32_t>(total - pc0TogglesLastSnapshot_);
    if (pc0SnapshotPrimed_ && now > pc0SnapshotTime_) {
        double dt = std::chrono::duration<double>(now - pc0SnapshotTime_).count();
        if (dt > 0.0) {
            snap.standardNoise.toggleRateHz =
                static_cast<double>(snap.standardNoise.togglesSinceLast) / dt;
        }
    }
    snap.standardNoise.dirty = snap.standardNoise.togglesSinceLast > 0;
    snap.standardNoise.lastLevel = pc0Mirror_;
    const_cast<DebugAdapter*>(this)->pc0TogglesLastSnapshot_ = total;
    const_cast<DebugAdapter*>(this)->pc0SnapshotTime_ = now;
    const_cast<DebugAdapter*>(this)->pc0SnapshotPrimed_ = true;

    return snap;
}

void DebugAdapter::setMuted(bool muted)
{
    // Use soundnik.pause() to mute/unmute audio output
    // pause(1) = paused (muted), pause(0) = playing
    // Mute is only one half of the condition — the frame loop has to be
    // active as well, see setAudioEmulationActive().
    audioMuted_ = muted;
    updateAudioPause();
}

void DebugAdapter::setAudioEmulationActive(bool active)
{
    audioEmulationActive_ = active;
    updateAudioPause();
}

// Sound samples are produced exclusively by the frame loop
// (Board::execute_frame -> Soundnik::soundSteps -> sample()), but they are
// consumed by the SDL audio callback on its own thread, out of an 8-buffer
// ring (8 * 20 ms). Left alone, that ring keeps playing after the emulation
// is paused, and once it runs dry the callback replays a half-written buffer
// and pads with the last sample value (a DC level, not silence) — the
// crackling heard around Pause/Resume. Soundnik::pause() both stops the
// device (SDL waits for the running callback) and resets the read/write
// cursors, so gating it on the frame loop drains the backlog on Pause and
// starts Resume from an empty ring.
void DebugAdapter::updateAudioPause()
{
    soundnik.pause(isAudioOutputPaused() ? 1 : 0);
}

// ---------------------------------------------------------------------------
// IDebugTarget: Keyboard injection
// ---------------------------------------------------------------------------

void DebugAdapter::pressKey(int scancode)
{
    keyboard.apply_key(static_cast<SDL_Scancode>(scancode), false);
}

void DebugAdapter::releaseKey(int scancode)
{
    keyboard.apply_key(static_cast<SDL_Scancode>(scancode), true);
}

// ---------------------------------------------------------------------------
// IDebugTarget: ROM / init
// ---------------------------------------------------------------------------

bool DebugAdapter::loadRom(const std::string &path, uint32_t org)
{
    std::vector<uint8_t> rom_data = util::load_binfile(path);
    if (rom_data.empty()) {
        printf("DebugAdapter::loadRom(): failed to load %s\n", path.c_str());
        return false;
    }

    // Auto-detect load address from file extension if org not explicitly given
    if (org == 0 && path.find('.') != std::string::npos) {
        org = getRomLoadAddress(path);
    }

    printf("DebugAdapter::loadRom(): loaded %s (%zu bytes) at %04x\n",
           path.c_str(), rom_data.size(), org);

    memory.init_from_vector(rom_data, org);

    // Reset CPU: boot ROM detached, PC=0, SP=0xc300 (BLK+СБР semantics).
    // We must NOT set PC to the ROM load address — after BLK+СБР the CPU
    // always starts at PC=0000 and the program is reached via vector table.
    Options.pc = 0;
    board.reset(Board::ResetMode::LOADROM);
    // LOADROM sets SP=0xc300 and i8080_init() sets PC=0.

    // i8080_init() clears only the flags and PC: IFF and the EI hold-off of
    // the program that was running survive, and Board::reset() drops `irq`
    // but leaves the `inte` pin high. init_from_vector() has just zeroed all
    // 64 KiB, so the first vblank of the new ROM would fire RST 7 -> CALL
    // 0038 into a field of NOPs and drown it in pushed return addresses (SP
    // walks down by 2 per frame; the ROM never really starts). DI is the only
    // public way to clear all three at once: it zeroes IFF and EI_PENDING and
    // calls i8080_hal_iff(0) -> Board::interrupt(false) -> inte + irq.
    // The opcode is passed directly, so nothing is fetched from memory and no
    // video cycles are consumed; DI leaves PC=0001, restored right below.
    i8080_execute(0xF3);   // DI
    i8080_jump(0);         // a loaded ROM always starts at PC=0000

    // The bootloader is skipped when a ROM is loaded directly, but on a
    // real machine it is the one that re-initializes the sound hardware
    // after БЛК+ВВОД. Without this, state left by the previous ROM — an
    // i8253 counter still running in square-wave mode or a latched PIA
    // Port C tape-out level — keeps sounding as a stuck note under the
    // new ROM (Board::reset only resets the AY chip itself).
    // Replicate boots.bin hardware init at 0x0000-0x0010:
    //   OUT 04,9B / OUT 00,88 / OUT 08,A8 / OUT 08,68 / OUT 08,28
    io.commit();  // apply any OUT the previous ROM latched but never committed
    static const uint8_t bootInit[][2] = {
        {0x04, 0x9B},  // PPI2 control word: ports A/B/C → outputs
        {0x00, 0x88},  // PIA1 control word → PA=PB=PC=0 (tape-out silent)
        {0x08, 0xA8},  // i8253 ctr2 → out of square-wave mode, out low
        {0x08, 0x68},  // i8253 ctr1 → same
        {0x08, 0x28},  // i8253 ctr0 → same
    };
    for (auto &w : bootInit) {
        io.output(w[0], w[1]);  // fires instrumentation hooks (I/O log, sound)
        io.commit();            // applies immediately — no frames run here
    }

    return true;
}

bool DebugAdapter::loadWav(const std::string &path)
{
    std::vector<uint8_t> data = util::load_binfile(path);
    if (data.empty()) {
        printf("DebugAdapter::loadWav(): failed to load %s\n", path.c_str());
        return false;
    }

    if (!wav.set_bytes(data)) {
        printf("DebugAdapter::loadWav(): invalid WAV format: %s\n", path.c_str());
        return false;
    }

    printf("DebugAdapter::loadWav(): loaded %s (%zu bytes)\n",
           path.c_str(), data.size());

    return true;
}

void DebugAdapter::initCpu(uint16_t pc, uint16_t sp)
{
    i8080_jump(pc);
    i8080_setreg_sp(sp);
    i8080_init();
}

// ---------------------------------------------------------------------------
// I/O ports (Stage 6.1 Iteration 3)
// ---------------------------------------------------------------------------

uint8_t DebugAdapter::readIoPort(uint8_t port)
{
    return static_cast<uint8_t>(io.input(port));
}

void DebugAdapter::writeIoPort(uint8_t port, uint8_t value)
{
    io.output(port, value);
}
