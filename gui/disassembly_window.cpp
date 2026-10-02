#include "disassembly_window.h"
#include "disassembler.h"
#include "opcode_info.h"
#include "symbol_database.h"

// Dear ImGui
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Address parsing
// ---------------------------------------------------------------------------

bool DisassemblyWindow::parseAddress(const char *input, uint16_t &address) const
{
    unsigned int addr = 0;
    if (sscanf(input, "%x", &addr) != 1) return false;
    if (addr > 0xFFFF) return false;
    address = static_cast<uint16_t>(addr);
    return true;
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void DisassemblyWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;
    
    // Bring dock tab to front when navigated to from another window
    // Must be called BEFORE Begin()
    if (pendingFocus_) {
        ImGui::SetNextWindowFocus();
        pendingFocus_ = false;
    }
    
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin("Disassembly", &visible_)) {
        ImGui::End();
        return;
    }
    
    // Detect PC changes for Follow PC
    CpuState cpu = backend.getCpuState();
    if (!pcInitialized_) {
        lastPc_ = cpu.pc;
        viewAddress_ = cpu.pc;
        pcInitialized_ = true;
    } else if (followPc_ && cpu.pc != lastPc_) {
        viewAddress_ = cpu.pc;
        snprintf(addressInput_, sizeof(addressInput_), "%04X", cpu.pc);
        needsRefresh_ = true;
    }
    lastPc_ = cpu.pc;
    
    // Render toolbar
    renderToolbar(backend);
    
    ImGui::Separator();
    
    // Render disassembly list
    renderDisassemblyList(backend);
    
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void DisassemblyWindow::renderToolbar(IDebugBackend &backend)
{
    // Address input
    ImGui::SetNextItemWidth(60);
    bool enterPressed = ImGui::InputText("##dasmaddr", addressInput_, sizeof(addressInput_),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    
    ImGui::SameLine();
    if (ImGui::Button("Go") || enterPressed) {
        uint16_t addr;
        if (parseAddress(addressInput_, addr)) {
            viewAddress_ = addr;
            followPc_ = false;  // manual navigation disables follow
            needsRefresh_ = true;
        }
    }
    
    ImGui::SameLine();
    if (ImGui::Button("PC")) {
        CpuState cpu = backend.getCpuState();
        viewAddress_ = cpu.pc;
        snprintf(addressInput_, sizeof(addressInput_), "%04X", cpu.pc);
        needsRefresh_ = true;
    }
    
    ImGui::SameLine();
    if (ImGui::Checkbox("Follow PC", &followPc_)) {
        if (followPc_) {
            CpuState cpu = backend.getCpuState();
            viewAddress_ = cpu.pc;
            snprintf(addressInput_, sizeof(addressInput_), "%04X", cpu.pc);
            needsRefresh_ = true;
        }
    }
    
    // Context before current address (applies to both Follow PC ON and OFF modes)
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40);
    if (ImGui::InputInt("##ctxBefore", &contextBeforePc_, 0, 0)) {
        if (contextBeforePc_ < 0) contextBeforePc_ = 0;
        if (contextBeforePc_ > 30) contextBeforePc_ = 30;
        needsRefresh_ = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Instructions before PC");
    }
    
    // Step button
    ImGui::SameLine();
    bool paused = backend.isPaused();
    if (!paused) ImGui::BeginDisabled();
    if (ImGui::Button("Step (F4)")) {
        backend.stepInstruction();
        needsRefresh_ = true;
    }
    
    // Skip button (F6) — run until next instruction
    ImGui::SameLine();
    if (ImGui::Button("Skip (F6)")) {
        backend.requestSkipInstruction();
        needsRefresh_ = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Run until next instruction (F6)");
    }
    if (!paused) ImGui::EndDisabled();
    
    // F6 shortcut — works when paused, checked outside disabled block
    if (paused && ImGui::IsKeyPressed(ImGuiKey_F6)) {
        backend.requestSkipInstruction();
        needsRefresh_ = true;
    }
}

// ---------------------------------------------------------------------------
// Disassembly list
// ---------------------------------------------------------------------------

void DisassemblyWindow::renderDisassemblyList(IDebugBackend &backend)
{
    CpuState cpu = backend.getCpuState();
    uint16_t pc = cpu.pc;
    
    // Get symbol database for name resolution
    auto &symbols = backend.symbolDatabase();
    
    // Determine the anchor address and start address for decoding
    uint16_t anchorAddr = followPc_ ? pc : viewAddress_;
    
    // Go back enough bytes to cover contextBeforePc_ instructions
    // (max i8080 instruction length is 3 bytes)
    int backBytes = contextBeforePc_ * 3;
    if (backBytes > anchorAddr) backBytes = anchorAddr;  // don't wrap below 0
    uint16_t startAddr = static_cast<uint16_t>(anchorAddr - backBytes);
    
    if (followPc_) {
        // Keep viewAddress_ in sync with PC so the address field stays correct
        viewAddress_ = pc;
    }
    
    // Read function: uses IDebugBackend::readMemory() which goes through
    // DebugMemoryAccess::peek() — respects banking, no raw buffer access.
    DisasmReadFn readFn = [&backend](uint16_t addr) -> uint8_t {
        return backend.readMemory(addr);
    };
    
    // Decode instructions forward from startAddr.
    // We collect enough lines to fill the visible area (~40 lines).
    
    const int maxLines = 50;
    const int displayLines = 40;
    
    uint16_t displayStartAddr = startAddr;
    
    // Decode all instructions into a temporary array
    struct Line {
        uint16_t addr;
        DisassembledInstruction instr;
    };
    Line lines[80];  // extra capacity for context region
    int lineCount = 0;
    
    uint16_t addr = startAddr;
    for (int i = 0; i < 80; ++i) {
        lines[lineCount].addr = addr;
        lines[lineCount].instr = disassemble(addr, readFn);
        lineCount++;
        
        uint8_t len = lines[lineCount - 1].instr.length;
        uint16_t nextAddr = static_cast<uint16_t>(addr + len);
        
        // Stop if address wrapped around (nextAddr < addr means overflow)
        if (nextAddr <= addr) break;
        addr = nextAddr;
        
        // Stop if we've decoded enough lines
        if (followPc_) {
            // Show enough lines past PC to fill the display
            if (lineCount > displayLines + contextBeforePc_ && 
                static_cast<int>(static_cast<uint16_t>(addr - pc)) > displayLines * 3) break;
        } else {
            if (lineCount >= maxLines) break;
        }
    }
    
    // Determine which lines to display
    int firstVisible = 0;
    // Find instructions that start before anchorAddr — show the last contextBeforePc_ of them
    int lastBeforeAnchor = -1;
    for (int i = 0; i < lineCount; ++i) {
        if (lines[i].addr >= anchorAddr) {
            lastBeforeAnchor = i;
            break;
        }
        lastBeforeAnchor = i + 1;
    }
    // Show the last contextBeforePc_ instructions before anchor
    firstVisible = lastBeforeAnchor - contextBeforePc_;
    if (firstVisible < 0) firstVisible = 0;
    
    // Child window for scrolling
    ImGui::BeginChild("DasmScroll", ImVec2(0, 0), ImGuiChildFlags_None,
                       ImGuiWindowFlags_HorizontalScrollbar);
    
    int pcLineIndex = -1;
    int displayedCount = 0;
    
    for (int i = firstVisible; i < lineCount && displayedCount < displayLines; ++i) {
        const auto &line = lines[i];
        uint16_t lineAddr = line.addr;
        const auto &instr = line.instr;
        
        bool isPc = (lineAddr == pc);
        bool hasBp = backend.hasBreakpoint(lineAddr);
        
        if (isPc) pcLineIndex = displayedCount;
        
        // Stage 4.6: Check if this address has a symbol label
        const DebugSymbol *sym = symbols.findSymbol(lineAddr);
        bool hasLabel = (sym != nullptr);
        
        // Stage 4.6: Check if this address has a comment
        bool hasComment = hasLabel && !sym->comment.empty();
        
        // Highlight PC line
        if (isPc) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.5f, 1.0f));
        }
        
        // Stage 4.6: Show label before address if symbol exists
        if (hasLabel) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 1.0f, 0.6f, 1.0f));
            ImGui::Text("%s:", sym->name.c_str());
            ImGui::PopStyleColor();
        }
        
        // Build the line text
        ImGui::PushID(i);
        
        // PC marker
        if (isPc) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "\xe2\x86\x92");  // →
        } else {
            ImGui::TextDisabled(" ");
        }
        ImGui::SameLine();
        
        // Breakpoint marker
        if (hasBp) {
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "\xe2\x97\x8f");  // ●
        } else {
            ImGui::TextDisabled(" ");
        }
        ImGui::SameLine();
        
        // Address
        char addrStr[8];
        snprintf(addrStr, sizeof(addrStr), "%04X", lineAddr);
        
        // Bytes column
        char bytesStr[16] = "";
        int bpos = 0;
        for (int b = 0; b < 3; ++b) {
            if (b < instr.length) {
                bpos += snprintf(bytesStr + bpos, sizeof(bytesStr) - bpos, "%02X ",
                                 static_cast<unsigned>(instr.bytes[b]));
            } else {
                bpos += snprintf(bytesStr + bpos, sizeof(bytesStr) - bpos, "   ");
            }
        }
        
        // Stage 4.6: Resolve symbol names in operands
        std::string displayText = instr.text;
        if (instr.hasTarget) {
            std::string symNameStr = symbols.displayName(instr.target);
            if (!symNameStr.empty()) {
                // Replace address with symbol name in the text
                char addrStr[8];
                snprintf(addrStr, sizeof(addrStr), "%04X", instr.target);
                size_t pos = displayText.find(addrStr);
                if (pos != std::string::npos) {
                    displayText.replace(pos, 4, symNameStr);
                }
            }
        }
        
        // Render as selectable line
        char fullLine[128];
        if (hasComment) {
            snprintf(fullLine, sizeof(fullLine), "%s  %s  %s  ; %s",
                     addrStr, bytesStr, displayText.c_str(), sym->comment.c_str());
        } else {
            snprintf(fullLine, sizeof(fullLine), "%s  %s  %s",
                     addrStr, bytesStr, displayText.c_str());
        }
        
        bool clicked = ImGui::Selectable(fullLine, isPc,
                                          ImGuiSelectableFlags_AllowDoubleClick);
        
        // Context menu for breakpoint toggle and symbol operations
        if (ImGui::BeginPopupContextItem("dasmctx")) {
            if (hasBp) {
                if (ImGui::MenuItem("Remove Breakpoint")) {
                    backend.removeBreakpoint(lineAddr);
                }
            } else {
                if (ImGui::MenuItem("Set Breakpoint")) {
                    backend.addBreakpoint(lineAddr);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Go to Memory Inspector")) {
                if (onGoToMemoryInspector) {
                    onGoToMemoryInspector(lineAddr);
                }
            }
            // Stage 4.6: Symbol operations
            ImGui::Separator();
            if (!hasLabel || sym->type != SymbolType::Function) {
                if (ImGui::MenuItem("Define Function")) {
                    editingDefineFunc_ = true;
                    editingAddress_ = lineAddr;
                    editBuffer_[0] = '\0';
                }
            }
            if (!hasLabel || sym->type != SymbolType::Label) {
                if (ImGui::MenuItem("Define Label")) {
                    editingDefineLabel_ = true;
                    editingAddress_ = lineAddr;
                    editBuffer_[0] = '\0';
                }
            }
            if (hasLabel) {
                if (ImGui::MenuItem("Edit Comment")) {
                    editingComment_ = true;
                    editingAddress_ = lineAddr;
                    snprintf(editBuffer_, sizeof(editBuffer_), "%s", sym->comment.c_str());
                }
                if (ImGui::MenuItem("Delete Symbol")) {
                    symbols.removeSymbol(lineAddr);
                    needsRefresh_ = true;
                }
            }
            // Memory region marking
            ImGui::Separator();
            if (ImGui::MenuItem("Mark as Code")) {
                symbols.setRegion(lineAddr, lineAddr, MemoryRegionType::Code);
                needsRefresh_ = true;
            }
            if (ImGui::MenuItem("Mark as Data")) {
                symbols.setRegion(lineAddr, lineAddr, MemoryRegionType::Data);
                needsRefresh_ = true;
            }
            if (ImGui::MenuItem("Mark as Unknown")) {
                symbols.removeRegion(lineAddr);
                needsRefresh_ = true;
            }
            ImGui::EndPopup();
        }
        
        // Double-click → Go to Memory Inspector
        if (clicked && ImGui::IsMouseDoubleClicked(0)) {
            if (onGoToMemoryInspector) {
                onGoToMemoryInspector(lineAddr);
            }
        }
        
        ImGui::PopID();
        
        if (isPc) {
            ImGui::PopStyleColor();
            
            // Auto-scroll to PC line when Follow PC is active
            if (followPc_) {
                ImGui::SetScrollHereY(0.4f);
            }
        }
        
        // Stage 3.9: scroll to gotoAddress target
        if (pendingScroll_ && lineAddr == viewAddress_) {
            ImGui::SetScrollHereY(0.3f);
            pendingScroll_ = false;
        }
        
        displayedCount++;
    }
    
    if (lineCount == 0) {
        ImGui::TextDisabled("(no disassembly)");
    }
    
    // Stage 4.6: Inline editing dialogs
    if (editingDefineFunc_ || editingDefineLabel_ || editingComment_) {
        ImGui::Separator();
        
        if (editingDefineFunc_) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "Define Function at %04X:", editingAddress_);
        } else if (editingDefineLabel_) {
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "Define Label at %04X:", editingAddress_);
        } else {
            ImGui::Text("Edit Comment at %04X:", editingAddress_);
        }
        
        ImGui::SetNextItemWidth(200);
        bool enterPressed = ImGui::InputText("##editinline", editBuffer_, sizeof(editBuffer_),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("OK") || enterPressed) {
            if (editBuffer_[0] != '\0') {
                if (editingDefineFunc_) {
                    symbols.addSymbol(editingAddress_, editBuffer_, SymbolType::Function);
                } else if (editingDefineLabel_) {
                    symbols.addSymbol(editingAddress_, editBuffer_, SymbolType::Label);
                } else if (editingComment_) {
                    symbols.setComment(editingAddress_, editBuffer_);
                }
                needsRefresh_ = true;
            }
            editingDefineFunc_ = false;
            editingDefineLabel_ = false;
            editingComment_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            editingDefineFunc_ = false;
            editingDefineLabel_ = false;
            editingComment_ = false;
        }
    }
    
    ImGui::EndChild();
}
