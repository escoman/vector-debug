#include "vector_screen_window.h"
#include "idebug_backend.h"
#include "vram_mapping.h"

// Dear ImGui
#include "imgui.h"

// OpenGL
#include "SDL.h"
#include "SDL_opengl.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

VectorScreenWindow::~VectorScreenWindow()
{
    destroyTexture();
}

void VectorScreenWindow::destroyTexture()
{
    if (textureId_) {
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
    }
    texWidth_  = 0;
    texHeight_ = 0;
}

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------

void VectorScreenWindow::updateTexture(const uint32_t *pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0) return;

    if (textureId_ == 0 || texWidth_ != width || texHeight_ != height) {
        destroyTexture();
        glGenTextures(1, &textureId_);
        glBindTexture(GL_TEXTURE_2D, textureId_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

        // SDL_PIXELFORMAT_ARGB8888 on little-endian = BGRA bytes in memory
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, pixels);

        texWidth_  = width;
        texHeight_ = height;
    } else {
        glBindTexture(GL_TEXTURE_2D, textureId_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                        GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
}

// ---------------------------------------------------------------------------
// Zoom helpers
// ---------------------------------------------------------------------------

void VectorScreenWindow::zoomIn()
{
    if (zoomLevel_ < ZOOM_LEVELS - 1) {
        zoomLevel_++;
        fitToWindow_ = false;
    }
}

void VectorScreenWindow::zoomOut()
{
    if (zoomLevel_ > 0) {
        zoomLevel_--;
        fitToWindow_ = false;
    }
}

void VectorScreenWindow::zoomReset()
{
    zoomLevel_ = 0;
    panX_ = panY_ = 0;
    fitToWindow_ = false;
}

// ---------------------------------------------------------------------------
// Pan clamping
// ---------------------------------------------------------------------------

void VectorScreenWindow::clampPan(int visibleW, int visibleH, int zoomFactor,
                                  float regionW, float regionH)
{
    float totalW = static_cast<float>(visibleW * zoomFactor);
    float totalH = static_cast<float>(visibleH * zoomFactor);

    // Pan offset range: 0 .. (totalSize - regionSize)
    float maxPanX = (totalW > regionW) ? totalW - regionW : 0;
    float maxPanY = (totalH > regionH) ? totalH - regionH : 0;

    panX_ = std::max(0.0f, std::min(panX_, maxPanX));
    panY_ = std::max(0.0f, std::min(panY_, maxPanY));
}

// ---------------------------------------------------------------------------
// Write highlight detection
// ---------------------------------------------------------------------------

void VectorScreenWindow::updateWriteHighlights(IDebugBackend &backend)
{
    auto activity = backend.activitySnapshot();

    if (prevWriteCounts_.empty()) {
        // First call — just store current counts
        prevWriteCounts_ = activity.writeCount;
        changedFlags_.assign(256, 0);
        return;
    }

    bool anyChanged = false;
    changedFlags_.assign(256, 0);

    // Check VRAM range 0xC000..0xC0FF for new writes
    for (int i = 0; i < 256; ++i) {
        uint16_t addr = 0xC000 + i;
        if (activity.writeCount[addr] > prevWriteCounts_[addr]) {
            changedFlags_[i] = 1;
            anyChanged = true;
        }
    }

    if (anyChanged) {
        highlightTimer_ = highlightFadeFrames_;
    }

    if (highlightTimer_ > 0) {
        highlightTimer_--;
    }

    prevWriteCounts_ = activity.writeCount;
}

// ---------------------------------------------------------------------------
// Capture current frame
// ---------------------------------------------------------------------------

void VectorScreenWindow::captureCurrentFrame(IDebugBackend &backend)
{
    auto snap = backend.screenSnapshot();
    if (!snap.pixels.empty()) {
        capturedPixels_ = snap.pixels;
        capturedWidth_  = snap.width;
        capturedHeight_ = snap.height;
    }
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void VectorScreenWindow::renderToolbar(IDebugBackend &backend)
{
    // Zoom buttons
    for (int i = 0; i < ZOOM_LEVELS; ++i) {
        char label[16];
        snprintf(label, sizeof(label), "%dx", ZOOM_FACTORS[i]);
        if (i > 0) ImGui::SameLine();
        bool isActive = (zoomLevel_ == i && !fitToWindow_);
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        }
        if (ImGui::Button(label, ImVec2(40, 0))) {
            zoomLevel_ = i;
            fitToWindow_ = false;
        }
        if (isActive) {
            ImGui::PopStyleColor();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("-")) zoomOut();
    ImGui::SameLine();
    if (ImGui::Button("+")) zoomIn();
    ImGui::SameLine();
    if (ImGui::Button("1x##reset")) zoomReset();

    ImGui::SameLine();
    ImGui::Checkbox("Fit", &fitToWindow_);

    // Separator
    ImGui::SameLine();
    ImGui::Text("|");

    // Live / Capture
    ImGui::SameLine();
    if (ImGui::Button(liveMode_ ? "Live" : "Capture")) {
        if (liveMode_) {
            // Switch to capture: grab current frame
            captureCurrentFrame(backend);
            liveMode_ = false;
        } else {
            liveMode_ = true;
        }
    }

    ImGui::SameLine();
    ImGui::Text("|");

    // VRAM Debug Mode
    ImGui::SameLine();
    ImGui::Checkbox("VRAM Debug", &vramDebugMode_);

    if (vramDebugMode_) {
        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();
        ImGui::Checkbox("Grid", &showByteGrid_);
        ImGui::SameLine();
        ImGui::Checkbox("Addr", &showAddress_);
        ImGui::SameLine();
        ImGui::Checkbox("Changed", &showChangedBytes_);
        ImGui::SameLine();
        ImGui::Checkbox("WrCount", &showWriteCount_);

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();
        if (ImGui::Button("Clear Activity")) {
            backend.clearActivityCounters();
            changedFlags_.assign(256, 0);
            highlightTimer_ = 0;
            prevWriteCounts_.clear();
        }
    }
}

// ---------------------------------------------------------------------------
// Coordinate info bar
// ---------------------------------------------------------------------------

void VectorScreenWindow::renderCoordinateInfo(IDebugBackend &backend)
{
    auto video = backend.videoModeSnapshot();

    // Content width depends on hardware mode
    const int contentW = video.visibleWidth;  // 256 or 512
    const int contentH = 256;

    if (hoverScreenX_ >= 0 && hoverScreenY_ >= 0) {
        // Convert framebuffer X to content X (divide by 2 in 256-mode due to pixel doubling)
        int contentX = video.mode512 ? hoverScreenX_ : (hoverScreenX_ / 2);
        const char *modeStr = video.mode512 ? "512" : "256";
        ImGui::Text("Screen: %s %dx%d  X: %d  Y: %d",
                    modeStr, contentW, contentH,
                    contentX, hoverScreenY_);
    } else if (hoverBorder_) {
        const char *modeStr = video.mode512 ? "512" : "256";
        ImGui::Text("Screen: %s %dx%d  (border area)",
                    modeStr, contentW, contentH);
    } else {
        const char *modeStr = video.mode512 ? "512" : "256";
        ImGui::Text("Screen: %s %dx%d  (hover for coordinates)",
                    modeStr, contentW, contentH);
    }
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

void VectorScreenWindow::renderContextMenu(IDebugBackend &backend)
{
    if (!ImGui::BeginPopupContextWindow()) return;

    auto video = backend.videoModeSnapshot();

    // Framebuffer layout: 576x288 with 512-pixel visible area
    const int fbVisW = 512;
    const int fbVisH = 256;
    const int fbBorderL = (video.screenWidth  - fbVisW) / 2;
    const int fbBorderT = (video.screenHeight - fbVisH) / 2;
    int displayPPB = video.pixelsPerByte * fbVisW / video.visibleWidth;

    VramMapping::VideoInfo vi = VramMapping::fromVideoMode(
        video.mode512, video.screenWidth, video.screenHeight,
        fbVisW, fbVisH,
        fbBorderL, fbBorderT,
        video.scrollValue, video.vramBase, displayPPB);

    uint16_t vramAddr = 0;
    bool hasVramAddr = false;

    if (hoverBufX_ >= 0 && hoverBufY_ >= 0) {
        auto result = VramMapping::screenToVram(hoverBufX_, hoverBufY_, vi);
        if (result.count > 0) {
            vramAddr = result.addresses[0];
            hasVramAddr = true;
        }
    }

    if (hasVramAddr) {
        char label[64];
        snprintf(label, sizeof(label), "Go to VRAM (%04X)", vramAddr);
        if (ImGui::MenuItem(label)) {
            if (onGoToMemoryInspector) onGoToMemoryInspector(vramAddr);
        }

        if (ImGui::MenuItem("Go to Memory Inspector")) {
            if (onGoToMemoryInspector) onGoToMemoryInspector(vramAddr);
        }
    }

    // Go to Disassembly — if we have last-write PC
    if (hasVramAddr && vramAddr >= 0xC000 && vramAddr < 0xC100) {
        auto vramSnap = backend.vramWriteSnapshot();
        int idx = vramAddr - 0xC000;
        auto &info = vramSnap.lastWrite[idx];
        if (info.sequence > 0) {
            char label[96];
            snprintf(label, sizeof(label), "Go to Instruction (PC=%04X)", info.pc);
            if (ImGui::MenuItem(label)) {
                if (onGoToDisassembly) onGoToDisassembly(info.pc);
            }
        }
    }

    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// VRAM Debug overlay (drawn on top of the screen image)
// ---------------------------------------------------------------------------

void VectorScreenWindow::renderVramDebugOverlay(IDebugBackend &backend)
{
    if (!vramDebugMode_) return;

    auto video = backend.videoModeSnapshot();

    // Framebuffer layout: 576x288 with 512-pixel visible area
    const int fbVisW = 512;
    const int fbVisH = 256;
    const int fbBorderL = (video.screenWidth  - fbVisW) / 2;
    const int fbBorderT = (video.screenHeight - fbVisH) / 2;
    int displayPPB = video.pixelsPerByte * fbVisW / video.visibleWidth;

    VramMapping::VideoInfo vi = VramMapping::fromVideoMode(
        video.mode512, video.screenWidth, video.screenHeight,
        fbVisW, fbVisH,
        fbBorderL, fbBorderT,
        video.scrollValue, video.vramBase, displayPPB);

    int zoom = currentZoom();
    if (zoom < 8) return;  // Byte grid only at high zoom

    // Get the drawing cursor position (top-left of the image region)
    ImVec2 imgMin = ImGui::GetItemRectMin();
    ImVec2 imgMax = ImGui::GetItemRectMax();

    ImDrawList *drawList = ImGui::GetWindowDrawList();

    // The image now includes borders.  The visible screen area starts at
    // (fbBorderL, fbBorderT) within the image.
    int visW = fbVisW;
    int visH = fbVisH;
    int ppb  = displayPPB;

    // Bytes per row in visible area
    int bytesPerRow = visW / ppb;  // 32 for 256-mode, 128 for 512-mode
    int bytesPerCol = visH;         // 256

    // Cell size in pixels
    float cellW = static_cast<float>(ppb * zoom);
    float cellH = static_cast<float>(zoom);

    // Get activity snapshot for write counts and changed detection
    IDebugBackend::ActivitySnapshot activity;
    IDebugBackend::VramWriteSnapshot vramSnap;
    if (showChangedBytes_ || showWriteCount_) {
        activity = backend.activitySnapshot();
    }
    if (showLastWrite_) {
        vramSnap = backend.vramWriteSnapshot();
    }

    // Offset to the start of the visible screen area within the image
    float borderOffsetX = static_cast<float>(fbBorderL * zoom);
    float borderOffsetY = static_cast<float>(fbBorderT * zoom);

    // The visible area rect in screen coordinates
    float visMinX = imgMin.x + borderOffsetX;
    float visMinY = imgMin.y + borderOffsetY;
    float visMaxX = visMinX + visW * static_cast<float>(zoom);
    float visMaxY = visMinY + visH * static_cast<float>(zoom);

    // Clip to the actual image widget bounds
    visMinX = std::max(visMinX, imgMin.x);
    visMinY = std::max(visMinY, imgMin.y);
    visMaxX = std::min(visMaxX, imgMax.x);
    visMaxY = std::min(visMaxY, imgMax.y);

    // Calculate the offset due to pan (for the visible-area grid)
    float gridStartX = visMinX - panX_;
    float gridStartY = visMinY - panY_;

    // Draw byte grid lines (clipped to the visible screen area)
    if (showByteGrid_) {
        ImU32 gridColor = IM_COL32(128, 128, 128, 80);

        // Vertical lines (byte column boundaries)
        for (int col = 0; col <= bytesPerRow; ++col) {
            float x = gridStartX + col * cellW;
            if (x < visMinX || x > visMaxX) continue;
            drawList->AddLine(ImVec2(x, visMinY), ImVec2(x, visMaxY), gridColor);
        }

        // Horizontal lines (one per scanline)
        // Only draw every 8th line to avoid clutter
        int lineStep = (zoom >= 16) ? 1 : 8;
        for (int row = 0; row <= bytesPerCol; row += lineStep) {
            float y = gridStartY + row * cellH;
            if (y < visMinY || y > visMaxY) continue;
            drawList->AddLine(ImVec2(visMinX, y), ImVec2(visMaxX, y), gridColor);
        }
    }

    // Draw changed byte highlights and write counts (within visible screen area)
    if (showChangedBytes_ && highlightTimer_ > 0) {
        float alpha = static_cast<float>(highlightTimer_) / highlightFadeFrames_;
        ImU32 highlightColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.0f, 1.0f, 0.0f, 0.3f * alpha));

        for (int row = 0; row < 256; ++row) {
            for (int col = 0; col < bytesPerRow; ++col) {
                uint16_t addr = static_cast<uint16_t>(0xC000 + col * 256 + row);
                int idx = addr - 0xC000;
                if (idx >= 0 && idx < 256 && changedFlags_[idx]) {
                    float x0 = gridStartX + col * cellW;
                    float y0 = gridStartY + row * cellH;
                    float x1 = x0 + cellW;
                    float y1 = y0 + cellH;
                    // Clip to visible screen region
                    if (x1 > visMinX && x0 < visMaxX &&
                        y1 > visMinY && y0 < visMaxY) {
                        drawList->AddRectFilled(
                            ImVec2(std::max(x0, visMinX), std::max(y0, visMinY)),
                            ImVec2(std::min(x1, visMaxX), std::min(y1, visMaxY)),
                            highlightColor);
                    }
                }
            }
        }
    }

    // Draw VRAM addresses (only at very high zoom)
    if (showAddress_ && zoom >= 16 && cellW >= 40) {
        for (int row = 0; row < 256; row += 8) {
            for (int col = 0; col < bytesPerRow; ++col) {
                uint16_t addr = static_cast<uint16_t>(0xC000 + col * 256 + row);
                float x = gridStartX + col * cellW + 2;
                float y = gridStartY + row * cellH + 2;

                // Clip
                if (x + 30 < visMinX || x > visMaxX ||
                    y + 10 < visMinY || y > visMaxY) continue;

                char buf[8];
                snprintf(buf, sizeof(buf), "%04X", addr);
                drawList->AddText(ImVec2(x, y), IM_COL32(255, 255, 0, 180), buf);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Screen rendering (main area)
// ---------------------------------------------------------------------------

void VectorScreenWindow::renderScreen(IDebugBackend &backend)
{
    if (textureId_ == 0) {
        ImGui::TextDisabled("(texture not created)");
        return;
    }

    auto video = backend.videoModeSnapshot();

    // The framebuffer always includes borders: 576x288 total.
    // The visible screen area is 512x256 centered within it.
    const int fullW = video.screenWidth;   // 576
    const int fullH = video.screenHeight;  // 288
    const int displayVisW = 512;
    const int displayVisH = 256;
    const int displayBorderL = (fullW - displayVisW) / 2;  // 32
    const int displayBorderT = (fullH - displayVisH) / 2;  // 16

    int zoom = currentZoom();

    // Calculate display size for the full framebuffer (including borders)
    float totalW = static_cast<float>(fullW * zoom);
    float totalH = static_cast<float>(fullH * zoom);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    // Reserve space for status bar at the bottom
    float statusBarHeight = ImGui::GetTextLineHeightWithSpacing() + 6;
    avail.y -= statusBarHeight;

    ImVec2 imageSize;
    if (fitToWindow_) {
        float aspect = static_cast<float>(fullH) / fullW;
        float w = avail.x;
        float h = w * aspect;
        if (h > avail.y) {
            h = avail.y;
            w = h / aspect;
        }
        imageSize = ImVec2(w, h);
    } else {
        imageSize = ImVec2(totalW, totalH);
    }

    // UV coordinates: show the entire framebuffer (including borders)
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = static_cast<float>(fullW) / texWidth_;
    float v1 = static_cast<float>(fullH) / texHeight_;

    // Create a child region for scrolling/panning
    ImVec2 childSize;
    if (fitToWindow_) {
        childSize = avail;
    } else {
        childSize = ImVec2(
            std::min(totalW, avail.x),
            std::min(totalH, avail.y));
    }

    ImGui::BeginChild("##screen_scroll", childSize, false,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    // Handle zoom input (Ctrl+wheel)
    handleZoomInput();

    // Handle pan input (left mouse drag)
    handlePanInput(fullW, fullH, zoom, childSize.x, childSize.y);

    // Clamp pan
    if (!fitToWindow_) {
        clampPan(fullW, fullH, zoom, childSize.x, childSize.y);
    }

    // Calculate image position with pan offset
    ImVec2 imgPos(canvasPos.x - panX_, canvasPos.y - panY_);
    ImVec2 imgEnd(imgPos.x + imageSize.x, imgPos.y + imageSize.y);

    // Push clip rect
    ImGui::PushClipRect(canvasPos,
                        ImVec2(canvasPos.x + childSize.x, canvasPos.y + childSize.y),
                        true);

    // Render the image
    ImGui::GetWindowDrawList()->AddImage(
        static_cast<ImTextureID>(static_cast<uint64_t>(textureId_)),
        imgPos, imgEnd,
        ImVec2(u0, v0), ImVec2(u1, v1));

    // VRAM debug overlay (drawn on top)
    // We need to set the cursor to the image position for overlay rendering
    ImGui::SetCursorScreenPos(imgPos);
    ImGui::Dummy(imageSize);  // invisible item to set rect bounds
    renderVramDebugOverlay(backend);

    ImGui::PopClipRect();

    // Detect hover position
    ImVec2 mousePos = ImGui::GetMousePos();
    ImVec2 relPos(mousePos.x - imgPos.x, mousePos.y - imgPos.y);
    hoverBorder_ = false;

    if (relPos.x >= 0 && relPos.x < imageSize.x &&
        relPos.y >= 0 && relPos.y < imageSize.y &&
        ImGui::IsWindowHovered()) {

        // Convert to framebuffer coordinates
        float fx = relPos.x / imageSize.x * fullW;
        float fy = relPos.y / imageSize.y * fullH;
        int bufX = static_cast<int>(fx);
        int bufY = static_cast<int>(fy);

        hoverBufX_ = bufX;
        hoverBufY_ = bufY;

        // Check if within the visible screen area (not border)
        int screenX = bufX - displayBorderL;
        int screenY = bufY - displayBorderT;
        if (screenX >= 0 && screenX < displayVisW &&
            screenY >= 0 && screenY < displayVisH) {
            hoverScreenX_ = screenX;
            hoverScreenY_ = screenY;
        } else {
            hoverScreenX_ = -1;
            hoverScreenY_ = -1;
            hoverBorder_ = true;
        }
    } else {
        hoverScreenX_ = -1;
        hoverScreenY_ = -1;
        hoverBufX_ = -1;
        hoverBufY_ = -1;
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Zoom input (Ctrl+wheel)
// ---------------------------------------------------------------------------

void VectorScreenWindow::handleZoomInput()
{
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::GetIO().KeyCtrl && wheel != 0) {
            if (wheel > 0) zoomIn();
            else zoomOut();
        }
    }
}

// ---------------------------------------------------------------------------
// Pan input (left mouse drag)
// ---------------------------------------------------------------------------

void VectorScreenWindow::handlePanInput(int visibleW, int visibleH, int zoomFactor,
                                        float regionW, float regionH)
{
    if (fitToWindow_) return;

    ImGuiIO &io = ImGui::GetIO();

    // Left mouse button for panning
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        isPanning_ = true;
        panStartMouseX_ = io.MousePos.x;
        panStartMouseY_ = io.MousePos.y;
        panStartX_ = panX_;
        panStartY_ = panY_;
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        isPanning_ = false;
    }

    if (isPanning_) {
        float dx = panStartMouseX_ - io.MousePos.x;
        float dy = panStartMouseY_ - io.MousePos.y;
        panX_ = panStartX_ + dx;
        panY_ = panStartY_ + dy;
        clampPan(visibleW, visibleH, zoomFactor, regionW, regionH);
    }
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void VectorScreenWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Vector Screen", &visible_)) {
        ImGui::End();
        return;
    }

    // Toolbar
    renderToolbar(backend);
    ImGui::Separator();

    // Get/update screen data
    if (liveMode_) {
        auto snap = backend.screenSnapshot();
        if (!snap.pixels.empty()) {
            if (snap.pixels != cachedPixels_) {
                cachedPixels_ = snap.pixels;
                updateTexture(snap.pixels.data(), snap.width, snap.height);
            }
            needsRefresh_ = false;
        }
    } else {
        // Capture mode — use cached captured frame
        if (!capturedPixels_.empty() && textureId_ == 0) {
            updateTexture(capturedPixels_.data(), capturedWidth_, capturedHeight_);
        }
    }

    // Update write highlights
    if (vramDebugMode_) {
        updateWriteHighlights(backend);
    }

    // Render screen area
    renderScreen(backend);

    // Context menu
    renderContextMenu(backend);

    // Status bar at the bottom using footer region
    ImGui::Separator();
    renderCoordinateInfo(backend);

    ImGui::End();
}
