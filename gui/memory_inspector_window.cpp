#include "memory_inspector_window.h"
#include "disassembler.h"

// Dear ImGui
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void MemoryInspectorWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;
    
    // Bring dock tab to front when navigated to from another window
    // Must be called BEFORE Begin()
    if (pendingFocus_) {
        ImGui::SetNextWindowFocus();
        pendingFocus_ = false;
    }
    
    // Window flags: movable, resizable, with title bar
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    
    ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin("Memory Inspector", &visible_, flags)) {
        ImGui::End();
        return;
    }
    
    // Refresh snapshot if needed
    if (needsRefresh_) {
        refreshSnapshot(backend);
        needsRefresh_ = false;
    }
    
    // Render toolbar (address input, Go, Follow PC, Refresh)
    renderToolbar(backend);
    
    ImGui::Separator();
    
    // Render memory view (hex + ASCII)
    renderMemoryView(backend);
    
    ImGui::Separator();
    
    // Render disassembly of selected address
    renderDisassembly(backend);
    
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void MemoryInspectorWindow::renderToolbar(IDebugBackend &backend)
{
    // Byte editing mode (Stage 3.5)
    if (editingByte_) {
        ImGui::Text("Edit %04X:", editAddress_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        bool enterPressed = ImGui::InputText("##editbyte", editBuffer_, sizeof(editBuffer_),
            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_AutoSelectAll);
        ImGui::SameLine();
        if (ImGui::Button("Go") || enterPressed) {
            confirmEdit(backend);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            cancelEdit();
        }
        if (writeFailed_) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Write failed: CPU must be Paused");
        }
        return;  // Skip normal toolbar while editing
    }
    
    writeFailed_ = false;
    // Address input
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputText("##addr", addressInput_, sizeof(addressInput_), 
                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        uint16_t addr;
        if (parseAddress(addressInput_, addr)) {
            address_ = addr;
            selectedAddress_ = addr;
        }
    }
    
    ImGui::SameLine();
    
    // Go button
    if (ImGui::Button("Go")) {
        uint16_t addr;
        if (parseAddress(addressInput_, addr)) {
            address_ = addr;
            selectedAddress_ = addr;
        }
    }
    
    ImGui::SameLine();
    
    // Follow PC button
    if (ImGui::Button("Follow PC")) {
        CpuState cpu = backend.getCpuState();
        address_ = cpu.pc;
        selectedAddress_ = cpu.pc;
        snprintf(addressInput_, sizeof(addressInput_), "%04X", cpu.pc);
    }
    
    ImGui::SameLine();
    
    // Refresh button
    if (ImGui::Button("Refresh")) {
        needsRefresh_ = true;
    }
    
    ImGui::SameLine();
    ImGui::Text("Address: %04X  Selected: %04X", address_, selectedAddress_);
}

// ---------------------------------------------------------------------------
// Memory View (hex + ASCII)
// ---------------------------------------------------------------------------

void MemoryInspectorWindow::renderMemoryView(IDebugBackend &backend)
{
    if (snapshot_.data.empty()) {
        ImGui::Text("No data. Click Refresh to load memory.");
        return;
    }
    
    // Get current PC for highlighting
    CpuState cpu = backend.getCpuState();
    uint16_t pc = cpu.pc;
    
    // Calculate visible range
    const int bytesPerLine = 16;
    const size_t totalBytes = snapshot_.data.size();
    const size_t totalLines = (totalBytes + bytesPerLine - 1) / bytesPerLine;
    
    // Character width for click calculation (monospace font)
    float charWidth = ImGui::CalcTextSize("M").x;
    if (charWidth < 1.0f) charWidth = 8.0f;
    
    // Child window for scrolling
    ImGui::BeginChild("MemoryScroll", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 4), 
                      ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    
    // Use clipper for performance with large memory ranges
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(totalLines));
    
    while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
            size_t lineOffset = static_cast<size_t>(line) * bytesPerLine;
            uint16_t lineAddr = static_cast<uint16_t>((snapshot_.start + lineOffset) & 0xFFFF);
            
            // Check if this line contains PC
            bool containsPc = false;
            for (int i = 0; i < bytesPerLine; ++i) {
                size_t offset = lineOffset + i;
                if (offset >= totalBytes) break;
                uint16_t addr = static_cast<uint16_t>((snapshot_.start + offset) & 0xFFFF);
                if (addr == pc) containsPc = true;
            }
            
            // Check if this line contains a breakpoint
            bool containsBp = false;
            for (int i = 0; i < bytesPerLine; ++i) {
                size_t offset = lineOffset + i;
                if (offset >= totalBytes) break;
                uint16_t addr = static_cast<uint16_t>((snapshot_.start + offset) & 0xFFFF);
                if (backend.hasBreakpoint(addr)) containsBp = true;
            }

            // Check if this line contains the selected address
            bool containsSelected = false;
            int selectedByteIndex = -1;
            for (int i = 0; i < bytesPerLine; ++i) {
                size_t offset = lineOffset + i;
                if (offset >= totalBytes) break;
                uint16_t addr = static_cast<uint16_t>((snapshot_.start + offset) & 0xFFFF);
                if (addr == selectedAddress_) {
                    containsSelected = true;
                    selectedByteIndex = i;
                }
            }

            // Highlight line with PC
            if (containsPc) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.5f, 1.0f));
            }
            
            // Make line selectable (for click-to-select-row)
            ImGui::PushID(line);
            
            // Address column with breakpoint marker (Stage 3.7)
            char lineBuf[256];
            int pos = 0;
            if (containsBp) {
                pos += snprintf(lineBuf, sizeof(lineBuf), "\xe2\x97\x8f ");  // red dot ●
            } else {
                pos += snprintf(lineBuf, sizeof(lineBuf), "  ");
            }
            pos += snprintf(lineBuf + pos, sizeof(lineBuf) - pos, "%04X: ", lineAddr);
            
            // Track byte positions in lineBuf for accurate highlight positioning
            int hexBytePos[16];  // byte position in lineBuf for each hex byte
            
            // Hex column
            for (int i = 0; i < bytesPerLine; ++i) {
                hexBytePos[i] = pos;  // record position before this byte
                size_t offset = lineOffset + i;
                if (offset >= totalBytes) {
                    pos += snprintf(lineBuf + pos, sizeof(lineBuf) - pos, "   ");
                } else {
                    uint8_t byte = snapshot_.data[offset];
                    pos += snprintf(lineBuf + pos, sizeof(lineBuf) - pos, "%02X ", byte);
                }
            }
            int hexEndPos = pos;  // position after hex section
            
            // Separator
            pos += snprintf(lineBuf + pos, sizeof(lineBuf) - pos, " |");
            int asciiStartPos = pos;  // position where ASCII section starts
            
            // Track ASCII byte positions
            int asciiBytePos[16];  // byte position in lineBuf for each ASCII char
            
            // ASCII column
            for (int i = 0; i < bytesPerLine; ++i) {
                asciiBytePos[i] = pos;  // record position before this char
                size_t offset = lineOffset + i;
                if (offset >= totalBytes) {
                    lineBuf[pos++] = ' ';
                } else {
                    uint8_t byte = snapshot_.data[offset];
                    if (byte >= 0x20 && byte <= 0x7E) {
                        lineBuf[pos++] = static_cast<char>(byte);
                    } else {
                        lineBuf[pos++] = '.';
                    }
                }
            }
            lineBuf[pos++] = '|';
            lineBuf[pos] = '\0';
            
            // Render the full line as text (not selectable — we draw cell highlight manually)
            ImGui::TextUnformatted(lineBuf);
            
            // Draw highlight rectangle over the selected byte cell
            if (containsSelected && selectedByteIndex >= 0) {
                ImDrawList *drawList = ImGui::GetWindowDrawList();
                ImVec2 textStart = ImGui::GetItemRectMin();
                float lineH = ImGui::GetTextLineHeight();
                
                // Measure actual pixel positions using CalcTextSize
                float hexCellStartX = textStart.x + ImGui::CalcTextSize(lineBuf, lineBuf + hexBytePos[selectedByteIndex]).x;
                float hexCellEndX = textStart.x + ImGui::CalcTextSize(lineBuf, lineBuf + hexBytePos[selectedByteIndex] + 3).x;  // "XX " = 3 chars
                
                // Hex cell highlight
                ImColor highlightCol(ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive]);
                highlightCol.Value.w = 0.5f;
                drawList->AddRectFilled(
                    ImVec2(hexCellStartX, textStart.y),
                    ImVec2(hexCellEndX, textStart.y + lineH),
                    highlightCol);
                
                // ASCII cell highlight
                float asciiCellStartX = textStart.x + ImGui::CalcTextSize(lineBuf, lineBuf + asciiBytePos[selectedByteIndex]).x;
                float asciiCellEndX = textStart.x + ImGui::CalcTextSize(lineBuf, lineBuf + asciiBytePos[selectedByteIndex] + 1).x;  // 1 char
                
                drawList->AddRectFilled(
                    ImVec2(asciiCellStartX, textStart.y),
                    ImVec2(asciiCellEndX, textStart.y + lineH),
                    highlightCol);
            }
            
            // Handle clicks on the line
            if (ImGui::IsItemClicked()) {
                ImVec2 textStart = ImGui::GetItemRectMin();
                float mouseX = ImGui::GetIO().MousePos.x;
                float mouseRelX = mouseX - textStart.x;
                
                // Find which byte was clicked by comparing mouse X with measured byte positions
                int byteIndex = -1;
                
                // Check hex section
                for (int i = 0; i < bytesPerLine; ++i) {
                    float cellStartX = ImGui::CalcTextSize(lineBuf, lineBuf + hexBytePos[i]).x;
                    float cellEndX = ImGui::CalcTextSize(lineBuf, lineBuf + hexBytePos[i] + 3).x;
                    if (mouseRelX >= cellStartX && mouseRelX < cellEndX) {
                        byteIndex = i;
                        break;
                    }
                }
                
                // Check ASCII section if not found in hex
                if (byteIndex < 0) {
                    for (int i = 0; i < bytesPerLine; ++i) {
                        float cellStartX = ImGui::CalcTextSize(lineBuf, lineBuf + asciiBytePos[i]).x;
                        float cellEndX = ImGui::CalcTextSize(lineBuf, lineBuf + asciiBytePos[i] + 1).x;
                        if (mouseRelX >= cellStartX && mouseRelX < cellEndX) {
                            byteIndex = i;
                            break;
                        }
                    }
                }
                
                if (byteIndex >= 0) {
                    size_t clickOffset = lineOffset + byteIndex;
                    if (clickOffset < totalBytes) {
                        uint16_t clickedAddr = static_cast<uint16_t>((snapshot_.start + clickOffset) & 0xFFFF);
                        selectedAddress_ = clickedAddr;
                        
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            beginEditByte(clickedAddr);
                        }
                    }
                }
            }
            
            // Right-click context menu for breakpoints (Stage 3.7) + navigation (Stage 3.9) + byte edit
            if (ImGui::BeginPopupContextItem("bpctx")) {
                // Edit byte
                if (ImGui::MenuItem("Edit Byte")) {
                    beginEditByte(selectedAddress_);
                }
                ImGui::Separator();
                // Breakpoint toggle
                if (backend.hasBreakpoint(selectedAddress_)) {
                    if (ImGui::MenuItem("Remove Breakpoint")) {
                        backend.removeBreakpoint(selectedAddress_);
                    }
                } else {
                    if (ImGui::MenuItem("Set Breakpoint")) {
                        backend.addBreakpoint(selectedAddress_);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Go to Disassembly")) {
                    if (onGoToDisassembly) {
                        onGoToDisassembly(selectedAddress_);
                    }
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
            
            if (containsPc) {
                ImGui::PopStyleColor();
            }
        }
    }
    clipper.End();
    
    // Handle arrow key navigation
    if (ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            selectedAddress_--;
            needsRefresh_ = false;  // don't need full refresh, just scroll
            pendingScroll_ = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            selectedAddress_++;
            needsRefresh_ = false;
            pendingScroll_ = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            selectedAddress_ -= bytesPerLine;
            needsRefresh_ = false;
            pendingScroll_ = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            selectedAddress_ += bytesPerLine;
            needsRefresh_ = false;
            pendingScroll_ = true;
        }
    }
    
    // Stage 3.9: real scroll to selected address
    if (pendingScroll_) {
        // Calculate which line contains selectedAddress_
        size_t offset = static_cast<size_t>((selectedAddress_ - snapshot_.start) & 0xFFFF);
        int targetLine = static_cast<int>(offset / bytesPerLine);
        float lineY = targetLine * ImGui::GetTextLineHeightWithSpacing();
        ImGui::SetScrollY(lineY - ImGui::GetWindowHeight() * 0.4f);
        pendingScroll_ = false;
    }
    
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Disassembly
// ---------------------------------------------------------------------------

void MemoryInspectorWindow::renderDisassembly(IDebugBackend &backend)
{
    ImGui::Text("Disassembly at %04X:", selectedAddress_);
    
    // Create read function for disassembler
    auto readFn = [&backend](uint16_t addr) -> uint8_t {
        return backend.readMemory(addr);
    };
    
    // Disassemble a few instructions starting from selected address
    uint16_t addr = selectedAddress_;
    for (int i = 0; i < 5; ++i) {
        DisassembledInstruction instr = disassemble(addr, readFn);
        
        // Format: "ADDR: BYTES  MNEMONIC OPERANDS"
        char buf[128];
        int pos = snprintf(buf, sizeof(buf), "  %04X: ", instr.address);
        
        // Bytes
        for (int j = 0; j < instr.length; ++j) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", instr.bytes[j]);
        }
        // Pad to align mnemonics
        for (int j = instr.length; j < 3; ++j) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "   ");
        }
        
        // Mnemonic + operands
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", instr.text.c_str());
        
        ImGui::Text("%s", buf);
        
        // Advance to next instruction
        addr = static_cast<uint16_t>((addr + instr.length) & 0xFFFF);
    }
}

// ---------------------------------------------------------------------------
// Snapshot management
// ---------------------------------------------------------------------------

void MemoryInspectorWindow::refreshSnapshot(IDebugBackend &backend)
{
    // Load entire 64 KB
    snapshot_ = backend.readMemorySnapshot(0x0000, 0x10000);
}

// ---------------------------------------------------------------------------
// Address parsing
// ---------------------------------------------------------------------------

bool MemoryInspectorWindow::parseAddress(const char *input, uint16_t &address) const
{
    if (!input || !*input) return false;
    
    // Skip leading whitespace
    while (*input && std::isspace(*input)) ++input;
    
    // Check for 0x prefix
    bool hasPrefix = false;
    if (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
        input += 2;
        hasPrefix = true;
    }
    
    // Parse hex digits
    unsigned long value = 0;
    int digits = 0;
    while (*input && std::isxdigit(*input)) {
        char c = *input++;
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        
        value = (value << 4) | digit;
        ++digits;
    }
    
    // Must have at least one digit
    if (digits == 0) return false;
    
    // Must fit in 16 bits
    if (value > 0xFFFF) return false;
    
    address = static_cast<uint16_t>(value);
    return true;
}

// ---------------------------------------------------------------------------
// Byte editing (Stage 3.5)
// ---------------------------------------------------------------------------

void MemoryInspectorWindow::beginEditByte(uint16_t address)
{
    editingByte_ = true;
    editAddress_ = address;
    writeFailed_ = false;
    
    // Pre-fill with current value from snapshot
    size_t offset = static_cast<size_t>((address - snapshot_.start) & 0xFFFF);
    if (offset < snapshot_.data.size()) {
        snprintf(editBuffer_, sizeof(editBuffer_), "%02X", snapshot_.data[offset]);
    } else {
        editBuffer_[0] = '\0';
    }
}

void MemoryInspectorWindow::confirmEdit(IDebugBackend &backend)
{
    // Parse hex value
    unsigned int value = 0;
    if (sscanf(editBuffer_, "%x", &value) != 1 || value > 0xFF) {
        cancelEdit();
        return;
    }
    
    uint8_t byte = static_cast<uint8_t>(value);
    
    // Write through backend (goes through emulation thread command protocol)
    bool ok = backend.writeMemoryByte(editAddress_, byte);
    
    if (ok) {
        // Write succeeded — refresh snapshot to show new value
        cancelEdit();
        needsRefresh_ = true;
    } else {
        // Write failed (not paused)
        writeFailed_ = true;
    }
}

void MemoryInspectorWindow::cancelEdit()
{
    editingByte_ = false;
    writeFailed_ = false;
}
