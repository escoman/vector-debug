#include "xrefs_window.h"
#include "idebug_backend.h"
#include "symbol_database.h"
#include "disassembler.h"

// Dear ImGui
#include "imgui.h"

#include <cstdio>

// ---------------------------------------------------------------------------
// Set target address
// ---------------------------------------------------------------------------

void XrefsWindow::setTargetAddress(uint16_t addr)
{
    targetAddress_ = addr;
    needsRefresh_ = true;
    visible_ = true;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void XrefsWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Cross References", &visible_)) {
        ImGui::End();
        return;
    }

    // Get symbol database
    auto &symbols = backend.symbolDatabase();

    // Header
    ImGui::Text("References to: %04X", targetAddress_);
    const DebugSymbol *targetSym = symbols.findSymbol(targetAddress_);
    if (targetSym) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "(%s)", targetSym->name.c_str());
    }

    ImGui::Separator();

    // Refresh xrefs if needed
    if (needsRefresh_) {
        cachedXrefs_.clear();

        auto xrefs = symbols.xrefsTo(targetAddress_);

        // Read function for disassembly
        DisasmReadFn readFn = [&backend](uint16_t addr) -> uint8_t {
            return backend.readMemory(addr);
        };

        for (const auto &xref : xrefs) {
            XrefEntry entry;
            entry.from = xref.from;

            // Disassemble the instruction at the source address
            auto instr = disassemble(xref.from, readFn);
            entry.instruction = instr.text;

            // Find the function containing this xref
            // Simple approach: look for the nearest function symbol before this address
            std::string funcName;
            auto allSymbols = symbols.allSymbols();
            uint16_t bestFuncAddr = 0xFFFF;
            bool found = false;

            for (const auto &sym : allSymbols) {
                if (sym.type == SymbolType::Function && sym.address <= xref.from) {
                    if (!found || sym.address > bestFuncAddr) {
                        // Check if xref.from is "within" this function
                        // (simple heuristic: within 4KB)
                        if (xref.from - sym.address < 4096) {
                            bestFuncAddr = sym.address;
                            funcName = sym.name;
                            found = true;
                        }
                    }
                }
            }

            entry.fromFunction = found ? funcName : "(unknown)";
            cachedXrefs_.push_back(entry);
        }

        needsRefresh_ = false;
    }

    // Display xrefs
    if (cachedXrefs_.empty()) {
        ImGui::TextDisabled("(no references found)");
    } else {
        ImGui::Text("Found %zu references:", cachedXrefs_.size());

        ImGui::BeginChild("XrefsScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);

        for (const auto &entry : cachedXrefs_) {
            ImGui::PushID(&entry);

            char line[256];
            snprintf(line, sizeof(line), "%04X:  %-30s  from %s",
                     entry.from, entry.instruction.c_str(), entry.fromFunction.c_str());

            if (ImGui::Selectable(line, false)) {
                if (onGoToDisassembly) {
                    onGoToDisassembly(entry.from);
                }
            }

            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    ImGui::End();
}
