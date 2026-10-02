#include "rom_database_window.h"
#include "idebug_backend.h"
#include "rdb_controller.h"
#include "rdb_types.h"

// Dear ImGui
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Type names for combo box
// ---------------------------------------------------------------------------

static const char *RDB_TYPE_NAMES[] = {
    "Function", "Variable", "Data", "Table", "String", "Code", "Label", "Unknown"
};
static const int RDB_TYPE_COUNT = 8;

static RdbObjectType indexToType(int idx)
{
    switch (idx) {
        case 0: return RdbObjectType::Function;
        case 1: return RdbObjectType::Variable;
        case 2: return RdbObjectType::Data;
        case 3: return RdbObjectType::Table;
        case 4: return RdbObjectType::String;
        case 5: return RdbObjectType::Code;
        case 6: return RdbObjectType::Label;
        default: return RdbObjectType::Unknown;
    }
}

// Copy `src` into a fixed GUI text buffer, never splitting a UTF-8 code point.
// snprintf("%s") truncates on a byte boundary, so a Cyrillic comment (2 bytes
// per letter) that overflows the buffer can leave a dangling continuation byte
// at the end and render as garbage. This helper stops at the last whole
// code point instead.
static void copyTextUtf8Safe(char *dst, size_t dstCap, const std::string &src)
{
    if (dstCap == 0) return;
    size_t n = std::min(src.size(), dstCap - 1);
    std::memcpy(dst, src.data(), n);

    if (n < src.size()) {           // we truncated — drop a trailing partial sequence
        size_t i = n;               // i points just past the last copied byte
        while (i > 0 && (static_cast<unsigned char>(dst[i - 1]) & 0xC0) == 0x80)
            --i;                    // walk back over continuation bytes (10xxxxxx)
        if (i > 0) {
            unsigned char lead = static_cast<unsigned char>(dst[i - 1]);
            size_t need;
            if      ((lead & 0x80) == 0x00) need = 1;
            else if ((lead & 0xE0) == 0xC0) need = 2;
            else if ((lead & 0xF0) == 0xE0) need = 3;
            else if ((lead & 0xF8) == 0xF0) need = 4;
            else                            need = 1;   // invalid lead, treat as opaque
            if ((n - i) + 1 < need)         n = i - 1;  // incomplete -> cut before the lead
        }
    }
    dst[n] = '\0';
}

// ---------------------------------------------------------------------------
// Cache refresh
// ---------------------------------------------------------------------------

void RomDatabaseWindow::refreshCache(const RdbController &rdb)
{
    cachedObjects_.clear();
    auto objects = rdb.listObjects();

    for (const auto &obj : objects) {
        CachedObject co;
        co.address = obj.address;
        co.typeStr = rdbObjectTypeToString(obj.type);
        co.name    = obj.name;
        co.sizeStr = obj.hasSize ? std::to_string(obj.size) : "-";
        co.comment = obj.comment;
        cachedObjects_.push_back(std::move(co));
    }

    needsRefresh_ = false;
}

bool RomDatabaseWindow::matchesFilter(const CachedObject &obj) const
{
    if (filterBuffer_[0] == '\0') return true;

    // Case-insensitive search in name
    std::string nameLower = obj.name;
    std::string filterLower = filterBuffer_;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
    if (nameLower.find(filterLower) != std::string::npos) return true;

    // Search by address hex
    char addrBuf[8];
    snprintf(addrBuf, sizeof(addrBuf), "%04X", obj.address);
    if (std::string(addrBuf).find(filterLower) != std::string::npos) return true;

    return false;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void RomDatabaseWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    RdbController &rdb = backend.rdbController();

    // Refresh cache if needed
    if (needsRefresh_) {
        refreshCache(rdb);
    }

    // Window title with dirty indicator
    std::string title = "ROM Database";
    if (rdb.isDirty()) {
        title += " *";
    }

    ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(title.c_str(), &visible_)) {
        ImGui::End();
        return;
    }

    // -- Toolbar -------------------------------------------------------------

    if (ImGui::Button("Add Object")) {
        showAddDialog_ = true;
        addAddrBuffer_[0] = '\0';
        addNameBuffer_[0] = '\0';
        addTypeIndex_ = 0;
    }
    ImGui::SameLine();
    if (rdb.isDirty()) {
        if (ImGui::Button("Save")) {
            rdb.save();
            needsRefresh_ = true;
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Save");
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::Text("Filter:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    if (ImGui::InputText("##filter", filterBuffer_, sizeof(filterBuffer_))) {
        // filter changed — no explicit action needed, rendering uses it
    }

    // ROM identity info
    auto identity = rdb.getRomIdentity();
    if (!identity.file.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu objects, %s)", rdb.objectCount(),
                            rdb.existsOnDisk() ? "on disk" : "in memory");
    }

    ImGui::Separator();

    // -- Add Object Dialog ---------------------------------------------------

    if (showAddDialog_) {
        ImGui::OpenPopup("Add RDB Object");
        showAddDialog_ = false;
    }

    if (ImGui::BeginPopupModal("Add RDB Object", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Address (hex)", addAddrBuffer_, sizeof(addAddrBuffer_));
        ImGui::InputText("Name", addNameBuffer_, sizeof(addNameBuffer_));
        ImGui::Combo("Type", &addTypeIndex_, RDB_TYPE_NAMES, RDB_TYPE_COUNT);

        if (ImGui::Button("Add", ImVec2(120, 0))) {
            unsigned int addr = 0;
            if (sscanf(addAddrBuffer_, "%x", &addr) == 1 && addr <= 0xFFFF && addNameBuffer_[0] != '\0') {
                RdbObject obj;
                obj.address = static_cast<uint16_t>(addr);
                obj.type = indexToType(addTypeIndex_);
                obj.name = addNameBuffer_;
                rdb.addObject(obj);
                needsRefresh_ = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // -- Object Table --------------------------------------------------------

    ImGui::BeginChild("RdbListScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);

    // Table header
    if (ImGui::BeginTable("RdbTable", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch);

        // Manual header row with clickable sort headers
        static const char *colNames[] = {"Address", "Type", "Name", "Size", "Comment"};
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int column = 0; column < 5; column++) {
            ImGui::TableSetColumnIndex(column);
            std::string label = std::string(colNames[column]);
            if (sortColumn_ == column) {
                label += sortReverse_ ? " \xe2\x96\xbc" : " \xe2\x96\xb2"; // ▼ or ▲
            }
            if (ImGui::Selectable(label.c_str(), sortColumn_ == column))
            {
                if (sortColumn_ == column) {
                    sortReverse_ = !sortReverse_;
                } else {
                    sortColumn_ = column;
                    sortReverse_ = false;
                }
            }
        }

        // Filter and sort
        std::vector<const CachedObject*> filtered;
        for (const auto &obj : cachedObjects_) {
            if (matchesFilter(obj)) {
                filtered.push_back(&obj);
            }
        }

        auto sizeToInt = [](const std::string &s) -> int {
            if (s.empty() || s == "-") return 0;
            try { return std::stoi(s); } catch (...) { return 0; }
        };

        auto compareCols = [&sizeToInt](const CachedObject *a, const CachedObject *b, int col) {
            switch (col) {
                case 0: return a->address < b->address;
                case 1: return a->typeStr < b->typeStr;
                case 2: return a->name < b->name;
                case 3: return sizeToInt(a->sizeStr) < sizeToInt(b->sizeStr);
                case 4: return a->comment < b->comment;
                default: return a->address < b->address;
            }
        };

        std::stable_sort(filtered.begin(), filtered.end(),
            [this, &compareCols](const CachedObject *a, const CachedObject *b) {
                // Swap arguments for reverse — preserves strict weak ordering
                // for equal elements (unlike negating the result).
                return sortReverse_
                    ? compareCols(b, a, sortColumn_)
                    : compareCols(a, b, sortColumn_);
            });

        if (filtered.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("(no objects)");
        }

        for (const auto *obj : filtered) {
            ImGui::PushID(obj->address);
            ImGui::TableNextRow();

            // Address column
            ImGui::TableNextColumn();
            char addrBuf[16];
            snprintf(addrBuf, sizeof(addrBuf), "%04X", obj->address);
            bool isSelected = (contextAddress_ == obj->address);
            if (ImGui::Selectable(addrBuf, isSelected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
            {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    openObjectDialog(rdb, obj->address);
                }
            }

            // Context menu
            if (ImGui::BeginPopupContextItem("rdbctx")) {
                contextAddress_ = obj->address;

                if (ImGui::MenuItem("Rename")) {
                    editingName_ = true;
                    editingAddress_ = contextAddress_;
                    snprintf(editNameBuffer_, sizeof(editNameBuffer_), "%s", obj->name.c_str());
                }
                if (ImGui::MenuItem("Edit Comment")) {
                    editingComment_ = true;
                    editingAddress_ = contextAddress_;
                    copyTextUtf8Safe(editCommentBuffer_, sizeof(editCommentBuffer_), obj->comment);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Go to Disassembly")) {
                    if (onGoToDisassembly) onGoToDisassembly(obj->address);
                }
                if (ImGui::MenuItem("Go to Memory")) {
                    if (onGoToMemoryInspector) onGoToMemoryInspector(obj->address);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Set Breakpoint")) {
                    backend.addBreakpoint(obj->address);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove Object")) {
                    rdb.removeObject(obj->address);
                    needsRefresh_ = true;
                }
                ImGui::EndPopup();
            }

            // Type column
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(obj->typeStr.c_str());

            // Name column (inline editing)
            ImGui::TableNextColumn();
            if (editingName_ && editingAddress_ == obj->address) {
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##editname", editNameBuffer_, sizeof(editNameBuffer_),
                        ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    if (editNameBuffer_[0] != '\0') {
                        const RdbObject *existing = rdb.getObject(obj->address);
                        if (existing) {
                            RdbObject updated = *existing;
                            updated.name = editNameBuffer_;
                            rdb.updateObject(updated);
                            needsRefresh_ = true;
                        }
                    }
                    editingName_ = false;
                }
                // Cancel on Escape or focus loss
                if (ImGui::IsItemDeactivated() && !ImGui::IsItemActive()) {
                    editingName_ = false;
                }
            } else {
                ImGui::TextUnformatted(obj->name.c_str());
            }

            // Size column
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(obj->sizeStr.c_str());

            // Comment column (inline editing)
            ImGui::TableNextColumn();
            if (editingComment_ && editingAddress_ == obj->address) {
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##editcomment", editCommentBuffer_, sizeof(editCommentBuffer_),
                        ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    rdb.setComment(obj->address, editCommentBuffer_);
                    needsRefresh_ = true;
                    editingComment_ = false;
                }
                if (ImGui::IsItemDeactivated() && !ImGui::IsItemActive()) {
                    editingComment_ = false;
                }
            } else {
                if (obj->comment.empty()) {
                    ImGui::TextDisabled("-");
                } else {
                    ImGui::TextUnformatted(obj->comment.c_str());
                }
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();

    // -- Object Details Dialog -----------------------------------------------

    renderObjectDialog(rdb);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Object Details Dialog
// ---------------------------------------------------------------------------

static int typeToIndex(RdbObjectType type)
{
    switch (type) {
        case RdbObjectType::Function:  return 0;
        case RdbObjectType::Variable:  return 1;
        case RdbObjectType::Data:      return 2;
        case RdbObjectType::Table:     return 3;
        case RdbObjectType::String:    return 4;
        case RdbObjectType::Code:      return 5;
        case RdbObjectType::Label:     return 6;
        default:                       return 7;
    }
}

void RomDatabaseWindow::openObjectDialog(const RdbController &rdb, uint16_t address)
{
    const RdbObject *obj = rdb.getObject(address);
    if (!obj) return;

    objectDialogAddress_ = address;
    snprintf(objDlgNameBuffer_, sizeof(objDlgNameBuffer_), "%s", obj->name.c_str());
    objDlgTypeIndex_ = typeToIndex(obj->type);
    if (obj->hasSize) {
        objDlgHasSize_ = true;
        snprintf(objDlgSizeBuffer_, sizeof(objDlgSizeBuffer_), "%u", obj->size);
    } else {
        objDlgHasSize_ = false;
        objDlgSizeBuffer_[0] = '\0';
    }
    copyTextUtf8Safe(objDlgCommentBuffer_, sizeof(objDlgCommentBuffer_), obj->comment);
    showObjectDialog_ = true;
}

void RomDatabaseWindow::renderObjectDialog(RdbController &rdb)
{
    if (showObjectDialog_) {
        ImGui::OpenPopup("Object Details");
        showObjectDialog_ = false;
    }

    if (ImGui::BeginPopupModal("Object Details", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const RdbObject *obj = rdb.getObject(objectDialogAddress_);
        if (!obj) {
            ImGui::TextDisabled("(object not found)");
            if (ImGui::Button("Close")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            return;
        }

        // -- Main Fields -----------------------------------------------------

        ImGui::Text("Address:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%04X", obj->address);

        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Name:");
        ImGui::SetNextItemWidth(350);
        ImGui::InputText("##Name", objDlgNameBuffer_, sizeof(objDlgNameBuffer_));
        ImGui::Spacing();

        ImGui::Text("Type:");
        ImGui::SetNextItemWidth(350);
        ImGui::Combo("##Type", &objDlgTypeIndex_, RDB_TYPE_NAMES, RDB_TYPE_COUNT);
        ImGui::Spacing();

        ImGui::Checkbox("Has Size", &objDlgHasSize_);
        if (objDlgHasSize_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::InputText("##Size", objDlgSizeBuffer_, sizeof(objDlgSizeBuffer_));
        }
        ImGui::Spacing();

        ImGui::Text("Comment:");
        ImGui::SetNextItemWidth(350);
        ImGui::InputTextMultiline("##Comment", objDlgCommentBuffer_, sizeof(objDlgCommentBuffer_),
                                   ImVec2(350, 90), ImGuiInputTextFlags_WordWrap);

        // -- Properties ------------------------------------------------------

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (obj->properties.empty()) {
                ImGui::TextDisabled("(no properties)");
            } else {
                ImGui::BeginChild("PropertiesList", ImVec2(350, 120), ImGuiChildFlags_Borders);
                for (const auto &kv : obj->properties) {
                    ImGui::Text("%s:", kv.first.c_str());
                    ImGui::SameLine(120);
                    switch (kv.second.type) {
                        case RdbPropertyValue::Type::String:
                            ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f),
                                               "\"%s\"", kv.second.stringValue.c_str());
                            break;
                        case RdbPropertyValue::Type::Integer:
                            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.6f, 1.0f),
                                               "%lld", (long long)kv.second.intValue);
                            break;
                        case RdbPropertyValue::Type::Boolean:
                            ImGui::TextColored(ImVec4(0.6f, 1.0f, 1.0f, 1.0f),
                                               "%s", kv.second.boolValue ? "true" : "false");
                            break;
                        case RdbPropertyValue::Type::Double:
                            ImGui::TextColored(ImVec4(1.0f, 0.7f, 1.0f, 1.0f),
                                               "%.4f", kv.second.doubleValue);
                            break;
                    }
                }
                ImGui::EndChild();
            }
        }

        // -- Links -----------------------------------------------------------

        if (ImGui::CollapsingHeader("Links", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto links = rdb.getLinks(obj->address);
            if (links.empty()) {
                ImGui::TextDisabled("(no links)");
            } else {
                ImGui::BeginChild("LinksList", ImVec2(350, 100), ImGuiChildFlags_Borders);
                for (uint16_t target : links) {
                    const RdbObject *targetObj = rdb.getObject(target);
                    if (targetObj && !targetObj->name.empty()) {
                        ImGui::Text("%04X  %s", target, targetObj->name.c_str());
                    } else {
                        ImGui::Text("%04X", target);
                    }
                    ImGui::SameLine();
                    char btnId[32];
                    snprintf(btnId, sizeof(btnId), "Go##%04X", target);
                    if (ImGui::SmallButton(btnId)) {
                        if (onGoToDisassembly) onGoToDisassembly(target);
                    }
                }
                ImGui::EndChild();
            }
        }

        // -- Buttons ---------------------------------------------------------

        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            RdbObject updated = *obj;
            updated.name = objDlgNameBuffer_;
            updated.type = indexToType(objDlgTypeIndex_);
            if (objDlgHasSize_) {
                unsigned int sz = 0;
                if (sscanf(objDlgSizeBuffer_, "%u", &sz) == 1) {
                    updated.size = sz;
                    updated.hasSize = true;
                } else {
                    updated.hasSize = false;
                }
            } else {
                updated.hasSize = false;
            }
            updated.comment = objDlgCommentBuffer_;
            rdb.updateObject(updated);
            needsRefresh_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}
