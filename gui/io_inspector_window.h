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
// I/O Inspector Window (Stage 3.11)
//
// Displays recently recorded I/O accesses (IN/OUT) from the instrumentation
// ring buffer.  Uses existing ioHistorySnapshot() + instructionHistorySnapshot()
// APIs — no direct Memory/Board/CPU access.
//
// PC is resolved by cross-referencing IoAccessEvent.instructionSequence with
// InstructionEvent.sequence from the instruction history snapshot.
// ---------------------------------------------------------------------------

class IoInspectorWindow
{
public:
    // I/O type filter
    enum class TypeFilter { All, In, Out };

    IoInspectorWindow() {}
    ~IoInspectorWindow() = default;

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

    // --- Test accessors (Stage 3.11) ---

    // Number of entries currently cached for display (after filtering)
    size_t cachedSize() const { return cachedEntries_.size(); }

    // Access a cached entry by index (0 = oldest in cache)
    const IoAccessEvent &cachedEntry(size_t index) const {
        return cachedEntries_[index];
    }

    // Follow / Pause state (for tests)
    bool followIo() const { return followIo_; }
    void setFollowIo(bool v) { followIo_ = v; }
    bool pauseCapture() const { return pauseCapture_; }
    void setPauseCapture(bool v) { pauseCapture_ = v; }

    // Max display entries
    int maxEntries() const { return maxEntries_; }
    void setMaxEntries(int v) { maxEntries_ = v; }

    // Filter state (for tests)
    TypeFilter typeFilter() const { return typeFilter_; }
    void setTypeFilter(TypeFilter f) { typeFilter_ = f; }
    int portFilter() const { return portFilter_; }
    void setPortFilter(int v) { portFilter_ = v; }

private:
    bool visible_ = true;
    bool followIo_ = true;
    bool pauseCapture_ = false;
    bool needsRefresh_ = true;
    int  maxEntries_ = 100;           // display limit (user-configurable)

    // Filter state
    TypeFilter typeFilter_ = TypeFilter::All;
    int portFilter_ = -1;             // -1 = all ports, 0..255 = specific port
    int portInput_ = -1;              // pending input value (before Apply)

    // Cached snapshots (refreshed when needsRefresh_ and !pauseCapture_)
    std::vector<IoAccessEvent> cachedEntries_;
    std::vector<InstructionEvent> cachedInstrEvents_;  // for PC resolution

    // Auto-scroll tracking
    int lastDisplayedCount_ = -1;  // scroll only when count changes

    // Render sub-components
    void renderToolbar(IDebugBackend &backend);
    void renderIoTable(IDebugBackend &backend);
    void renderHardwareState(IDebugBackend &backend);
    void renderPalette(IDebugBackend &backend);

    // Resolve PC for an I/O event by cross-referencing instructionSequence
    uint16_t resolvePc(uint64_t instructionSequence) const;
};
