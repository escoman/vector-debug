#pragma once

#include <cstdint>
#include <functional>

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Functions Window — Stage 4.5 (readonly since Stage 6.11 RDB)
//
// Table of symbols from MAP file (readonly view).
// Columns: Address | Name | Size | Calls | Comment
// Title: "MAP-file Info"
// ---------------------------------------------------------------------------

class FunctionsWindow
{
public:
    FunctionsWindow() = default;

    void render(IDebugBackend &backend);

    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    bool &getVisibleRef() { return visible_; }
    void requestRefresh() { needsRefresh_ = true; }

    // Navigation callbacks
    std::function<void(uint16_t address)> onGoToDisassembly;
    std::function<void(uint16_t address)> onGoToMemoryInspector;

private:
    bool visible_ = true;
    bool needsRefresh_ = true;

    // Search/filter
    char searchBuffer_[64] = "";

    // Sorting
    int sortColumn_ = 0;  // 0=address, 1=name
    bool sortReverse_ = false;

    // Context menu state
    uint16_t contextAddress_ = 0;
};
