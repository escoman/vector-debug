#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include "idebug_backend.h"

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Memory Inspector Window (Stage 3.3)
//
// Displays 64 KB virtual address space with banking support.
// Uses MemorySnapshot for efficient data access.
// ---------------------------------------------------------------------------

class MemoryInspectorWindow
{
public:
    MemoryInspectorWindow() {
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
    
    // Navigate to specific address (Stage 3.9 — real scrolling)
    void gotoAddress(uint16_t address) {
        address_ = address;
        selectedAddress_ = address;
        snprintf(addressInput_, sizeof(addressInput_), "%04X", address);
        needsRefresh_ = true;
        pendingScroll_ = true;
        pendingFocus_ = true;
        visible_ = true;
    }
    
    // Current view address (for tests / navigation)
    uint16_t address() const { return address_; }
    
    // Callback: Go to Disassembly (Stage 3.9)
    std::function<void(uint16_t address)> onGoToDisassembly;
    
private:
    bool visible_ = true;
    
    // Address state
    uint16_t address_ = 0;          // Current view address (for scroll position)
    uint16_t selectedAddress_ = 0;  // Selected byte address
    
    // Snapshot cache
    MemorySnapshot snapshot_;
    bool needsRefresh_ = true;
    bool pendingScroll_ = false;      // Stage 3.9: real scroll on next render
    bool pendingFocus_ = false;       // bring dock tab to front
    
    // Input buffer for address field
    char addressInput_[8] = "0000";
    
    // Byte editing state (Stage 3.5)
    bool editingByte_ = false;
    char editBuffer_[4] = "";        // hex input for byte edit
    uint16_t editAddress_ = 0;       // address being edited
    bool writeFailed_ = false;       // status: write rejected (not paused)
    
    // Render sub-components
    void renderToolbar(IDebugBackend &backend);
    void renderMemoryView(IDebugBackend &backend);
    void renderDisassembly(IDebugBackend &backend);
    
    // Snapshot management
    void refreshSnapshot(IDebugBackend &backend);
    
    // Address parsing
    bool parseAddress(const char *input, uint16_t &address) const;
    
    // Byte editing
    void beginEditByte(uint16_t address);
    void confirmEdit(IDebugBackend &backend);
    void cancelEdit();
};
