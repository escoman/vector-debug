#pragma once

#include <cstdint>
#include <chrono>
#include <functional>

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Memory Map Window — Stage 5.2
//
// Visualizes the full 64K address space as 256 blocks (each 256 bytes).
// Layout: 32 columns × 8 rows, grouped into 8 groups of 4 columns (8 KB each).
// Each block is a 2×2 px cell with 1px grid + 2px group separators.
// Color is driven by live read/write activity timestamps.
// ---------------------------------------------------------------------------

// Block color determined by live activity (testable without ImGui)
enum class LiveBlockColor
{
    DarkGray,   // no recent activity, no data
    Green,      // read within ACTIVITY_DURATION
    Red,        // write within ACTIVITY_DURATION
    Yellow,     // both read and write within ACTIVITY_DURATION
    Cyan        // block contains non-zero data (no live activity)
};

// Compute block color from timestamps (free function for unit testing)
LiveBlockColor computeBlockColor(
    std::chrono::steady_clock::time_point lastRead,
    std::chrono::steady_clock::time_point lastWrite,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds duration);

// Convert block index (0..255) to base address
// Layout: 32 columns × 8 rows.
// Column = group of 8 KB (4 columns of 256-byte blocks per row).
// Block N → col = N / 8, row = N % 8
// Address = col * 0x800 + row * 0x100
inline uint16_t blockToAddress(int block) {
    int col = block / 8;
    int row = block % 8;
    return static_cast<uint16_t>((col * 0x800 + row * 0x100) & 0xFFFF);
}

class MemoryMapWindow
{
public:
    MemoryMapWindow();
    ~MemoryMapWindow() = default;

    void render(IDebugBackend &backend);

    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    bool &getVisibleRef() { return visible_; }

    // Navigation callbacks
    std::function<void(uint16_t address)> onGoToMemoryInspector;
    std::function<void(uint16_t address)> onGoToDisassembly;

    // Stage 6.25: seed the Memory Access window's range from the clicked
    // block, refresh and focus it.  Argument = block base address.
    std::function<void(uint16_t address)> onGoToMemoryAccess;

    // -- Test helpers (expose internal state for unit tests) -----------------

    bool isLive() const { return live_; }
    void setLive(bool v);
    void clearActivity();

    // Data presence scanning
    bool blockHasData(int index) const { return hasData_[index]; }
    void scanMemoryForData(IDebugBackend &backend);

    static constexpr int NUM_BLOCKS      = 256;
    static constexpr int BLOCK_SIZE      = 256;   // bytes per block
    static constexpr int CELL_PX         = 2;     // pixels per block side
    static constexpr int GRID_PX         = 1;     // grid line thickness
    static constexpr int SEPARATOR_PX    = 2;     // group separator thickness
    static constexpr int BLOCKS_PER_GROUP = 4;    // columns per 8KB group
    static constexpr int NUM_GROUPS      = 8;     // 8 groups of 8KB
    static constexpr int COLUMNS         = 32;    // total columns (NUM_GROUPS * BLOCKS_PER_GROUP)
    static constexpr int ROWS            = 8;     // 8 rows of 256-byte blocks

    static constexpr auto ACTIVITY_DURATION = std::chrono::milliseconds(500);
    static constexpr auto SCAN_INTERVAL     = std::chrono::milliseconds(1000);

    struct BlockState
    {
        std::chrono::steady_clock::time_point lastReadTime;
        std::chrono::steady_clock::time_point lastWriteTime;
    };

    const BlockState &blockState(int index) const { return blocks_[index]; }

    // Stage 6.25: expose the currently hovered / context-selected block so
    // other windows (Memory Access) can seed their range from it.  Returns
    // true and writes the block's base address when a hover or context
    // selection is active.
    bool getSelectedBlockAddress(uint16_t &outAddress) const {
        if (hoverBlock_ >= 0) {
            outAddress = blockToAddress(hoverBlock_);
            return true;
        }
        if (contextBlock_ >= 0) {
            outAddress = blockToAddress(contextBlock_);
            return true;
        }
        return false;
    }

private:
    bool visible_ = true;
    bool live_    = false;

    BlockState blocks_[NUM_BLOCKS];

    // Data presence tracking (scanned periodically)
    bool hasData_[NUM_BLOCKS];
    std::chrono::steady_clock::time_point lastScanTime_;
    bool scanPending_ = true;  // force initial scan

    // Hover state
    uint16_t hoverAddress_ = 0;
    int      hoverBlock_   = -1;

    // Context menu state
    uint16_t contextAddress_ = 0;
    int      contextBlock_   = -1;

    // Map block index from canvas pixel coordinates.
    // Returns -1 if outside any block.
    int hitTest(float localX, float localY) const;

    // Canvas dimensions in pixels (with group separators)
    static constexpr int canvasWidth()  {
        // cells + internal grid + group separators + border grid
        return COLUMNS * CELL_PX
             + (COLUMNS + 1) * GRID_PX
             + (NUM_GROUPS - 1) * (SEPARATOR_PX - GRID_PX);
    }
    static constexpr int canvasHeight() {
        return ROWS * CELL_PX + (ROWS + 1) * GRID_PX;
    }
};
