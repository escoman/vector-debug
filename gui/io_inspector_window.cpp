#include "io_inspector_window.h"

// Dear ImGui
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// PC resolution — cross-reference instructionSequence with InstructionEvent
// ---------------------------------------------------------------------------

uint16_t IoInspectorWindow::resolvePc(uint64_t instructionSequence) const
{
    // Linear scan of cached instruction events to find matching sequence.
    // The instruction history is typically small (last 10000 entries),
    // and this is only called during GUI rendering (not emulation thread).
    for (const auto &ie : cachedInstrEvents_) {
        if (ie.sequence == instructionSequence) {
            return ie.pcBefore;
        }
    }
    return 0xFFFF;  // unknown — instruction not in cached history
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void IoInspectorWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    ImGui::SetNextWindowSize(ImVec2(550, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("I/O & Hardware Inspector", &visible_)) {
        ImGui::End();
        return;
    }

    // Refresh snapshots:
    // - live_ OFF (default) → don't poll backend, keep last cache
    // - live_ ON            → fetch fresh snapshots every frame
    if (live_) {
        cachedEntries_     = backend.ioHistorySnapshot();
        cachedInstrEvents_ = backend.instructionHistorySnapshot();
        cachedIntEntries_  = backend.interruptEventsSnapshot(
                                static_cast<size_t>(maxEntries_) * 4);
    }
    needsRefresh_ = false;

    // Render toolbar
    renderToolbar(backend);

    ImGui::Separator();

    // Render I/O table
    renderIoTable(backend);

    ImGui::Separator();

    // Render interrupt (RST 7 / INT) log
    renderInterruptLog(backend);

    ImGui::Separator();

    // Render hardware state
    renderHardwareState(backend);

    // Render palette
    renderPalette(backend);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void IoInspectorWindow::renderToolbar(IDebugBackend &backend)
{
    // Follow I/O checkbox
    ImGui::Checkbox("Follow I/O", &followIo_);

    ImGui::SameLine();
    // Live checkbox: when ON, poll backend snapshots every frame
    ImGui::Checkbox("Live", &live_);

    ImGui::SameLine();
    // Clear button — clears I/O and interrupt history
    if (ImGui::Button("Clear")) {
        backend.clearIoHistory();
        backend.clearInterruptEvents();
        cachedEntries_.clear();
        cachedIntEntries_.clear();
        needsRefresh_ = true;
    }

    ImGui::SameLine();
    // Max entries input
    ImGui::SetNextItemWidth(140);
    if (ImGui::InputInt("Max", &maxEntries_)) {
        if (maxEntries_ < 100) maxEntries_ = 100;
        if (maxEntries_ > 10000) maxEntries_ = 10000;
    }

    // Type filter: All / IN / OUT
    ImGui::SameLine();
    if (ImGui::RadioButton("All", typeFilter_ == TypeFilter::All)) {
        typeFilter_ = TypeFilter::All;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("IN", typeFilter_ == TypeFilter::In)) {
        typeFilter_ = TypeFilter::In;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("OUT", typeFilter_ == TypeFilter::Out)) {
        typeFilter_ = TypeFilter::Out;
    }

    // Port filter
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("Port", &portInput_);
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
        if (portInput_ < 0) {
            portFilter_ = -1;  // all ports
        } else if (portInput_ > 255) {
            portFilter_ = -1;  // invalid — keep all ports
        } else {
            portFilter_ = portInput_;
        }
    }
}

// ---------------------------------------------------------------------------
// I/O table
// ---------------------------------------------------------------------------

void IoInspectorWindow::renderIoTable(IDebugBackend &backend)
{
    if (cachedEntries_.empty()) {
        ImGui::TextDisabled("(no I/O events recorded)");
        return;
    }

    // Determine the range of entries to display (last maxEntries_)
    size_t totalEntries = cachedEntries_.size();
    size_t startIdx = 0;
    if (static_cast<int>(totalEntries) > maxEntries_) {
        startIdx = totalEntries - static_cast<size_t>(maxEntries_);
    }

    if (!ImGui::CollapsingHeader("I/O Log")) {
        return;
    }

    // Column header
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                       "%-5s  %-4s  %-4s  %-4s  %-5s",
                       "#", "PC", "Type", "Port", "Value");

    int displayedCount = 0;
    int entryNum = 0;  // simple entry counter for display

    for (size_t i = startIdx; i < totalEntries; ++i) {
        const auto &ev = cachedEntries_[i];
        entryNum++;

        // Type filter
        if (typeFilter_ == TypeFilter::In && ev.type != IoAccessType::In) continue;
        if (typeFilter_ == TypeFilter::Out && ev.type != IoAccessType::Out) continue;

        // Port filter
        if (portFilter_ >= 0 && ev.port != static_cast<uint8_t>(portFilter_)) continue;

        // Resolve PC (returns 0xFFFF if unknown)
        uint16_t pc = resolvePc(ev.instructionSequence);
        bool pcKnown = (pc != 0xFFFF);

        const char *typeStr = (ev.type == IoAccessType::In) ? "IN" : "OUT";

        // Render as selectable line
        char line[128];
        if (pcKnown) {
            snprintf(line, sizeof(line), "%-5d  %04X  %-4s  %02X     %02X",
                     entryNum, pc, typeStr, ev.port, ev.value);
        } else {
            snprintf(line, sizeof(line), "%-5d  --    %-4s  %02X     %02X",
                     entryNum, typeStr, ev.port, ev.value);
        }

        // PushID per row — gives each Selectable+popup a unique ID scope
        ImGui::PushID(static_cast<int>(i));

        bool isSelected = false;
        bool clicked = ImGui::Selectable(line, isSelected,
                                          ImGuiSelectableFlags_AllowDoubleClick);

        // Context menu for navigation (unique per row via PushID)
        if (ImGui::BeginPopupContextItem("ioctx")) {
            if (pcKnown) {
                if (ImGui::MenuItem("Go to Disassembly")) {
                    if (onGoToDisassembly) {
                        onGoToDisassembly(pc);
                    }
                }
                if (ImGui::MenuItem("Go to Memory Inspector")) {
                    if (onGoToMemoryInspector) {
                        onGoToMemoryInspector(pc);
                    }
                }
            } else {
                ImGui::TextDisabled("PC unknown — step mode required");
            }
            ImGui::EndPopup();
        }

        // Double-click -> Go to Disassembly
        if (clicked && ImGui::IsMouseDoubleClicked(0)) {
            if (pcKnown && onGoToDisassembly) {
                onGoToDisassembly(pc);
            }
        }

        ImGui::PopID();

        displayedCount++;
    }

    // Auto-scroll to last entry only when NEW entries appear (not every frame)
    if (followIo_ && displayedCount > 0 && displayedCount != lastDisplayedCount_) {
        ImGui::SetScrollHereY(1.0f);
    }
    lastDisplayedCount_ = displayedCount;

    if (displayedCount == 0) {
        ImGui::TextDisabled("(no matching I/O events)");
    }
}

// ---------------------------------------------------------------------------
// Interrupt (RST 7 / INT) log
// ---------------------------------------------------------------------------

void IoInspectorWindow::renderInterruptLog(IDebugBackend &backend)
{
    (void)backend;

    char title[64];
    snprintf(title, sizeof(title), "Interrupt Log (%zu)###intlog",
             cachedIntEntries_.size());

    if (!ImGui::CollapsingHeader(title)) {
        return;
    }

    if (cachedIntEntries_.empty()) {
        ImGui::TextDisabled("(no hardware interrupts recorded — INT fires during "
                            "frame run, not single-step)");
        lastIntCount_ = -1;
        return;
    }

    // Summary line: hardware RST 7 events are always vector 0x0038; show the
    // wake-vs-active split so a busy-wait vs HLT idling ROM is obvious at a glance.
    size_t wakes = 0;
    for (const auto &ev : cachedIntEntries_) {
        if (ev.wasHalt) wakes++;
    }
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                       "HARDWARE INT via RST 7 -> 0x0038 | total=%zu  wake-from-HLT=%zu  active=%zu",
                       cachedIntEntries_.size(), wakes,
                       cachedIntEntries_.size() - wakes);

    // Display the most recent maxEntries_ events.
    size_t total = cachedIntEntries_.size();
    size_t startIdx = 0;
    if (static_cast<int>(total) > maxEntries_) {
        startIdx = total - static_cast<size_t>(maxEntries_);
    }

    // Column widths are shared verbatim by the header and every data row, all
    // left-justified with a fixed 2-space gap, so each value's left edge lands
    // directly under its label. Addresses are pre-formatted to 4-char hex so
    // they never lose leading zeros and never overflow their field.
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                       "%-7s  %-6s  %-5s  %-6s  %-5s  %-7s  %s",
                       "#", "Frame", "Line", "RetPC", "SP", "Vector", "Site");

    int displayedCount = 0;
    for (size_t i = startIdx; i < total; ++i) {
        const auto &ev = cachedIntEntries_[i];

        // Pre-format the three address columns to fixed 4-char hex strings so
        // they align under their headers (widths match the header format above).
        char retpcStr[8], spStr[8], vecStr[8];
        snprintf(retpcStr, sizeof(retpcStr), "%04X", static_cast<unsigned>(ev.retPc));
        snprintf(spStr,    sizeof(spStr),    "%04X", static_cast<unsigned>(ev.sp));
        snprintf(vecStr,   sizeof(vecStr),   "%04X", static_cast<unsigned>(ev.vector));

        char line[160];
        snprintf(line, sizeof(line),
                 "%-7llu  %-6llu  %-5u  %-6s  %-5s  %-7s  %s",
                 static_cast<unsigned long long>(ev.seq),
                 static_cast<unsigned long long>(ev.frame & 0xFFFF),
                 ev.rasterLine,
                 retpcStr, spStr, vecStr,
                 ev.wasHalt ? "wake-HLT" : "active");

        ImGui::PushID(static_cast<int>(i));

        bool clicked = ImGui::Selectable(line, false,
                                          ImGuiSelectableFlags_AllowDoubleClick);

        if (ImGui::BeginPopupContextItem("intctx")) {
            if (ImGui::MenuItem("Go to Disassembly")) {
                if (onGoToDisassembly) onGoToDisassembly(ev.retPc);
            }
            if (ImGui::MenuItem("Go to Memory Inspector")) {
                if (onGoToMemoryInspector) onGoToMemoryInspector(ev.retPc);
            }
            ImGui::EndPopup();
        }

        if (clicked && ImGui::IsMouseDoubleClicked(0)) {
            if (onGoToDisassembly) onGoToDisassembly(ev.retPc);
        }

        ImGui::PopID();
        displayedCount++;
    }

    // Auto-scroll to the newest interrupt only when new ones appear.
    if (followIo_ && displayedCount > 0 && displayedCount != lastIntCount_) {
        ImGui::SetScrollHereY(1.0f);
    }
    lastIntCount_ = displayedCount;
}

// ---------------------------------------------------------------------------
// Hardware State
// ---------------------------------------------------------------------------

void IoInspectorWindow::renderHardwareState(IDebugBackend &backend)
{
    if (!ImGui::CollapsingHeader("Hardware State")) {
        return;
    }

    CpuState cpu = backend.getCpuState();

    ImGui::Text("CPU:");
    ImGui::Text("  PC: %04X   SP: %04X   IFF: %d", cpu.pc, cpu.sp, cpu.iff ? 1 : 0);
    ImGui::Text("  A=%02X  B=%02X  C=%02X  D=%02X  E=%02X  H=%02X  L=%02X  F=%02X",
                cpu.a, cpu.b, cpu.c, cpu.d, cpu.e, cpu.h, cpu.l, cpu.flags);
    ImGui::Text("  Cycles: %u   EI pending: %d", cpu.cycles, cpu.ei_pending ? 1 : 0);

    ImGui::Spacing();
    ImGui::TextDisabled("(Timer, AY, FD1793, Keyboard: not available via current backend API)");
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

void IoInspectorWindow::renderPalette(IDebugBackend &backend)
{
    if (!ImGui::CollapsingHeader("Palette")) {
        return;
    }

    PaletteSnapshot pal = backend.paletteSnapshot();
    if (pal.count == 0) {
        ImGui::TextDisabled("(palette not available)");
        return;
    }

    for (int i = 0; i < pal.count; ++i) {
        const auto &c = pal.entries[i];

        // Draw color swatch via ImDrawList (guaranteed to render each frame)
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImU32 col = IM_COL32(c.r, c.g, c.b, 255);
        ImGui::GetWindowDrawList()->AddRectFilled(
            p, ImVec2(p.x + 20, p.y + 20), col);
        // Border
        ImGui::GetWindowDrawList()->AddRect(
            p, ImVec2(p.x + 20, p.y + 20),
            IM_COL32(128, 128, 128, 255));
        ImGui::Dummy(ImVec2(20, 20));
        ImGui::SameLine();

        ImGui::Text("[%02X]  #%02X%02X%02X  (%02X)",
                    i, c.r, c.g, c.b, c.rawByte);
    }
}
