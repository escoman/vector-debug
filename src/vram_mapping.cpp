#include "vram_mapping.h"

// ---------------------------------------------------------------------------
// VramMapping — screen pixel <-> VRAM address translation
// ---------------------------------------------------------------------------

VramMapping::VramResult
VramMapping::screenToVram(int bufX, int bufY, const VideoInfo &video)
{
    VramResult result;
    result.addresses[0] = 0;
    result.addresses[1] = 0;
    result.count = 0;

    // Check if within visible area (excluding borders)
    int visLeft   = video.borderLeft;
    int visRight  = video.borderLeft + video.visibleWidth;
    int visTop    = video.borderTop;
    int visBottom = video.borderTop + video.visibleHeight;

    if (bufX < visLeft || bufX >= visRight ||
        bufY < visTop  || bufY >= visBottom) {
        return result;  // border area → count = 0
    }

    // Convert to visible-area coordinates
    int visibleX    = bufX - visLeft;   // 0 .. visibleWidth-1
    int visibleLine = bufY - visTop;    // 0 .. 255

    // Apply scroll register
    int vramRow = (video.scrollValue + visibleLine) & 0xFF;

    // Calculate VRAM column (which byte)
    int vramCol = visibleX / video.pixelsPerByte;

    // VRAM address = base + col * 256 + row
    uint16_t addr = static_cast<uint16_t>(
        video.vramBase + vramCol * 256 + vramRow);

    result.addresses[0] = addr;
    result.count = 1;

    return result;
}

VramMapping::ScreenPos
VramMapping::vramToScreen(uint16_t vramAddr, const VideoInfo &video)
{
    ScreenPos pos;
    pos.x = 0;
    pos.y = 0;
    pos.valid = false;

    if (vramAddr < video.vramBase) return pos;

    int offset = vramAddr - video.vramBase;  // 0..4095
    int vramCol = offset / 256;              // 0..31 (256-mode) or 0..127 (512-mode)
    int vramRow = offset % 256;              // 0..255

    // Reverse the scroll: visibleLine = (vramRow - scrollValue) & 0xFF
    int visibleLine = (vramRow - video.scrollValue) & 0xFF;

    // Check bounds
    int maxCol = (video.mode512 ? 128 : 32);
    if (vramCol >= maxCol) return pos;

    // Convert to screen coordinates (visible area)
    pos.x = vramCol * video.pixelsPerByte;
    pos.y = visibleLine;
    pos.valid = true;

    return pos;
}

VramMapping::VideoInfo
VramMapping::fromVideoMode(bool mode512, int screenWidth, int screenHeight,
                           int visibleWidth, int visibleHeight,
                           int borderLeft, int borderTop,
                           int scrollValue, uint16_t vramBase,
                           int pixelsPerByte)
{
    VideoInfo info;
    info.mode512       = mode512;
    info.screenWidth   = screenWidth;
    info.screenHeight  = screenHeight;
    info.visibleWidth  = visibleWidth;
    info.visibleHeight = visibleHeight;
    info.borderLeft    = borderLeft;
    info.borderTop     = borderTop;
    info.scrollValue   = scrollValue;
    info.vramBase      = vramBase;
    info.pixelsPerByte = pixelsPerByte;
    return info;
}
