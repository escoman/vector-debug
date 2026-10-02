#pragma once

#include <cstdint>
#include <functional>
#include "idebug_backend.h"

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Stack View Window (Stage 3.4)
//
// Displays memory around current SP with byte and little-endian word columns.
// Uses existing readMemorySnapshot() API — no direct Memory access.
// ---------------------------------------------------------------------------

class StackViewWindow
{
public:
    StackViewWindow() {
        snapshot_.start = 0;
    }
    
    // Render the window. Call every frame.
    void render(IDebugBackend &backend);
    
    // Request snapshot refresh on next render
    void requestRefresh() { needsRefresh_ = true; }
    
    // Check if window is visible
    bool isVisible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    bool &getVisibleRef() { return visible_; }
    
    // Navigate to specific address (Stage 3.9 — real scrolling, inline for testability)
    void gotoAddress(uint16_t address) {
        viewCenter_ = address;
        followSP_ = false;
        pendingScroll_ = true;
        needsRefresh_ = true;
        visible_ = true;
    }
    
    // Current view center address (for tests / navigation)
    uint16_t address() const { return followSP_ ? currentSP_ : viewCenter_; }
    
    // Follow SP toggle (Stage 3.9)
    bool followSP() const { return followSP_; }
    void setFollowSP(bool v) { followSP_ = v; needsRefresh_ = true; }
    
    // Callbacks for cross-navigation (Stage 3.9)
    std::function<void(uint16_t address)> onGoToMemoryInspector;
    std::function<void(uint16_t address)> onGoToDisassembly;
    
private:
    bool visible_ = true;
    
    // Snapshot cache
    MemorySnapshot snapshot_;
    bool needsRefresh_ = true;
    bool pendingScroll_ = false;        // Stage 3.9: real scroll on next render
    
    // Current SP (cached for follow logic)
    uint16_t currentSP_ = 0;
    
    // Stage 3.9: Follow SP toggle + manual navigation
    bool followSP_ = true;
    uint16_t viewCenter_ = 0;           // center of view when followSP_ is off
    
    // Render sub-components
    void renderToolbar(IDebugBackend &backend);
    void renderStackView(IDebugBackend &backend);
    
    // Snapshot management
    void refreshSnapshot(IDebugBackend &backend);
    
    // Helper: compute safe range around a center address
    static void computeRange(uint16_t center, int &start, int &end);
};
