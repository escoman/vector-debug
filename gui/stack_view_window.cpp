#include "stack_view_window.h"

// Dear ImGui
#include "imgui.h"

#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void StackViewWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;
    
    // Window flags: movable, resizable, with title bar
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    
    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin("Stack View", &visible_, flags)) {
        ImGui::End();
        return;
    }
    
    // Get current SP
    CpuState cpu = backend.getCpuState();
    currentSP_ = cpu.sp;
    
    // Stage 3.9: Follow SP — detect SP changes and auto-refresh
    // (refreshSnapshot will use currentSP_ if followSP_ is true)
    
    // Refresh snapshot if needed
    if (needsRefresh_) {
        refreshSnapshot(backend);
        needsRefresh_ = false;
    }
    
    // Render toolbar
    renderToolbar(backend);
    
    ImGui::Separator();
    
    // Render stack view
    renderStackView(backend);
    
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void StackViewWindow::renderToolbar(IDebugBackend &backend)
{
    CpuState cpu = backend.getCpuState();
    
    ImGui::Text("SP: %04X", cpu.sp);
    ImGui::SameLine();
    
    // Stage 3.9: Follow SP checkbox (replaces old button)
    if (ImGui::Checkbox("Follow SP", &followSP_)) {
        needsRefresh_ = true;
    }
    
    ImGui::SameLine();
    
    // Refresh button
    if (ImGui::Button("Refresh")) {
        needsRefresh_ = true;
    }
}

// ---------------------------------------------------------------------------
// Stack View
// ---------------------------------------------------------------------------

void StackViewWindow::renderStackView(IDebugBackend &backend)
{
    if (snapshot_.data.empty()) {
        ImGui::Text("No data. Click Refresh to load stack.");
        return;
    }
    
    // Header
    ImGui::Text("Address   Byte   Word");
    ImGui::Separator();
    
    // Scrollable area
    ImGui::BeginChild("StackScroll", ImVec2(0, 0), ImGuiChildFlags_None, 
                      ImGuiWindowFlags_None);
    
    const size_t totalBytes = snapshot_.data.size();
    uint16_t startAddr = snapshot_.start;
    
    for (size_t i = 0; i < totalBytes; ++i) {
        uint16_t addr = static_cast<uint16_t>((startAddr + i) & 0xFFFF);
        uint8_t byteVal = snapshot_.data[i];
        
        // Check if this is SP
        bool isSP = (addr == currentSP_);
        
        // Highlight SP line
        if (isSP) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.5f, 1.0f));
        }
        
        // Format line
        char lineBuf[128];
        int pos = snprintf(lineBuf, sizeof(lineBuf), "%04X:    %02X", addr, byteVal);
        
        // Compute word (little-endian) if we have a next byte
        // Don't compute word for FFFF (no second byte available)
        bool hasWord = false;
        uint16_t wordVal = 0;
        if (i + 1 < totalBytes && addr != 0xFFFF) {
            uint8_t lo = snapshot_.data[i];
            uint8_t hi = snapshot_.data[i + 1];
            wordVal = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
            hasWord = true;
        }
        
        if (hasWord) {
            pos += snprintf(lineBuf + pos, sizeof(lineBuf) - pos, "    %04X", wordVal);
        }
        
        // Render as selectable
        ImGui::PushID(static_cast<int>(i));
        
        if (ImGui::Selectable(lineBuf, false, ImGuiSelectableFlags_None)) {
            // Click on address: navigate to Memory Inspector
            if (onGoToMemoryInspector) {
                onGoToMemoryInspector(addr);
            }
        }
        
        // Context menu (Stage 3.9: added Go to Disassembly)
        if (ImGui::BeginPopupContextItem()) {
            if (onGoToMemoryInspector) {
                char menuText[64];
                snprintf(menuText, sizeof(menuText), "Go to Memory Inspector (%04X)", addr);
                if (ImGui::MenuItem(menuText)) {
                    onGoToMemoryInspector(addr);
                }
            }
            if (onGoToDisassembly) {
                char menuText[64];
                snprintf(menuText, sizeof(menuText), "Go to Disassembly (%04X)", addr);
                if (ImGui::MenuItem(menuText)) {
                    onGoToDisassembly(addr);
                }
            }
            if (hasWord && onGoToDisassembly) {
                ImGui::Separator();
                char menuText[64];
                snprintf(menuText, sizeof(menuText), "Go to Disassembly (Word %04X)", wordVal);
                if (ImGui::MenuItem(menuText)) {
                    onGoToDisassembly(wordVal);
                }
            }
            if (hasWord && onGoToMemoryInspector) {
                char menuText[64];
                snprintf(menuText, sizeof(menuText), "Go to Memory Inspector (Word %04X)", wordVal);
                if (ImGui::MenuItem(menuText)) {
                    onGoToMemoryInspector(wordVal);
                }
            }
            ImGui::EndPopup();
        }
        
        ImGui::PopID();
        
        if (isSP) {
            ImGui::PopStyleColor();
        }
    }
    
    // Stage 3.9: real scroll to target address
    if (pendingScroll_) {
        // Find the line index for viewCenter_ (or currentSP_ if following)
        uint16_t targetAddr = followSP_ ? currentSP_ : viewCenter_;
        int lineIndex = -1;
        for (size_t j = 0; j < totalBytes; ++j) {
            uint16_t a = static_cast<uint16_t>((startAddr + j) & 0xFFFF);
            if (a == targetAddr) { lineIndex = static_cast<int>(j); break; }
        }
        if (lineIndex >= 0) {
            float lineY = lineIndex * ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetScrollY(lineY - ImGui::GetWindowHeight() * 0.4f);
        }
        pendingScroll_ = false;
    }
    
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Snapshot management
// ---------------------------------------------------------------------------

void StackViewWindow::refreshSnapshot(IDebugBackend &backend)
{
    // Stage 3.9: use SP if followSP_, else use viewCenter_
    uint16_t center = followSP_ ? currentSP_ : viewCenter_;
    
    int start, end;
    computeRange(center, start, end);
    
    if (start >= end) {
        snapshot_.data.clear();
        return;
    }
    
    size_t size = static_cast<size_t>(end - start + 1);
    snapshot_ = backend.readMemorySnapshot(static_cast<uint16_t>(start), size);
}

// ---------------------------------------------------------------------------
// Compute safe range around SP
// ---------------------------------------------------------------------------

void StackViewWindow::computeRange(uint16_t sp, int &start, int &end)
{
    // Use int to avoid uint16_t overflow
    int spInt = static_cast<int>(sp);
    
    start = spInt - 32;
    end = spInt + 32;
    
    // Clamp to valid address range
    start = std::max(start, 0);
    end = std::min(end, 0xFFFF);
}
