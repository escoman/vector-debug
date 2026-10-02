#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <functional>

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Search Window — Stage 4.8
//
// Global search across addresses, function names, labels, and comments.
// ---------------------------------------------------------------------------

class SearchWindow
{
public:
    SearchWindow() = default;

    void render(IDebugBackend &backend);

    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    bool &getVisibleRef() { return visible_; }

    // Navigation callbacks
    std::function<void(uint16_t address)> onGoToDisassembly;
    std::function<void(uint16_t address)> onGoToMemoryInspector;

private:
    bool visible_ = true;
    char searchBuffer_[128] = "";

    struct SearchResult {
        uint16_t address;
        std::string name;
        std::string detail;  // "Function", "Label", "Comment: ..."
    };
    std::vector<SearchResult> results_;
    bool needsSearch_ = false;
};
