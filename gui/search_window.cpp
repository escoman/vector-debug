#include "search_window.h"
#include "idebug_backend.h"
#include "symbol_database.h"

// Dear ImGui
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void SearchWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Search", &visible_)) {
        ImGui::End();
        return;
    }

    // Search input
    ImGui::Text("Search:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(250);
    bool enterPressed = ImGui::InputText("##search", searchBuffer_, sizeof(searchBuffer_),
        ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine();
    if (ImGui::Button("Search") || enterPressed) {
        needsSearch_ = true;
    }

    ImGui::Separator();

    // Perform search if needed
    if (needsSearch_) {
        results_.clear();

        if (searchBuffer_[0] != '\0') {
            auto &symbols = backend.symbolDatabase();
            auto allSymbols = symbols.allSymbols();

            // Convert search string to lowercase for case-insensitive comparison
            std::string searchLower = searchBuffer_;
            std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);

            // Check if search is a hex address
            bool isHexAddress = false;
            uint16_t searchAddr = 0;
            if (searchLower.length() <= 4) {
                unsigned int addr = 0;
                if (sscanf(searchLower.c_str(), "%x", &addr) == 1 && addr <= 0xFFFF) {
                    searchAddr = static_cast<uint16_t>(addr);
                    isHexAddress = true;
                }
            }

            // Search through symbols
            for (const auto &sym : allSymbols) {
                bool match = false;

                // Match by address (if search is hex)
                if (isHexAddress && sym.address == searchAddr) {
                    match = true;
                }

                // Match by name (case-insensitive)
                std::string nameLower = sym.name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                if (nameLower.find(searchLower) != std::string::npos) {
                    match = true;
                }

                // Match by comment (case-insensitive)
                if (!sym.comment.empty()) {
                    std::string commentLower = sym.comment;
                    std::transform(commentLower.begin(), commentLower.end(), commentLower.begin(), ::tolower);
                    if (commentLower.find(searchLower) != std::string::npos) {
                        match = true;
                    }
                }

                if (match) {
                    SearchResult result;
                    result.address = sym.address;
                    result.name = sym.name;

                    if (sym.type == SymbolType::Function) {
                        result.detail = "Function";
                    } else {
                        result.detail = "Label";
                    }

                    if (!sym.comment.empty()) {
                        result.detail += " — " + sym.comment;
                    }

                    results_.push_back(result);
                }
            }

            // Sort results by address
            std::sort(results_.begin(), results_.end(),
                [](const SearchResult &a, const SearchResult &b) {
                    return a.address < b.address;
                });
        }

        needsSearch_ = false;
    }

    // Display results
    if (searchBuffer_[0] == '\0') {
        ImGui::TextDisabled("(enter search term)");
    } else if (results_.empty()) {
        ImGui::TextDisabled("(no results)");
    } else {
        ImGui::Text("Found %zu results:", results_.size());

        ImGui::BeginChild("SearchResults", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);

        for (const auto &result : results_) {
            ImGui::PushID(&result);

            char line[256];
            snprintf(line, sizeof(line), "%04X: %-20s [%s]",
                     result.address, result.name.c_str(), result.detail.c_str());

            if (ImGui::Selectable(line, false)) {
                // Left click: go to disassembly
                if (onGoToDisassembly) {
                    onGoToDisassembly(result.address);
                }
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Go to Disassembly")) {
                    if (onGoToDisassembly) {
                        onGoToDisassembly(result.address);
                    }
                }
                if (ImGui::MenuItem("Go to Memory Inspector")) {
                    if (onGoToMemoryInspector) {
                        onGoToMemoryInspector(result.address);
                    }
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    ImGui::End();
}
