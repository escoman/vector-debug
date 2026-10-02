#pragma once

#include <cstdint>
#include <vector>
#include <functional>

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Vector Screen Window — Enhanced with zoom, pan, VRAM debug
//
// Displays the live Vector-06C framebuffer with:
//   - Zoom 1x–16x (nearest-neighbor, no smoothing)
//   - Pan (middle mouse drag when zoomed)
//   - Screen coordinate display (X/Y in Vector pixels)
//   - VRAM address mapping
//   - VRAM Debug Mode (byte grid, write highlights, write count)
//   - Live / Capture mode
//   - Context menu (Go to VRAM, Memory Inspector, Disassembly)
// ---------------------------------------------------------------------------

class VectorScreenWindow
{
public:
    VectorScreenWindow() = default;
    ~VectorScreenWindow();

    void render(IDebugBackend &backend);

    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    bool &getVisibleRef() { return visible_; }
    void requestRefresh() { needsRefresh_ = true; }

    // Hover info for main status bar
    bool isHoveringScreen() const { return hoverScreenX_ >= 0 && hoverScreenY_ >= 0; }
    int hoverScreenX() const { return hoverScreenX_; }
    int hoverScreenY() const { return hoverScreenY_; }
    bool isHoveringBorder() const { return hoverBorder_; }

    // Navigation callbacks (set by GUI main)
    std::function<void(uint16_t address)> onGoToMemoryInspector;
    std::function<void(uint16_t address)> onGoToDisassembly;

private:
    bool visible_ = true;
    bool needsRefresh_ = true;

    // -- Zoom ----------------------------------------------------------------
    int zoomLevel_ = 0;  // 0=1x, 1=2x, 2=4x, 3=8x, 4=16x
    static constexpr int ZOOM_LEVELS = 5;
    static constexpr int ZOOM_FACTORS[ZOOM_LEVELS] = {1, 2, 4, 8, 16};
    bool fitToWindow_ = false;

    int currentZoom() const { return ZOOM_FACTORS[zoomLevel_]; }
    void zoomIn();
    void zoomOut();
    void zoomReset();

    // -- Pan (in visible-area screen coordinates) ----------------------------
    float panX_ = 0, panY_ = 0;
    bool isPanning_ = false;
    float panStartX_ = 0, panStartY_ = 0;
    float panStartMouseX_ = 0, panStartMouseY_ = 0;

    void clampPan(int visibleW, int visibleH, int zoomFactor,
                  float regionW, float regionH);

    // -- Live / Capture ------------------------------------------------------
    bool liveMode_ = true;
    std::vector<uint32_t> capturedPixels_;
    int capturedWidth_ = 0, capturedHeight_ = 0;

    // -- VRAM Debug Mode -----------------------------------------------------
    bool vramDebugMode_ = false;
    bool showByteGrid_ = true;
    bool showAddress_ = true;
    bool showChangedBytes_ = true;
    bool showWriteCount_ = false;
    bool showLastWrite_ = true;

    // -- Texture -------------------------------------------------------------
    unsigned int textureId_ = 0;
    int texWidth_  = 0;
    int texHeight_ = 0;
    std::vector<uint32_t> cachedPixels_;

    void updateTexture(const uint32_t *pixels, int width, int height);
    void destroyTexture();

    // -- Hover state ---------------------------------------------------------
    int hoverBufX_ = -1, hoverBufY_ = -1;        // framebuffer coordinates
    int hoverScreenX_ = -1, hoverScreenY_ = -1;   // visible-area coordinates
    bool hoverBorder_ = false;                     // mouse over border area

    // -- Write highlight overlay ---------------------------------------------
    std::vector<uint64_t> prevWriteCounts_;   // previous per-address write counts
    std::vector<uint8_t>  changedFlags_;      // 256 bytes: 1 = recently changed
    int highlightFadeFrames_ = 120;            // ~2 seconds at 60fps
    int highlightTimer_ = 0;                   // countdown

    // -- Sub-render methods --------------------------------------------------
    void renderToolbar(IDebugBackend &backend);
    void renderScreen(IDebugBackend &backend);
    void renderCoordinateInfo(IDebugBackend &backend);
    void renderVramDebugOverlay(IDebugBackend &backend);
    void renderContextMenu(IDebugBackend &backend);

    // -- Helpers -------------------------------------------------------------
    void handleZoomInput();
    void handlePanInput(int visibleW, int visibleH, int zoomFactor,
                        float regionW, float regionH);
    void updateWriteHighlights(IDebugBackend &backend);
    void captureCurrentFrame(IDebugBackend &backend);
};
