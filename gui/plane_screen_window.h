#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// Plane Screen Window — individual VRAM bit-plane viewer
//
// Displays one of the 4 VRAM bit planes (0–3) as a black-and-white image.
// VRAM layout (CPU logical view, each plane at its own base):
//   Plane 0: 0xE000   Plane 1: 0xC000   Plane 2: 0xA000   Plane 3: 0x8000
//   Each plane: 8KB, addr = base + byteCol*256 + byteRow
//   byteCol = 0..31  (32 byte-columns × 8 px = 256 px wide)
//   byteRow = 0..255 (0 = bottom, 255 = top)
//   Each byte: 8 horizontal pixels, bit 7 = left, bit 0 = right
// ---------------------------------------------------------------------------

class PlaneScreenWindow
{
public:
    PlaneScreenWindow() = default;
    ~PlaneScreenWindow();

    void render(IDebugBackend &backend);

    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    bool &getVisibleRef() { return visible_; }

private:
    bool visible_ = true;

    // Selected plane (0–3)
    int selectedPlane_ = 0;

    // Live mode: color pixels by recent memory access
    bool liveMode_ = false;

    // Track last read/write times per VRAM byte address (for live mode)
    struct AccessTimes {
        std::chrono::steady_clock::time_point lastRead;
        std::chrono::steady_clock::time_point lastWrite;
    };
    std::unordered_map<uint16_t, AccessTimes> accessTimes_;

    // Activity duration for live mode coloring
    static constexpr auto ACTIVITY_DURATION = std::chrono::milliseconds(500);

    // Zoom
    int zoomLevel_ = 0;  // 0=1x, 1=2x, 2=4x, 3=8x
    static constexpr int ZOOM_LEVELS = 4;
    static constexpr int ZOOM_FACTORS[ZOOM_LEVELS] = {1, 2, 4, 8};

    int currentZoom() const { return ZOOM_FACTORS[zoomLevel_]; }
    void zoomIn();
    void zoomOut();

    // Pan
    float panX_ = 0, panY_ = 0;
    bool isPanning_ = false;
    float panStartX_ = 0, panStartY_ = 0;
    float panStartMouseX_ = 0, panStartMouseY_ = 0;

    // Texture
    unsigned int textureId_ = 0;
    static constexpr int BASE_SIZE = 256;  // logical pixels per plane side
    int texW_ = BASE_SIZE;                 // actual texture width  (= BASE_SIZE * stride)
    int texH_ = BASE_SIZE;                 // actual texture height (= BASE_SIZE * stride)
    std::vector<uint32_t> planePixels_;    // ARGB8888, texW_ × texH_

    int currentStride() const;
    void updateTexture();
    void destroyTexture();
    void buildPlanePixels(IDebugBackend &backend);

    // Sub-render
    void renderToolbar();
    void renderScreen();

    // Helpers
    void handleZoomInput();
    void handlePanInput(float regionW, float regionH);
    void clampPan(float regionW, float regionH);
};
