#include "functions_window.h"
#include "idebug_backend.h"
#include "symbol_database.h"

// Dear ImGui
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Helper: calculate function size
// ---------------------------------------------------------------------------

static uint16_t calculateFunctionSize(uint16_t addr, const SymbolDatabase &db)
{
    // Find the next function after this one
    auto allSymbols = db.allSymbols();
    uint16_t nextFuncAddr = 0xFFFF;
    bool found = false;

    for (const auto &sym : allSymbols) {
        if (sym.type == SymbolType::Function && sym.address > addr) {
            if (!found || sym.address < nextFuncAddr) {
                nextFuncAddr = sym.address;
                found = true;
            }
        }
    }

    if (found) {
        return nextFuncAddr - addr;
    }
    return 0;  // Unknown size
}

// ---------------------------------------------------------------------------
// Helper: count xrefs to an address
// ---------------------------------------------------------------------------

static int countXrefsTo(uint16_t addr, const SymbolDatabase &db)
{
    auto xrefs = db.xrefsTo(addr);
    return static_cast<int>(xrefs.size());
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void FunctionsWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("MAP-file Info", &visible_)) {
        ImGui::End();
        return;
    }

    // Toolbar
    ImGui::Text("Filter:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::InputText("##search", searchBuffer_, sizeof(searchBuffer_))) {
        needsRefresh_ = true;
    }

    ImGui::Separator();

    // Get symbols
    auto allSymbols = backend.symbolDatabase();
    auto symbols = allSymbols.allSymbols();

    // Filter by search
    std::vector<DebugSymbol> filtered;
    for (const auto &sym : symbols) {
        if (searchBuffer_[0] != '\0') {
            // Case-insensitive search in name
            std::string nameLower = sym.name;
            std::string searchLower = searchBuffer_;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);
            if (nameLower.find(searchLower) == std::string::npos) {
                continue;
            }
        }
        filtered.push_back(sym);
    }

    // Sort
    if (sortColumn_ == 0) {
        // Sort by address
        std::sort(filtered.begin(), filtered.end(),
            [this](const DebugSymbol &a, const DebugSymbol &b) {
                return sortReverse_ ? (a.address > b.address) : (a.address < b.address);
            });
    } else if (sortColumn_ == 1) {
        // Sort by name
        std::sort(filtered.begin(), filtered.end(),
            [this](const DebugSymbol &a, const DebugSymbol &b) {
                return sortReverse_ ? (a.name > b.name) : (a.name < b.name);
            });
    }

    // Symbol list
    ImGui::BeginChild("FuncListScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);

    if (filtered.empty()) {
        ImGui::TextDisabled("(no functions)");
    }

    for (const auto &sym : filtered) {
        ImGui::PushID(sym.address);

        bool hasBp = backend.hasBreakpoint(sym.address);

        // Build row content as a single formatted string
        char rowBuf[256];
        int pos = 0;

        // Breakpoint indicator + address
        if (hasBp) {
            pos += snprintf(rowBuf + pos, sizeof(rowBuf) - pos, "\xe2\x97\x8f ");  // ●
        } else {
            pos += snprintf(rowBuf + pos, sizeof(rowBuf) - pos, "  ");
        }
        pos += snprintf(rowBuf + pos, sizeof(rowBuf) - pos, "%04X  ", sym.address);

        // Name [type]
        const char *typeStr = (sym.type == SymbolType::Function) ? "func" : "label";
        pos += snprintf(rowBuf + pos, sizeof(rowBuf) - pos, "%s [%s]  ", sym.name.c_str(), typeStr);

        // Size
        if (sym.type == SymbolType::Function) {
            uint16_t sz = calculateFunctionSize(sym.address, allSymbols);
            if (sz > 0) {
                pos += snprintf(rowBuf + pos, sizeof(rowBuf) - pos, "sz:%u  ", sz);
            }
        }

        // Calls (xrefs)
        int xrefs = countXrefsTo(sym.address, allSymbols);
        pos += snprintf(rowBuf + pos, sizeof(rowBuf) - pos, "calls:%d  ", xrefs);

        // Source
        if (!sym.sourceFile.empty()) {
            if (sym.sourceLine > 0) {
                pos += snprintf(rowBuf + pos, sizeof(rowBuf) - pos, "%s:%d  ",
                                sym.sourceFile.c_str(), sym.sourceLine);
            } else {
                pos += snprintf(rowBuf + pos, sizeof(rowBuf) - pos, "%s  ", sym.sourceFile.c_str());
            }
        }

        // Comment
        if (!sym.comment.empty()) {
            snprintf(rowBuf + pos, sizeof(rowBuf) - pos, "; %s", sym.comment.c_str());
        }

        // Selectable row
        bool isSelected = (contextAddress_ == sym.address);
        ImGui::Selectable(rowBuf, isSelected);

        // Context menu (readonly — breakpoints and navigation only)
        if (ImGui::BeginPopupContextItem("funcctx")) {
            contextAddress_ = sym.address;
            bool hasBpCtx = backend.hasBreakpoint(contextAddress_);

            if (hasBpCtx) {
                if (ImGui::MenuItem("Remove Breakpoint")) {
                    backend.removeBreakpoint(contextAddress_);
                    needsRefresh_ = true;
                }
            } else {
                if (ImGui::MenuItem("Set Breakpoint")) {
                    backend.addBreakpoint(contextAddress_);
                    needsRefresh_ = true;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Go to Disassembly")) {
                if (onGoToDisassembly) {
                    onGoToDisassembly(contextAddress_);
                }
            }
            if (ImGui::MenuItem("Go to Memory Inspector")) {
                if (onGoToMemoryInspector) {
                    onGoToMemoryInspector(contextAddress_);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    ImGui::End();
}
