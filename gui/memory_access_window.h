#pragma once

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "agent_types.h"       // RuntimeAccessLogEntry

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Memory Access Window — Stage 6.25
//
// Detailed inspector for individual memory accesses within a chosen address
// range.  Sits on top of the existing bounded runtime access log accumulated
// by DebugBackend (Stage 6.20 / 6.22 / 6.24).  Renders one row per observed
// access: sequence, PC (of the accessing instruction), disassembly text,
// access type (READ/WRITE), effective memory address, and byte value.
//
// Design constraints (from Stage 6.25 pipeline doc):
//   - Fetch accesses (opcode / immediate operand bytes) are NEVER shown.
//     They are already classified via `fetchRemaining_` in onMemoryRead().
//   - Filtering happens on the *memory address*, not on PC.
//   - Live mode polls backend once per ~100 ms, not per access.
//   - Disassembly is done lazily on GUI thread via backend.readMemory()
//     + disassembler.h::disassemble().  No decoder duplication.
//   - Clear button clears ONLY the detailed log (new backend API
//     `clearMemoryAccessLog()`), never the aggregated runtime map.
// ---------------------------------------------------------------------------

// Filter configuration; pure data, unit-testable.
struct MemoryAccessFilter {
    uint16_t from = 0x8000;
    uint16_t to   = 0x80FF;

    enum Mode { Read, Write, All };
    Mode mode = All;

    // Retained for future extension.  Stack accesses (PUSH/POP/CALL/RET)
    // are currently indistinguishable from other writes in the log; the
    // flag exists so tests can pin the default-true behaviour.
    bool includeStack = true;

    // Returns true when [from, to] is a valid inclusive range.
    bool isValid() const { return from <= to; }
};

// Pure filtering function: no ImGui, no backend, no thread coupling.
// Returns a NEW vector containing the entries that pass the filter,
// preserving chronological order (input is already chronological from
// RingBuffer::snapshot()).  Truncates to the most-recent `maxRows`
// matching entries (drop-oldest).
std::vector<RuntimeAccessLogEntry> applyMemoryAccessFilter(
    const std::vector<RuntimeAccessLogEntry> &log,
    const MemoryAccessFilter &filter,
    size_t maxRows);

// ---------------------------------------------------------------------------
// MemoryAccessWindow — GUI panel
// ---------------------------------------------------------------------------

class MemoryAccessWindow
{
public:
    MemoryAccessWindow();
    ~MemoryAccessWindow() = default;

    // Called every frame from DebuggerGui::render().  Handles its own
    // throttled refresh when Live mode is on.
    void render(IDebugBackend &backend);

    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    bool &getVisibleRef() { return visible_; }

    // Navigation callbacks — wired by DebuggerGui (see gui.cpp).
    std::function<void(uint16_t address)> onGoToDisassembly;
    std::function<void(uint16_t address)> onGoToMemoryInspector;

    // Optional: ask MemoryMapWindow for the currently selected block's
    // base address.  Returns true and sets `outAddress` when a valid
    // selection exists; false otherwise (button is disabled).
    std::function<bool(uint16_t &outAddress)> getSelectedMapBlock;

    // -- Testable state -----------------------------------------------------

    const MemoryAccessFilter &filter() const { return filter_; }
    MemoryAccessFilter       &mutableFilter() { return filter_; }
    bool isLive() const { return live_; }
    void setLive(bool v) { live_ = v; }

    // Manually trigger one refresh (equivalent to pressing Refresh).
    void refresh(IDebugBackend &backend);

    // Explicitly clear ONLY the detailed log (backend side + local rows_).
    void clearLog(IDebugBackend &backend);

    // Stage 6.25: invoked from Memory Map's context menu ("To Memory
    // Access").  Sets the range to the block [base, base+0xFF], makes
    // the window visible, and queues a Refresh + focus bring-forward on the
    // next render() call (which is where we still have the backend
    // reference).
    void focusOnBlock(uint16_t base);

    // Test helpers for the pending flags set by focusOnBlock().
    bool hasPendingRefresh() const { return pendingRefresh_; }
    bool hasPendingFocus()   const { return pendingFocus_; }

    // Access current rendered rows (for tests).
    const std::vector<RuntimeAccessLogEntry> &rows() const { return rows_; }

    // Total entries the backend had at last refresh (before filtering).
    size_t totalLogSize() const { return totalLogSize_; }

    // Disassembly string cache (populated lazily during render()).
    const std::string *lookupDisassembly(uint16_t pc) const;

    // How many rows we render at most.
    static constexpr size_t MAX_ROWS = 5000;

private:
    bool visible_ = false;
    bool live_    = false;

    MemoryAccessFilter filter_;

    // Copy of the latest filtered snapshot shown to the user.
    std::vector<RuntimeAccessLogEntry> rows_;
    size_t totalLogSize_ = 0;

    // Hex-string buffers for From/To fields (4 uppercase hex digits + NUL).
    char fromBuf_[8] = "8000";
    char toBuf_[8]   = "80FF";

    // Live-mode throttle.
    using Clock = std::chrono::steady_clock;
    Clock::time_point lastRefresh_{};
    static constexpr auto REFRESH_INTERVAL = std::chrono::milliseconds(100);

    // Per-PC disassembly cache.  Kept across refreshes so re-decoding the
    // same hot-loop PC is a cache hit.  Reset on ROM change (see render():
    // we track `lastRomId_` via a weak heuristic — first render after
    // loadRom triggers a full clear by checking total log reset).
    std::unordered_map<uint16_t, std::string> disasmCache_;

    // Invalidate disasm cache when the backend's log wraps around to empty
    // (which happens on loadRom() → clearRuntimeAccessMap()).
    bool wasEmptyLastFrame_ = true;

    // Row context menu / selection state.
    int  hoveredRow_ = -1;
    bool ctxMenuOpen_ = false;
    int  ctxMenuRow_ = -1;

    // Stage 6.25: set by focusOnBlock(); consumed at the top of the next
    // render().  Refresh must happen inside render() because it needs the
    // backend reference; focus must happen BEFORE Begin().
    bool pendingRefresh_ = false;
    bool pendingFocus_   = false;

    // Sync hex input buffers with filter_ when user edits.
    void parseHexInputs();
    // Format filter_.from/to back into the hex buffers.
    void formatHexInputs();

    // Refresh rows_ from backend snapshot (does not touch timing state).
    void doRefresh(IDebugBackend &backend);

    // Populate disasmCache_ for unique PCs currently in rows_.
    void primeDisassembly(IDebugBackend &backend);
};
