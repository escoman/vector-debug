#pragma once

#include <cstdint>

// Forward declaration
class DebugBackend;

// ---------------------------------------------------------------------------
// VramMapping — screen pixel <-> VRAM address translation
//
// Based on the Vector-06C video system formulas from filler.cpp:
//   VRAM addr = 0xC000 + vramCol * 256 + vramRow
//   vramRow   = (scrollValue + visibleLine) & 0xFF
//   256-mode: vramCol = visibleX / 8  (8 pixels per byte)
//   512-mode: vramCol = visibleX / 4  (4 pixels per byte)
// ---------------------------------------------------------------------------

class VramMapping
{
public:
    // Video mode info (mirrors DebugBackend::VideoModeSnapshot to avoid coupling)
    struct VideoInfo
    {
        bool mode512 = false;
        int screenWidth  = 576;
        int screenHeight = 288;
        int visibleWidth  = 512;
        int visibleHeight = 256;
        int borderLeft = 32;
        int borderTop  = 16;
        int scrollValue = 0;
        uint16_t vramBase = 0xC000;
        int pixelsPerByte = 8;
    };

    // Result of screen-to-VRAM mapping
    struct VramResult
    {
        uint16_t addresses[2];  // up to 2 addresses (for 512-mode sub-pixel)
        int count;              // 0 = border/N/A, 1 or 2
    };

    // Screen framebuffer pixel (bufX, bufY) -> VRAM address(es)
    // bufX/bufY are in framebuffer coordinates (including borders)
    static VramResult screenToVram(int bufX, int bufY,
                                   const VideoInfo &video);

    // Position on screen for a VRAM byte address
    struct ScreenPos
    {
        int x, y;  // in visible-area coordinates (0..visibleWidth-1, 0..255)
        bool valid;
    };

    // VRAM byte address -> screen pixel position (top-left corner of byte area)
    static ScreenPos vramToScreen(uint16_t vramAddr,
                                  const VideoInfo &video);

    // Convert DebugBackend::VideoModeSnapshot to VideoInfo
    static VideoInfo fromVideoMode(bool mode512, int screenWidth, int screenHeight,
                                   int visibleWidth, int visibleHeight,
                                   int borderLeft, int borderTop,
                                   int scrollValue, uint16_t vramBase,
                                   int pixelsPerByte);
};
