#include "execution_trace_window.h"
#include "disassembler.h"
#include "opcode_info.h"
#include "symbol_database.h"

// Dear ImGui
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void ExecutionTraceWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(600, 350), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Execution Trace", &visible_)) {
        ImGui::End();
        return;
    }

    // Refresh snapshot if needed
    if (needsRefresh_ && !pauseCapture_) {
        uint64_t currentSeq = backend.instructionSequence();
        if (currentSeq != lastSnapshotSeq_ || cachedEntries_.empty()) {
            cachedEntries_ = backend.instructionHistorySnapshot();
            lastSnapshotSeq_ = currentSeq;
        }
        needsRefresh_ = false;
    } else if (needsRefresh_ && pauseCapture_) {
        // Pause Capture: don't fetch new data, just clear the refresh flag
        needsRefresh_ = false;
    }

    // Render toolbar
    renderToolbar(backend);

    ImGui::Separator();

    // Render trace table
    renderTraceTable(backend);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void ExecutionTraceWindow::renderToolbar(IDebugBackend &backend)
{
    // Follow Execution checkbox
    ImGui::Checkbox("Follow Execution", &followExecution_);

    ImGui::SameLine();
    // Pause Capture checkbox
    ImGui::Checkbox("Pause Capture", &pauseCapture_);

    ImGui::SameLine();
    // Clear button
    if (ImGui::Button("Clear")) {
        backend.clearHistory();
        cachedEntries_.clear();
        lastSnapshotSeq_ = 0;
        needsRefresh_ = true;
    }

    ImGui::SameLine();
    // Max entries input
    ImGui::SetNextItemWidth(70);
    if (ImGui::InputInt("Max", &maxEntries_)) {
        if (maxEntries_ < 100) maxEntries_ = 100;
        if (maxEntries_ > 10000) maxEntries_ = 10000;
    }

    ImGui::SameLine();
    // Search field
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("Search", searchBuffer_, sizeof(searchBuffer_));
}

// ---------------------------------------------------------------------------
// Trace table
// ---------------------------------------------------------------------------

void ExecutionTraceWindow::renderTraceTable(IDebugBackend &backend)
{
    if (cachedEntries_.empty()) {
        ImGui::TextDisabled("(no instructions recorded)");
        return;
    }

    // Get symbol database for function name resolution (Stage 4.10)
    auto &symbols = backend.symbolDatabase();

    // Determine the range of entries to display (last maxEntries_)
    size_t totalEntries = cachedEntries_.size();
    size_t startIdx = 0;
    if (static_cast<int>(totalEntries) > maxEntries_) {
        startIdx = totalEntries - static_cast<size_t>(maxEntries_);
    }

    bool hasSearch = (searchBuffer_[0] != '\0');

    // Child window for scrolling
    ImGui::BeginChild("TraceScroll", ImVec2(0, 0), ImGuiChildFlags_None,
                       ImGuiWindowFlags_HorizontalScrollbar);

    // Column header
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                       "%-8s  %-4s  %-10s  %-20s  %-6s  %-15s",
                       "Seq", "PC", "Bytes", "Instruction", "Cycles", "Function");

    int displayedCount = 0;
    int lastDisplayedIndex = -1;

    for (size_t i = startIdx; i < totalEntries; ++i) {
        const auto &ev = cachedEntries_[i];

        // Build disassembly from stored event bytes (not current memory)
        // This is correct even for self-modifying code.
        uint8_t allBytes[3];
        allBytes[0] = ev.opcode;
        allBytes[1] = (ev.length >= 2) ? ev.operandBytes[0] : 0;
        allBytes[2] = (ev.length >= 3) ? ev.operandBytes[1] : 0;

        // Build a readFn that returns stored bytes for this entry's address
        DisasmReadFn entryReadFn = [&allBytes, pc = ev.pcBefore](uint16_t addr) -> uint8_t {
            int offset = static_cast<int>(static_cast<uint16_t>(addr - pc));
            if (offset >= 0 && offset < 3) return allBytes[offset];
            return 0;  // should not happen for valid addresses within the instruction
        };

        DisassembledInstruction instr = disassemble(ev.pcBefore, entryReadFn);

        // Search filter
        if (hasSearch) {
            if (instr.text.find(searchBuffer_) == std::string::npos &&
                std::string(instr.text).find(searchBuffer_) == std::string::npos) {
                continue;
            }
        }

        // Format bytes column
        char bytesStr[12] = "";
        if (ev.length == 1) {
            snprintf(bytesStr, sizeof(bytesStr), "%02X", ev.opcode);
        } else if (ev.length == 2) {
            snprintf(bytesStr, sizeof(bytesStr), "%02X %02X", ev.opcode, ev.operandBytes[0]);
        } else {
            snprintf(bytesStr, sizeof(bytesStr), "%02X %02X %02X",
                     ev.opcode, ev.operandBytes[0], ev.operandBytes[1]);
        }

        // Stage 4.10: Get function name for this address
        std::string funcName = symbols.displayName(ev.pcBefore);
        if (funcName.empty()) {
            funcName = "-";
        }

        // Render as selectable line
        char line[160];
        snprintf(line, sizeof(line), "%-8llu  %04X  %-10s  %-20s  %-6d  %-15s",
                 (unsigned long long)ev.sequence,
                 ev.pcBefore,
                 bytesStr,
                 instr.text.c_str(),
                 ev.cycles,
                 funcName.c_str());

        bool isSelected = false;
        bool clicked = ImGui::Selectable(line, isSelected,
                                          ImGuiSelectableFlags_AllowDoubleClick);

        // Context menu for navigation
        if (ImGui::BeginPopupContextItem("tracectx")) {
            if (ImGui::MenuItem("Go to Disassembly")) {
                if (onGoToDisassembly) {
                    onGoToDisassembly(ev.pcBefore);
                }
            }
            if (ImGui::MenuItem("Go to Memory Inspector")) {
                if (onGoToMemoryInspector) {
                    onGoToMemoryInspector(ev.pcBefore);
                }
            }
            ImGui::EndPopup();
        }

        // Double-click -> Go to Disassembly
        if (clicked && ImGui::IsMouseDoubleClicked(0)) {
            if (onGoToDisassembly) {
                onGoToDisassembly(ev.pcBefore);
            }
        }

        lastDisplayedIndex = displayedCount;
        displayedCount++;
    }

    // Auto-scroll to last entry when Follow Execution is ON
    if (followExecution_ && lastDisplayedIndex >= 0) {
        ImGui::SetScrollHereY(1.0f);
    }

    if (displayedCount == 0) {
        if (hasSearch) {
            ImGui::TextDisabled("(no matching instructions)");
        } else {
            ImGui::TextDisabled("(no instructions recorded)");
        }
    }

    ImGui::EndChild();
}
