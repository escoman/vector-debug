#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Xrefs Window — Stage 4.7
//
// Shows all cross-references to a selected address.
// Columns: From Address | Instruction | From Function
// ---------------------------------------------------------------------------

class XrefsWindow
{
public:
    XrefsWindow() = default;

    void render(IDebugBackend &backend);

    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    bool &getVisibleRef() { return visible_; }
    void requestRefresh() { needsRefresh_ = true; }

    // Set the target address to show xrefs for
    void setTargetAddress(uint16_t addr);

    // Navigation callback
    std::function<void(uint16_t address)> onGoToDisassembly;

private:
    bool visible_ = true;
    uint16_t targetAddress_ = 0;

    // Cached xref data
    struct XrefEntry {
        uint16_t from;
        std::string instruction;
        std::string fromFunction;
    };
    std::vector<XrefEntry> cachedXrefs_;
    bool needsRefresh_ = true;
};
