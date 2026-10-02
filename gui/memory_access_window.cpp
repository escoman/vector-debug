#include "memory_access_window.h"

#include "idebug_backend.h"
#include "disassembler.h"

// Dear ImGui
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// applyMemoryAccessFilter — pure function (Stage 6.25)
//
// Filters the raw RuntimeAccessLog down to entries that should appear in the
// Memory Access window.  Never emits Fetch entries — the whole point of the
// window is to show operand/data accesses, not instruction fetches (which
// would otherwise dominate the display for immediate-operand instructions).
//
// Range filter is applied on the *memory address*, not on PC (§13 исходного
// ТЗ): the range describes the block being observed, PC may be anywhere.
// ---------------------------------------------------------------------------

std::vector<RuntimeAccessLogEntry> applyMemoryAccessFilter(
    const std::vector<RuntimeAccessLogEntry> &log,
    const MemoryAccessFilter &filter,
    size_t maxRows)
{
    std::vector<RuntimeAccessLogEntry> result;

    if (!filter.isValid() || maxRows == 0) return result;

    // Pre-size to what we can hold — never allocate more than maxRows.
    // Full scan is required (log is chronological); we keep the tail.
    result.reserve(std::min(maxRows, log.size()));

    for (const auto &e : log) {
        // Fetch is NEVER shown, in any mode.
        if (e.type == RuntimeAccessLogEntry::Fetch) continue;

        // Mode filter.
        if (filter.mode == MemoryAccessFilter::Read  && e.type != RuntimeAccessLogEntry::Read)  continue;
        if (filter.mode == MemoryAccessFilter::Write && e.type != RuntimeAccessLogEntry::Write) continue;

        // Range filter — inclusive on both ends, applied to memory address.
        if (e.address < filter.from || e.address > filter.to) continue;

        result.push_back(e);
    }

    // Bounded: keep the most-recent maxRows (drop-oldest).  `result` is in
    // chronological order because `log` is (RingBuffer::snapshot guarantees
    // 0 = oldest, size()-1 = newest).
    if (result.size() > maxRows) {
        result.erase(result.begin(),
                     result.begin() + static_cast<long>(result.size() - maxRows));
    }

    return result;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MemoryAccessWindow::MemoryAccessWindow()
{
    formatHexInputs();
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace {

// Parse up to 4 hex digits into uint16_t.  Returns false on any non-hex char
// or empty input.  Leading "0x" is tolerated to match other debugger fields.
bool parseHex16(const char *buf, uint16_t &out)
{
    if (!buf) return false;
    while (*buf == ' ' || *buf == '\t') ++buf;
    if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) buf += 2;

    unsigned value = 0;
    int digits = 0;
    for (; *buf; ++buf) {
        int d;
        if (*buf >= '0' && *buf <= '9')      d = *buf - '0';
        else if (*buf >= 'A' && *buf <= 'F') d = *buf - 'A' + 10;
        else if (*buf >= 'a' && *buf <= 'f') d = *buf - 'a' + 10;
        else return false;
        value = (value << 4) | static_cast<unsigned>(d);
        if (value > 0xFFFFu) return false;
        if (++digits > 4) return false;
    }
    if (digits == 0) return false;
    out = static_cast<uint16_t>(value);
    return true;
}

const char *accessTypeName(RuntimeAccessLogEntry::Type t)
{
    switch (t) {
        case RuntimeAccessLogEntry::Read:  return "READ";
        case RuntimeAccessLogEntry::Write: return "WRITE";
        case RuntimeAccessLogEntry::Fetch: return "FETCH";
    }
    return "?";
}

ImU32 accessTypeColor(RuntimeAccessLogEntry::Type t)
{
    switch (t) {
        case RuntimeAccessLogEntry::Read:  return IM_COL32(120, 200, 255, 255);
        case RuntimeAccessLogEntry::Write: return IM_COL32(255, 160, 100, 255);
        case RuntimeAccessLogEntry::Fetch: return IM_COL32(160, 160, 160, 255);
    }
    return IM_COL32(255, 255, 255, 255);
}

} // namespace

// ---------------------------------------------------------------------------
// Hex buffer <-> filter state
// ---------------------------------------------------------------------------

void MemoryAccessWindow::formatHexInputs()
{
    snprintf(fromBuf_, sizeof(fromBuf_), "%04X", filter_.from);
    snprintf(toBuf_,   sizeof(toBuf_),   "%04X", filter_.to);
}

void MemoryAccessWindow::parseHexInputs()
{
    uint16_t v;
    if (parseHex16(fromBuf_, v)) filter_.from = v;
    if (parseHex16(toBuf_,   v)) filter_.to   = v;
}

// ---------------------------------------------------------------------------
// Refresh / Clear / Disassembly
// ---------------------------------------------------------------------------

void MemoryAccessWindow::doRefresh(IDebugBackend &backend)
{
    auto snap = backend.getRuntimeAccessLog(MemoryAccessWindow::MAX_ROWS * 4);
    totalLogSize_ = snap.size();
    rows_ = applyMemoryAccessFilter(snap, filter_, MemoryAccessWindow::MAX_ROWS);
    lastRefresh_ = Clock::now();
}

void MemoryAccessWindow::refresh(IDebugBackend &backend)
{
    doRefresh(backend);
}

void MemoryAccessWindow::clearLog(IDebugBackend &backend)
{
    backend.clearMemoryAccessLog();
    rows_.clear();
    totalLogSize_ = 0;
    disasmCache_.clear();
}

// Stage 6.25: seed from Memory Map block click.  Cannot call doRefresh()
// here — we have no backend reference outside render().  Set the filter,
// expose the window and let the next frame's render() consume the pending
// flags (which is also where SetNextWindowFocus must happen — BEFORE Begin).
void MemoryAccessWindow::focusOnBlock(uint16_t base)
{
    filter_.from = base;
    filter_.to   = static_cast<uint16_t>(base + 0x00FF);
    formatHexInputs();
    visible_        = true;
    pendingRefresh_ = true;
    pendingFocus_   = true;
}

void MemoryAccessWindow::primeDisassembly(IDebugBackend &backend)
{
    // Build a readByte lambda that peeks via backend.readMemory().
    DisasmReadFn readFn = [&backend](uint16_t addr) -> uint8_t {
        return backend.readMemory(addr);
    };

    for (const auto &e : rows_) {
        if (disasmCache_.find(e.pc) != disasmCache_.end()) continue;
        // Only cache misses trigger a decode; hot-loop PCs collapse to one entry.
        DisassembledInstruction ins = disassemble(e.pc, readFn);
        disasmCache_.emplace(e.pc, std::move(ins.text));
    }
}

const std::string *MemoryAccessWindow::lookupDisassembly(uint16_t pc) const
{
    auto it = disasmCache_.find(pc);
    if (it == disasmCache_.end()) return nullptr;
    return &it->second;
}

// ---------------------------------------------------------------------------
// render() — ImGui panel body
// ---------------------------------------------------------------------------

void MemoryAccessWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    // Stage 6.25: bring the dock tab to front when navigated to from
    // Memory Map.  Must be called BEFORE Begin() (ImGui requirement).
    if (pendingFocus_) {
        ImGui::SetNextWindowFocus();
        pendingFocus_ = false;
    }

    // ROM-load detection: after loadRom() the log clears to size 0.  If we
    // previously had entries and now see 0, drop the disasm cache so stale
    // PCs from a different ROM do not leak across sessions.
    {
        auto probe = backend.getRuntimeAccessLog(1);
        bool isEmptyNow = probe.empty();
        if (wasEmptyLastFrame_ == false && isEmptyNow) {
            disasmCache_.clear();
            rows_.clear();
            totalLogSize_ = 0;
        }
        wasEmptyLastFrame_ = isEmptyNow;
    }

    ImGui::SetNextWindowSize(ImVec2(640, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory Access", &visible_)) {
        // Even when collapsed we still owe the caller a refresh if they
        // clicked "To Memory Access"; do it here so subsequent renders
        // show the correct rows immediately.
        if (pendingRefresh_) {
            doRefresh(backend);
            pendingRefresh_ = false;
        }
        ImGui::End();
        return;
    }

    // Stage 6.25: perform the queued Refresh now that we are rendered.
    if (pendingRefresh_) {
        doRefresh(backend);
        pendingRefresh_ = false;
    }

    // ---- Range selector -------------------------------------------------
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Range:");
    ImGui::SameLine();
    ImGui::Text("From");
    ImGui::SameLine();
    // Four uppercase hex digits need barely more than the label itself;
    // size the field tightly so the row fits without pushing the
    // "Use selected Memory Map range" button to the next line.
    const float kHexInputWidth = ImGui::CalcTextSize("0000").x
                               + ImGui::GetStyle().FramePadding.x * 2.0f
                               + 12.0f;
    ImGui::SetNextItemWidth(kHexInputWidth);
    bool fromEdited = ImGui::InputText("##from", fromBuf_, sizeof(fromBuf_),
                                       ImGuiInputTextFlags_CharsUppercase |
                                       ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::Text("To");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(kHexInputWidth);
    bool toEdited = ImGui::InputText("##to", toBuf_, sizeof(toBuf_),
                                     ImGuiInputTextFlags_CharsUppercase |
                                     ImGuiInputTextFlags_CharsHexadecimal);
    if (fromEdited || toEdited) {
        parseHexInputs();
    }
    if (!filter_.isValid()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(from > to)");
    }

    // ---- Use-Memory-Map-Range button ------------------------------------
    if (getSelectedMapBlock) {
        uint16_t base = 0;
        bool have = getSelectedMapBlock(base);
        ImGui::SameLine();
        ImGui::BeginDisabled(!have);
        if (ImGui::Button("Use selected Memory Map range")) {
            filter_.from = base;
            filter_.to   = static_cast<uint16_t>(base + 0x00FF);
            formatHexInputs();
        }
        ImGui::EndDisabled();
    }

    // ---- Access mode + Live + action buttons -----------------------------
    int modeInt = static_cast<int>(filter_.mode);
    ImGui::Text("Access:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Read",  modeInt == MemoryAccessFilter::Read))  { filter_.mode = MemoryAccessFilter::Read; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Write", modeInt == MemoryAccessFilter::Write)) { filter_.mode = MemoryAccessFilter::Write; }
    ImGui::SameLine();
    if (ImGui::RadioButton("All",   modeInt == MemoryAccessFilter::All))   { filter_.mode = MemoryAccessFilter::All; }

    ImGui::SameLine();
    ImGui::Checkbox("Live", &live_);

    ImGui::SameLine();
    ImGui::BeginDisabled(!filter_.isValid());
    if (ImGui::Button("Refresh")) {
        doRefresh(backend);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        clearLog(backend);
    }

    // ---- Live polling (throttled) ---------------------------------------
    if (live_ && filter_.isValid()) {
        auto now = Clock::now();
        if (now - lastRefresh_ >= REFRESH_INTERVAL) {
            doRefresh(backend);
        }
    }

    // ---- Disassembly priming (only for visible rows) --------------------
    primeDisassembly(backend);

    ImGui::Separator();

    // ---- Table ----------------------------------------------------------
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##memory_access_table", 6, tableFlags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Seq",       ImGuiTableColumnFlags_WidthFixed,   72.0f);
        ImGui::TableSetupColumn("PC",        ImGuiTableColumnFlags_WidthFixed,   56.0f);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Access",    ImGuiTableColumnFlags_WidthFixed,   64.0f);
        ImGui::TableSetupColumn("Address",   ImGuiTableColumnFlags_WidthFixed,   72.0f);
        ImGui::TableSetupColumn("Value",     ImGuiTableColumnFlags_WidthFixed,   52.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const auto &e = rows_[static_cast<size_t>(i)];
            ImGui::TableNextRow();

            // --- Seq ---
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", static_cast<unsigned long long>(e.sequence));

            // --- PC (interactive) ---
            ImGui::TableSetColumnIndex(1);
            char pcBuf[16];
            snprintf(pcBuf, sizeof(pcBuf), "%04X", e.pc);
            ImGui::PushID(("pc" + std::to_string(i)).c_str());
            if (ImGui::Selectable(pcBuf, false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap)) {
                if (ImGui::IsMouseDoubleClicked(0) && onGoToDisassembly) {
                    onGoToDisassembly(e.pc);
                }
            }
            if (ImGui::BeginPopupContextItem("##pc_ctx")) {
                if (ImGui::MenuItem("Go to instruction")) {
                    if (onGoToDisassembly) onGoToDisassembly(e.pc);
                }
                if (ImGui::MenuItem("Go to memory address")) {
                    if (onGoToMemoryInspector) onGoToMemoryInspector(e.address);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Copy PC")) {
                    ImGui::SetClipboardText(pcBuf);
                }
                if (ImGui::MenuItem("Copy address")) {
                    char addrBuf[16];
                    snprintf(addrBuf, sizeof(addrBuf), "%04X", e.address);
                    ImGui::SetClipboardText(addrBuf);
                }
                if (ImGui::MenuItem("Copy instruction")) {
                    if (const std::string *s = lookupDisassembly(e.pc)) {
                        ImGui::SetClipboardText(s->c_str());
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            // --- Instruction (cached disassembly) ---
            ImGui::TableSetColumnIndex(2);
            if (const std::string *s = lookupDisassembly(e.pc)) {
                ImGui::TextUnformatted(s->c_str());
            } else {
                ImGui::TextDisabled("(no disasm)");
            }

            // --- Access ---
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(
                ImVec4(
                    ((accessTypeColor(e.type) >>  0) & 0xFF) / 255.0f,
                    ((accessTypeColor(e.type) >>  8) & 0xFF) / 255.0f,
                    ((accessTypeColor(e.type) >> 16) & 0xFF) / 255.0f,
                    ((accessTypeColor(e.type) >> 24) & 0xFF) / 255.0f),
                "%s", accessTypeName(e.type));

            // --- Address (interactive) ---
            ImGui::TableSetColumnIndex(4);
            char addrBuf[16];
            snprintf(addrBuf, sizeof(addrBuf), "%04X", e.address);
            ImGui::PushID(("addr" + std::to_string(i)).c_str());
            if (ImGui::Selectable(addrBuf, false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap)) {
                if (ImGui::IsMouseDoubleClicked(0) && onGoToMemoryInspector) {
                    onGoToMemoryInspector(e.address);
                }
            }
            ImGui::PopID();

            // --- Value ---
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%02X", e.value);
        }

        ImGui::EndTable();
    }

    // ---- Status line ----------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Showing %zu of %zu matching accesses (log total: %zu)",
                rows_.size(), rows_.size(), totalLogSize_);

    ImGui::End();
}
