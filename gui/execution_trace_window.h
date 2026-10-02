#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>
#include "idebug_backend.h"
#include "events.h"

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Execution Trace Window (Stage 3.10)
//
// Displays recently executed CPU instructions from the instrumentation
// ring buffer.  Uses existing instructionHistorySnapshot() API — no
// direct Memory/Board/CPU access.
// ---------------------------------------------------------------------------

class ExecutionTraceWindow
{
public:
    ExecutionTraceWindow() {}

    // Render the window. Call every frame.
    void render(IDebugBackend &backend);

    // Request refresh on next render
    void requestRefresh() { needsRefresh_ = true; }

    // Check if window is visible
    bool isVisible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    bool &getVisibleRef() { return visible_; }

    // Navigation callbacks (Stage 3.9 pattern)
    std::function<void(uint16_t address)> onGoToDisassembly;
    std::function<void(uint16_t address)> onGoToMemoryInspector;

    // --- Test accessors (Stage 3.10) ---

    // Number of entries currently cached for display
    size_t cachedSize() const { return cachedEntries_.size(); }

    // Access a cached entry by index (0 = oldest in cache)
    const InstructionEvent &cachedEntry(size_t index) const {
        return cachedEntries_[index];
    }

    // Follow / Pause state (for tests)
    bool followExecution() const { return followExecution_; }
    void setFollowExecution(bool v) { followExecution_ = v; }
    bool pauseCapture() const { return pauseCapture_; }
    void setPauseCapture(bool v) { pauseCapture_ = v; }

    // Max display entries
    int maxEntries() const { return maxEntries_; }
    void setMaxEntries(int v) { maxEntries_ = v; }

private:
    bool visible_ = true;
    bool followExecution_ = true;
    bool pauseCapture_ = false;
    bool needsRefresh_ = true;
    int  maxEntries_ = 1000;          // display limit (user-configurable)
    char searchBuffer_[64] = "";

    // Cached snapshot (refreshed when needsRefresh_ and !pauseCapture_)
    std::vector<InstructionEvent> cachedEntries_;
    uint64_t lastSnapshotSeq_ = 0;    // detect new data from backend

    // Render sub-components
    void renderToolbar(IDebugBackend &backend);
    void renderTraceTable(IDebugBackend &backend);
};
