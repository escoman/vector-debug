#include "breakpoints_window.h"
#include "disassembler.h"
#include "symbol_database.h"

// Dear ImGui
#include "imgui.h"

#include <cstdio>
#include <cstring>

void BreakpointsWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;
    
    ImGui::SetNextWindowSize(ImVec2(380, 280), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Breakpoints", &visible_)) {
        ImGui::End();
        return;
    }
    
    // -- Add breakpoint input --
    ImGui::Text("Address:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    bool enterPressed = ImGui::InputText("##bpaddr", addressInput_, sizeof(addressInput_),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Add") || enterPressed) {
        unsigned int addr = 0;
        if (sscanf(addressInput_, "%x", &addr) == 1 && addr <= 0xFFFF) {
            auto result = backend.requestAddBreakpoint(static_cast<uint16_t>(addr));
            if (result.success) {
                addressInput_[0] = '\0';  // clear input
            }
            // If failed, duplicate — silently ignore
        }
    }
    
    ImGui::Separator();
    
    // -- Breakpoint list --
    auto bps = backend.getBreakpoints();
    auto &symbols = backend.symbolDatabase();  // Stage 4.10: for function names
    
    if (bps.empty()) {
        ImGui::TextDisabled("(no breakpoints)");
    } else {
        // Table header
        ImGui::Columns(4, "bplist", true);
        ImGui::Text("Enabled");   ImGui::NextColumn();
        ImGui::Text("Address");   ImGui::NextColumn();
        ImGui::Text("Instruction"); ImGui::NextColumn();
        ImGui::Text("");          ImGui::NextColumn();
        ImGui::Separator();
        
        for (int i = 0; i < static_cast<int>(bps.size()); ++i) {
            const auto &bp = bps[i];
            
            // Enabled checkbox
            bool enabled = bp.enabled;
            ImGui::PushID(i);
            if (ImGui::Checkbox("##en", &enabled)) {
                backend.requestSetBreakpointEnabled(bp.address, enabled);
            }
            ImGui::NextColumn();
            
            // Address (clickable to select) — Stage 4.10: show function name
            char addrStr[32];
            std::string funcName = symbols.displayName(bp.address);
            if (funcName.empty()) {
                snprintf(addrStr, sizeof(addrStr), "%04X", bp.address);
            } else {
                snprintf(addrStr, sizeof(addrStr), "%04X (%s)", bp.address, funcName.c_str());
            }
            if (ImGui::Selectable(addrStr, i == selectedBpIndex_,
                                   ImGuiSelectableFlags_SpanAllColumns)) {
                selectedBpIndex_ = i;
            }
            
            // Stage 3.9: right-click context menu for navigation
            if (ImGui::BeginPopupContextItem()) {
                if (onGoToDisassembly) {
                    if (ImGui::MenuItem("Go to Disassembly")) {
                        onGoToDisassembly(bp.address);
                    }
                }
                if (onGoToMemoryInspector) {
                    if (ImGui::MenuItem("Go to Memory Inspector")) {
                        onGoToMemoryInspector(bp.address);
                    }
                }
                ImGui::EndPopup();
            }
            
            ImGui::NextColumn();
            
            // Disassembly at breakpoint address
            DisasmReadFn readFn = [&backend](uint16_t addr) -> uint8_t {
                return backend.readMemory(addr);
            };
            DisassembledInstruction instr = disassemble(bp.address, readFn);
            ImGui::TextDisabled("%s", instr.text.c_str());
            ImGui::NextColumn();
            
            // Marker column (for future use)
            ImGui::NextColumn();
            
            ImGui::PopID();
        }
        
        ImGui::Columns(1);
    }
    
    ImGui::Separator();
    
    // -- Action buttons --
    bool hasSelection = (selectedBpIndex_ >= 0 && selectedBpIndex_ < static_cast<int>(bps.size()));
    
    if (ImGui::Button("Remove") && hasSelection) {
        backend.requestRemoveBreakpoint(bps[selectedBpIndex_].address);
        selectedBpIndex_ = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear All")) {
        backend.requestClearBreakpoints();
        selectedBpIndex_ = -1;
    }
    
    ImGui::End();
}
