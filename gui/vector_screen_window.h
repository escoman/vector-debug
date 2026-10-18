#pragma once

#include <cstdint>
#include <vector>
#include <functional>

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Vector Screen Window — live Vector-06C framebuffer view
//
//   - Zoom 1x–16x (nearest-neighbor, no smoothing)
//   - Pan (left mouse drag when zoomed)
//   - Screen coordinate display (X/Y in Vector pixels)
//   - VRAM address mapping (for context-menu navigation)
//   - Live / Capture mode
//   - Context menu (Go to VRAM, Memory Inspector, Disassembly)
//
// Byte-level VRAM inspection (grid, addresses, write highlighting) lives in
// the separate "VRAM Planes" window (see plane_screen_window.h).
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

    // -- Beam overlay (racing-the-beam) --------------------------------------
    // When enabled and the emulation is paused, draw a blinking marker at the
    // beam's current raster position and report its coordinates in the status
    // bar. The marker is sized in framebuffer pixels so it scales with zoom.
    bool showBeam_ = false;

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

    // -- Sub-render methods --------------------------------------------------
    void renderToolbar(IDebugBackend &backend);
    void renderScreen(IDebugBackend &backend);
    void renderCoordinateInfo(IDebugBackend &backend);
    void renderContextMenu(IDebugBackend &backend);

    // -- Helpers -------------------------------------------------------------
    void handleZoomInput();
    void handlePanInput(int visibleW, int visibleH, int zoomFactor,
                        float regionW, float regionH);
    void captureCurrentFrame(IDebugBackend &backend);
};
