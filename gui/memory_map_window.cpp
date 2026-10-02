#include "memory_map_window.h"
#include "idebug_backend.h"
#include "symbol_database.h"

// Dear ImGui
#include "imgui.h"

// OpenGL
#include "SDL_opengl.h"

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// computeBlockColor — pure function, testable without ImGui
// ---------------------------------------------------------------------------

LiveBlockColor computeBlockColor(
    std::chrono::steady_clock::time_point lastRead,
    std::chrono::steady_clock::time_point lastWrite,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds duration)
{
    using TimePoint = std::chrono::steady_clock::time_point;

    auto isActive = [&](TimePoint tp) -> bool {
        if (tp == TimePoint::min()) return false;
        return (now - tp) < duration;
    };

    bool readActive  = isActive(lastRead);
    bool writeActive = isActive(lastWrite);

    if (readActive && writeActive) return LiveBlockColor::Yellow;
    if (readActive)                return LiveBlockColor::Green;
    if (writeActive)               return LiveBlockColor::Red;
    return LiveBlockColor::DarkGray;
}

// ---------------------------------------------------------------------------
// Color → ImU32
// ---------------------------------------------------------------------------

static ImU32 blockColorToImU32(LiveBlockColor c)
{
    switch (c) {
        case LiveBlockColor::DarkGray: return IM_COL32(64, 64, 64, 255);
        case LiveBlockColor::Green:    return IM_COL32(50, 200, 50, 255);
        case LiveBlockColor::Red:      return IM_COL32(200, 50, 50, 255);
        case LiveBlockColor::Yellow:   return IM_COL32(220, 220, 50, 255);
        case LiveBlockColor::Cyan:     return IM_COL32(50, 180, 220, 255);
    }
    return IM_COL32(64, 64, 64, 255);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MemoryMapWindow::MemoryMapWindow()
{
    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint minTime = TimePoint::min();
    for (int i = 0; i < NUM_BLOCKS; ++i) {
        blocks_[i].lastReadTime  = minTime;
        blocks_[i].lastWriteTime = minTime;
        hasData_[i] = false;
    }
    lastScanTime_ = minTime;
}

void MemoryMapWindow::setLive(bool v)
{
    if (v == live_) return;
    live_ = v;
    if (live_) {
        // OFF → ON: clear all timestamps for fresh start
        clearActivity();
    }
}

void MemoryMapWindow::clearActivity()
{
    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint minTime = TimePoint::min();
    for (int i = 0; i < NUM_BLOCKS; ++i) {
        blocks_[i].lastReadTime  = minTime;
        blocks_[i].lastWriteTime = minTime;
    }
}

// ---------------------------------------------------------------------------
// Memory scanning — detect blocks with non-zero data
// ---------------------------------------------------------------------------

void MemoryMapWindow::scanMemoryForData(IDebugBackend &backend)
{
    // Read full 64K in one snapshot for efficiency
    auto snapshot = backend.readMemorySnapshot(0x0000, 65536);

    for (int block = 0; block < NUM_BLOCKS; ++block) {
        bool found = false;
        int base = block * BLOCK_SIZE;
        for (int offset = 0; offset < BLOCK_SIZE; ++offset) {
            if (snapshot.data[base + offset] != 0) {
                found = true;
                break;
            }
        }
        hasData_[block] = found;
    }

    lastScanTime_ = std::chrono::steady_clock::now();
    scanPending_ = false;
}

// ---------------------------------------------------------------------------
// Hit test — map canvas pixel to block index
// ---------------------------------------------------------------------------

int MemoryMapWindow::hitTest(float localX, float localY) const
{
    int x = static_cast<int>(localX);

    // Horizontal: find column (accounting for group separators)
    int foundCol = -1;
    for (int col = 0; col < COLUMNS; ++col) {
        int cellStart = col * (CELL_PX + GRID_PX) + GRID_PX
                      + (col / BLOCKS_PER_GROUP) * (SEPARATOR_PX - GRID_PX);
        int cellEnd = cellStart + CELL_PX;
        if (x >= cellStart && x < cellEnd) { foundCol = col; break; }
    }
    if (foundCol < 0) return -1;

    // Vertical: find row (uniform grid)
    int stride = CELL_PX + GRID_PX;  // 3
    int modY = static_cast<int>(localY) % stride;
    if (modY < GRID_PX) return -1;
    int row = static_cast<int>(localY) / stride;
    if (row < 0 || row >= ROWS) return -1;

    return foundCol * ROWS + row;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void MemoryMapWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Memory Map", &visible_)) {
        ImGui::End();
        return;
    }

    // -- Toolbar -------------------------------------------------------------

    ImGui::Checkbox("Live", &live_);
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        scanPending_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        clearActivity();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("256 blocks");

    ImGui::Separator();

    // -- Auto-scan memory for data presence (every SCAN_INTERVAL) ------------

    auto now_scan = std::chrono::steady_clock::now();
    if (scanPending_ || (now_scan - lastScanTime_) > SCAN_INTERVAL) {
        scanMemoryForData(backend);
    }

    // -- Update block timestamps from backend --------------------------------

    if (live_) {
        auto snap = backend.liveActivitySnapshot();

        for (int i = 0; i < NUM_BLOCKS; ++i) {
            if (snap.blocks[i].lastReadTime > blocks_[i].lastReadTime)
                blocks_[i].lastReadTime = snap.blocks[i].lastReadTime;
            if (snap.blocks[i].lastWriteTime > blocks_[i].lastWriteTime)
                blocks_[i].lastWriteTime = snap.blocks[i].lastWriteTime;
        }
    }

    // -- Draw canvas via ImDrawList ------------------------------------------

    auto *drawList = ImGui::GetWindowDrawList();

    // Reserve space for the canvas (label area above grid for group addresses)
    float labelAreaH = 4.0f;  // base pixels for address labels
    float gap = 6.0f;          // gap between labels and grid
    float scale = 4.0f;
    float gridH = static_cast<float>(canvasHeight());
    ImVec2 scaledSize(static_cast<float>(canvasWidth()) * scale,
                      (labelAreaH + gap + gridH) * scale);

    ImGui::InvisibleButton("##map_canvas", scaledSize);
    ImVec2 canvasMin = ImGui::GetItemRectMin();
    ImVec2 canvasMax = ImGui::GetItemRectMax();

    // Grid area starts below the label region
    float gridTopY = canvasMin.y + labelAreaH * scale;

    // Draw full background (labels area + grid area)
    //drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(32, 32, 32, 255));

    // Draw group address labels above the grid
    {
        ImU32 labelColor = IM_COL32(180, 180, 180, 255);
        float fontSize = ImGui::GetFontSize();
        float textY = canvasMin.y + (labelAreaH * scale - fontSize) * 0.5f;

        for (int g = 0; g < NUM_GROUPS; ++g) {
            int startCol = g * BLOCKS_PER_GROUP;
            // Pixel center of this group (same formula as colX lambda)
            int groupStartPx = startCol * (CELL_PX + GRID_PX) + GRID_PX
                              + (startCol / BLOCKS_PER_GROUP) * (SEPARATOR_PX - GRID_PX);
            int groupEndPx = (startCol + BLOCKS_PER_GROUP - 1) * (CELL_PX + GRID_PX)
                           + GRID_PX + CELL_PX
                           + ((startCol + BLOCKS_PER_GROUP - 1) / BLOCKS_PER_GROUP)
                           * (SEPARATOR_PX - GRID_PX);
            float centerX = canvasMin.x + (groupStartPx + groupEndPx) * 0.5f * scale;

            char buf[8];
            snprintf(buf, sizeof(buf), "%04X", g * 0x2000);
            ImVec2 textSize = ImGui::CalcTextSize(buf);
            float textX = centerX - textSize.x * 0.5f;

            drawList->AddText(ImVec2(textX, textY), labelColor, buf);
        }
    }

    // Helper: pixel X position of column cell start (in base coords)
    auto colX = [](int col) -> int {
        return col * (CELL_PX + GRID_PX) + GRID_PX
             + (col / BLOCKS_PER_GROUP) * (SEPARATOR_PX - GRID_PX);
    };

    // Draw grid lines
    {
        ImU32 gridColor = IM_COL32(48, 48, 48, 255);
        ImU32 sepColor  = IM_COL32(70, 70, 70, 255);

        // Vertical lines (left border, between columns, group separators, right border)
        for (int col = 0; col <= COLUMNS; ++col) {
            float x = canvasMin.x + colX(std::min(col, COLUMNS - 1)) * scale;
            if (col == COLUMNS) {
                // Right border
                x = canvasMin.x + (colX(COLUMNS - 1) + CELL_PX + GRID_PX) * scale;
            }
            bool isGroupSep = (col > 0 && col < COLUMNS && (col % BLOCKS_PER_GROUP) == 0);
            ImU32 color = isGroupSep ? sepColor : gridColor;
            float thickness = isGroupSep ? SEPARATOR_PX * scale : GRID_PX * scale;
            drawList->AddLine(
                ImVec2(x, gridTopY),
                ImVec2(x, gridTopY + gridH * scale),
                color, thickness);
        }
        // Horizontal lines
        for (int row = 0; row <= ROWS; ++row) {
            float y = gridTopY + row * (CELL_PX + GRID_PX) * scale;
            drawList->AddLine(
                ImVec2(canvasMin.x, y),
                ImVec2(canvasMin.x + static_cast<float>(canvasWidth()) * scale, y),
                gridColor, GRID_PX * scale);
        }
    }

    // Draw blocks
    auto now = std::chrono::steady_clock::now();
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLUMNS; ++col) {
            int blockIdx = col * ROWS + row;

            LiveBlockColor color;
            if (live_) {
                LiveBlockColor activity = computeBlockColor(
                    blocks_[blockIdx].lastReadTime,
                    blocks_[blockIdx].lastWriteTime,
                    now,
                    ACTIVITY_DURATION);
                // Live activity takes priority; if no activity, show data presence
                if (activity == LiveBlockColor::DarkGray && hasData_[blockIdx]) {
                    color = LiveBlockColor::Cyan;
                } else {
                    color = activity;
                }
            } else {
                // Not live: show data presence
                color = hasData_[blockIdx] ? LiveBlockColor::Cyan
                                           : LiveBlockColor::DarkGray;
            }

            float x0 = canvasMin.x + colX(col) * scale;
            float y0 = gridTopY + (GRID_PX + row * (CELL_PX + GRID_PX)) * scale;
            float x1 = x0 + CELL_PX * scale;
            float y1 = y0 + CELL_PX * scale;

            drawList->AddRectFilled(
                ImVec2(x0, y0), ImVec2(x1, y1),
                blockColorToImU32(color));
        }
    }

    // -- Mouse interaction ---------------------------------------------------

    if (ImGui::IsItemHovered()) {
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        float localX = (mousePos.x - canvasMin.x) / scale;
        float localY = (mousePos.y - gridTopY) / scale;

        hoverBlock_ = hitTest(localX, localY);

        if (hoverBlock_ >= 0) {
            hoverAddress_ = blockToAddress(hoverBlock_);
            uint16_t endAddr = (hoverAddress_ + BLOCK_SIZE - 1) & 0xFFFF;

            // Tooltip
            ImGui::BeginTooltip();
            ImGui::Text("Block %d: %04X – %04X", hoverBlock_, hoverAddress_, endAddr);
            ImGui::Text("Size: %d bytes", BLOCK_SIZE);

            // Symbol info
            const auto &symbols = backend.symbolDatabase();
            const DebugSymbol *sym = symbols.findSymbol(hoverAddress_);
            if (sym) {
                ImGui::Text("Symbol: %s", sym->name.c_str());
                if (!sym->comment.empty()) {
                    ImGui::Text("%s", sym->comment.c_str());
                }
            }

            MemoryRegionType mrt = symbols.classify(hoverAddress_);
            const char *typeStr = "Unknown";
            if (mrt == MemoryRegionType::Code) typeStr = "Code";
            else if (mrt == MemoryRegionType::Data) typeStr = "Data";
            ImGui::Text("Type: %s", typeStr);

            if (hasData_[hoverBlock_]) {
                ImGui::TextColored(ImVec4(0.2f, 0.7f, 0.86f, 1.0f), "Has data");
            }

            ImGui::EndTooltip();

            // Left click → context menu
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                contextAddress_ = hoverAddress_;
                contextBlock_   = hoverBlock_;
                ImGui::OpenPopup("MapContextMenu");
            }

            // Right click → mark menu
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                contextAddress_ = hoverAddress_;
                contextBlock_   = hoverBlock_;
                ImGui::OpenPopup("MapMarkMenu");
            }
        } else {
            hoverBlock_   = -1;
            hoverAddress_ = 0;
        }
    }

    // -- Context menu: navigation -------------------------------------------

    if (ImGui::BeginPopup("MapContextMenu")) {
        ImGui::Text("Block %d: %04X", contextBlock_, contextAddress_);
        ImGui::Separator();

        if (ImGui::MenuItem("Go to Memory Inspector")) {
            if (onGoToMemoryInspector)
                onGoToMemoryInspector(contextAddress_);
        }
        if (ImGui::MenuItem("Go to Disassembly")) {
            if (onGoToDisassembly)
                onGoToDisassembly(contextAddress_);
        }
        // Stage 6.25: seed the Memory Access window with the block's range
        // and bring it to front.
        if (ImGui::MenuItem("To Memory Access")) {
            if (onGoToMemoryAccess)
                onGoToMemoryAccess(contextAddress_);
        }
        ImGui::EndPopup();
    }

    // -- Context menu: mark as Code/Data/Unknown ----------------------------

    if (ImGui::BeginPopup("MapMarkMenu")) {
        ImGui::Text("Mark %04X as:", contextAddress_);
        ImGui::Separator();

        if (ImGui::MenuItem("Code")) {
            auto &symbols = backend.symbolDatabase();
            symbols.setRegion(contextAddress_,
                             (contextAddress_ + BLOCK_SIZE - 1) & 0xFFFF,
                             MemoryRegionType::Code);
        }
        if (ImGui::MenuItem("Data")) {
            auto &symbols = backend.symbolDatabase();
            symbols.setRegion(contextAddress_,
                             (contextAddress_ + BLOCK_SIZE - 1) & 0xFFFF,
                             MemoryRegionType::Data);
        }
        if (ImGui::MenuItem("Unknown")) {
            auto &symbols = backend.symbolDatabase();
            symbols.removeRegion(contextAddress_);
        }
        ImGui::EndPopup();
    }

    // -- Legend + address labels ---------------------------------------------

    ImGui::Separator();

    // Color legend
    ImGui::ColorButton("##gray",   ImVec4(0.25f, 0.25f, 0.25f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
    ImGui::SameLine(); ImGui::Text("Idle");
    ImGui::SameLine();
    ImGui::ColorButton("##cyan",   ImVec4(0.20f, 0.70f, 0.86f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
    ImGui::SameLine(); ImGui::Text("Data");
    ImGui::SameLine();
    ImGui::ColorButton("##green",  ImVec4(0.20f, 0.78f, 0.20f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
    ImGui::SameLine(); ImGui::Text("Read");
    ImGui::SameLine();
    ImGui::ColorButton("##red",    ImVec4(0.78f, 0.20f, 0.20f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
    ImGui::SameLine(); ImGui::Text("Write");
    ImGui::SameLine();
    ImGui::ColorButton("##yellow", ImVec4(0.86f, 0.86f, 0.20f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
    ImGui::SameLine(); ImGui::Text("R+W");

    ImGui::End();
}
