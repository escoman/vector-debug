#pragma once

#include <cstdint>
#include <functional>
#include "idebug_backend.h"

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Breakpoints Window (Stage 3.7)
//
// Displays all breakpoints with enable/disable toggle, add/remove controls.
// Uses IDebugBackend API — no direct access to Board or CPU.
// ---------------------------------------------------------------------------

class BreakpointsWindow
{
public:
    BreakpointsWindow() {}
    
    // Render the window. Call every frame.
    void render(IDebugBackend &backend);
    
    // Check if window is visible
    bool isVisible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    bool &getVisibleRef() { return visible_; }
    
    // Callbacks for cross-navigation (Stage 3.9)
    std::function<void(uint16_t address)> onGoToDisassembly;
    std::function<void(uint16_t address)> onGoToMemoryInspector;
    
private:
    bool visible_ = true;
    
    // Input buffer for address field
    char addressInput_[8] = "";
    
    // Selected breakpoint index in the list (-1 = none)
    int selectedBpIndex_ = -1;
};
