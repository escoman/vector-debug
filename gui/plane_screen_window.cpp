#include "plane_screen_window.h"
#include "idebug_backend.h"

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

PlaneScreenWindow::~PlaneScreenWindow()
{
    destroyTexture();
}

void PlaneScreenWindow::destroyTexture()
{
    if (textureId_) {
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
    }
    texW_ = 0;
    texH_ = 0;
}

// ---------------------------------------------------------------------------
// Stride
// ---------------------------------------------------------------------------

int PlaneScreenWindow::currentStride() const
{
    // At 1x zoom: stride 1 (no grid, 256×256 texture).
    // At 2x+: stride = zoom + 1, so each logical pixel is stride×stride texels.
    int zoom = currentZoom();
    return (zoomLevel_ >= 1) ? zoom + 1 : 1;
}

// ---------------------------------------------------------------------------
// Build plane pixel data from VRAM
// ---------------------------------------------------------------------------

void PlaneScreenWindow::buildPlanePixels(IDebugBackend &backend)
{
    static const uint16_t planeBases[] = { 0xE000, 0xC000, 0xA000, 0x8000 };
    uint16_t base = planeBases[selectedPlane_];

    auto snapshot = backend.readMemorySnapshot(base, 8192);
    if (snapshot.data.size() < 8192) return;

    const uint8_t *vram = snapshot.data.data();

    // Live activity: per-byte wall-clock timestamps straight from the backend,
    // i.e. the same source the Memory Map uses.  Nothing is cached here, so a
    // highlight fades on its own once the accesses stop — including while the
    // emulator is paused — and no state can survive Reset or a ROM load.  Only
    // the current plane's 8 KB are fetched, not the whole event ring.
    std::vector<IDebugBackend::ByteActivityState> activity;
    if (liveMode_) {
        activity = backend.byteActivitySnapshot(base, 8192);
        if (activity.size() != 8192) activity.clear();
    }

    int stride = currentStride();
    int newW = BASE_SIZE * stride;
    int newH = BASE_SIZE * stride;

    // Normal colors (black and white)
    static constexpr uint32_t kColorSet   = 0xFFCCCCCC;  // light gray
    static constexpr uint32_t kColorUnset = 0xFF000000;  // black
    static constexpr uint32_t kColorGrid  = 0xFF404040;  // dark gray

    // Live mode colors (bright and dark variants for set/unset bits)
    // ARGB format: 0xAARRGGBB
    // Write: red
    static constexpr uint32_t kWriteSet   = 0xFFE04040;  // bright red
    static constexpr uint32_t kWriteUnset = 0xFF402020;  // dark red
    // Read: green
    static constexpr uint32_t kReadSet    = 0xFF40E040;  // bright green
    static constexpr uint32_t kReadUnset  = 0xFF204020;  // dark green
    // Read+Write: yellow
    static constexpr uint32_t kBothSet    = 0xFFE0E040;  // bright yellow
    static constexpr uint32_t kBothUnset  = 0xFF404020;  // dark yellow
    // Fetch (code execution in this plane, opt-in): blue
    static constexpr uint32_t kFetchSet   = 0xFF4090E0;  // bright blue
    static constexpr uint32_t kFetchUnset = 0xFF202838;  // dark blue

    auto now = std::chrono::steady_clock::now();
    // byteActivityRecent() rejects the TimePoint::min() "never accessed" sentinel
    // before doing the subtraction; without that check the difference overflows
    // and every untouched byte would look active.
    auto isActive = [&](std::chrono::steady_clock::time_point tp) -> bool {
        return IDebugBackend::byteActivityRecent(tp, now, ACTIVITY_DURATION);
    };

    // Resize pixel buffer if texture dimensions changed (zoom changed).
    if ((int)planePixels_.size() != newW * newH) {
        planePixels_.resize(newW * newH);
    }
    texW_ = newW;
    texH_ = newH;

    for (int ty = 0; ty < newH; ++ty) {
        int pixelY  = ty / stride;           // logical pixel Y (0..255)
        int cellY   = ty % stride;           // position inside cell
        int byteRow = 255 - pixelY;          // byteRow 0 = bottom → tex Y = top
        bool isGridRow = (stride > 1 && cellY == 0);

        for (int tx = 0; tx < newW; ++tx) {
            int pixelX = tx / stride;         // logical pixel X (0..255)
            int cellX  = tx % stride;
            bool isGridCol = (stride > 1 && cellX == 0);

            uint32_t color;
            if (isGridRow || isGridCol) {
                color = kColorGrid;
            } else {
                int byteCol = pixelX / 8;
                int bit     = 7 - (pixelX % 8);
                int vramOffset = byteCol * 256 + byteRow;
                uint8_t byteVal = vram[vramOffset];
                bool pixelSet = (byteVal >> bit) & 1;

                if (liveMode_ && !activity.empty()) {
                    const auto &state = activity[vramOffset];
                    bool readActive  = isActive(state.lastDataRead);
                    bool writeActive = isActive(state.lastWrite);
                    bool fetchActive = showFetches_ && isActive(state.lastFetch);

                    if (readActive && writeActive) {
                        color = pixelSet ? kBothSet : kBothUnset;
                    } else if (readActive) {
                        color = pixelSet ? kReadSet : kReadUnset;
                    } else if (writeActive) {
                        color = pixelSet ? kWriteSet : kWriteUnset;
                    } else if (fetchActive) {
                        color = pixelSet ? kFetchSet : kFetchUnset;
                    } else {
                        color = pixelSet ? kColorSet : kColorUnset;
                    }
                } else {
                    color = pixelSet ? kColorSet : kColorUnset;
                }
            }
            planePixels_[ty * newW + tx] = color;
        }
    }
}

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------

void PlaneScreenWindow::updateTexture()
{
    if (planePixels_.empty() || texW_ <= 0 || texH_ <= 0) return;

    // Delete old texture if it exists, then recreate with current dimensions.
    if (textureId_) {
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
    }

    glGenTextures(1, &textureId_);
    glBindTexture(GL_TEXTURE_2D, textureId_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW_, texH_, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, planePixels_.data());
}

// ---------------------------------------------------------------------------
// Zoom
// ---------------------------------------------------------------------------

void PlaneScreenWindow::zoomIn()
{
    if (zoomLevel_ < ZOOM_LEVELS - 1) zoomLevel_++;
}

void PlaneScreenWindow::zoomOut()
{
    if (zoomLevel_ > 0) zoomLevel_--;
}

// ---------------------------------------------------------------------------
// Pan
// ---------------------------------------------------------------------------

void PlaneScreenWindow::clampPan(float regionW, float regionH)
{
    float totalW = static_cast<float>(texW_);
    float totalH = static_cast<float>(texH_);

    float maxPanX = (totalW > regionW) ? totalW - regionW : 0;
    float maxPanY = (totalH > regionH) ? totalH - regionH : 0;

    panX_ = std::max(0.0f, std::min(panX_, maxPanX));
    panY_ = std::max(0.0f, std::min(panY_, maxPanY));
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void PlaneScreenWindow::renderToolbar()
{
    // Plane selector buttons
    const char *planeNames[] = { "Plane 0", "Plane 1", "Plane 2", "Plane 3" };
    const char *planeAddrs[] = { "E000", "C000", "A000", "8000" };

    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        char label[32];
        snprintf(label, sizeof(label), "%s##%s", planeNames[i], planeAddrs[i]);
        bool isActive = (selectedPlane_ == i);
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        }
        if (ImGui::Button(label, ImVec2(70, 0))) {
            selectedPlane_ = i;
        }
        if (isActive) {
            ImGui::PopStyleColor();
        }
    }

    ImGui::SameLine();
    ImGui::Text("|");

    // Zoom buttons
    ImGui::SameLine();
    if (ImGui::Button("-")) zoomOut();
    ImGui::SameLine();
    if (ImGui::Button("+")) zoomIn();
    ImGui::SameLine();
    for (int i = 0; i < ZOOM_LEVELS; ++i) {
        char label[16];
        snprintf(label, sizeof(label), "%dx", ZOOM_FACTORS[i]);
        if (i > 0) ImGui::SameLine();
        bool isActive = (zoomLevel_ == i);
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        }
        if (ImGui::Button(label, ImVec2(36, 0))) {
            zoomLevel_ = i;
            panX_ = panY_ = 0;
        }
        if (isActive) {
            ImGui::PopStyleColor();
        }
    }

    // Address info
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    static const uint16_t planeBases[] = { 0xE000, 0xC000, 0xA000, 0x8000 };
    uint16_t baseAddr = planeBases[selectedPlane_];
    ImGui::Text("Addr: %04X–%04X", baseAddr, (uint16_t)(baseAddr + 0x1FFF));

    // Live mode checkbox
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::Checkbox("Live", &liveMode_);

    // Fetch display: off by default, a fetch is not a data read
    ImGui::SameLine();
    if (!liveMode_) ImGui::BeginDisabled();
    ImGui::Checkbox("Fetch", &showFetches_);
    if (!liveMode_) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Colour bytes fetched as code (opcode/operand) — blue.\n"
                          "Off by default: a fetch is execution, not a data read.");
    }
}

// ---------------------------------------------------------------------------
// Screen rendering
// ---------------------------------------------------------------------------

void PlaneScreenWindow::renderScreen()
{
    if (textureId_ == 0) {
        ImGui::TextDisabled("(no VRAM data)");
        return;
    }

    // Display image at 1:1 texel-to-pixel ratio.
    float totalW = static_cast<float>(texW_);
    float totalH = static_cast<float>(texH_);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 imageSize(totalW, totalH);

    ImVec2 childSize(
        std::min(totalW, avail.x),
        std::min(totalH, avail.y));

    ImGui::BeginChild("##plane_scroll", childSize, false,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    handleZoomInput();
    handlePanInput(childSize.x, childSize.y);
    clampPan(childSize.x, childSize.y);

    ImVec2 imgPos(canvasPos.x - panX_, canvasPos.y - panY_);
    ImVec2 imgEnd(imgPos.x + imageSize.x, imgPos.y + imageSize.y);

    ImGui::PushClipRect(canvasPos,
                        ImVec2(canvasPos.x + childSize.x, canvasPos.y + childSize.y),
                        true);

    ImGui::GetWindowDrawList()->AddImage(
        static_cast<ImTextureID>(static_cast<uint64_t>(textureId_)),
        imgPos, imgEnd,
        ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));

    ImGui::PopClipRect();
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Zoom input (Ctrl+wheel)
// ---------------------------------------------------------------------------

void PlaneScreenWindow::handleZoomInput()
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

void PlaneScreenWindow::handlePanInput(float regionW, float regionH)
{
    ImGuiIO &io = ImGui::GetIO();

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
        clampPan(regionW, regionH);
    }
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void PlaneScreenWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("VRAM Planes", &visible_)) {
        ImGui::End();
        return;
    }

    // Toolbar
    renderToolbar();
    ImGui::Separator();

    // Read VRAM and build plane image
    buildPlanePixels(backend);
    updateTexture();

    // Render
    renderScreen();

    ImGui::End();
}
