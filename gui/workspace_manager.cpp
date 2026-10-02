#include "workspace_manager.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Static data
// ---------------------------------------------------------------------------

const std::vector<std::string> WorkspaceManager::builtInNames_ = {
    "Default",
    "Screen Analysis",
    "CPU Analysis",
    "I/O Analysis",
    "Memory Analysis"
};

const std::map<std::string, WorkspaceManager::LayoutBuilder> WorkspaceManager::presets_ = {
    {"Default",          buildDefaultLayout},
    {"Screen Analysis",  buildScreenAnalysisLayout},
    {"CPU Analysis",     buildCpuAnalysisLayout},
    {"I/O Analysis",     buildIoAnalysisLayout},
    {"Memory Analysis",  buildMemoryAnalysisLayout},
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WorkspaceManager::initialize(const std::string &workspacesDir)
{
    workspacesDir_ = workspacesDir;
    ensureDirectory();

    // Read last used workspace name (don't generate files yet — visibility
    // refs haven't been registered; that happens in setWindowVisibilityRefs)
    std::string currentFile = workspacesDir_ + "/current.txt";
    std::ifstream cf(currentFile);
    if (cf.good()) {
        std::string lastName;
        std::getline(cf, lastName);
        if (!lastName.empty()) {
            std::string path = workspaceFilePath(lastName);
            struct stat st;
            if (stat(path.c_str(), &st) == 0) {
                currentName_ = lastName;
            }
        }
    }
}

void WorkspaceManager::shutdown()
{
    // Do NOT auto-save workspace on exit.
    // Workspace is only saved by explicit user action (Save in menu).

    // Remember last used workspace
    std::string currentFile = workspacesDir_ + "/current.txt";
    std::ofstream cf(currentFile);
    if (cf.good()) {
        cf << currentName_;
    }
}

void WorkspaceManager::setWindowVisibilityRefs(std::vector<WindowVisibility> refs)
{
    // Just store refs as pending — actual registration and workspace loading
    // happen in applyPendingWorkspace() which must be called between frames.
    pendingVisibilityRefs_ = std::move(refs);
    pendingVisibilityRefsSet_ = true;
}

void WorkspaceManager::applyPendingWorkspace()
{
    if (!pendingVisibilityRefsSet_) return;

    // Register visibility refs
    visibilityRefs_.clear();
    for (auto &r : pendingVisibilityRefs_) {
        if (r.visiblePtr) {
            visibilityRefs_[r.name] = r.visiblePtr;
        }
    }
    pendingVisibilityRefs_.clear();
    pendingVisibilityRefsSet_ = false;

    // Don't auto-generate any workspace files on startup.
    // Default.ini is tracked in the repo and managed by the user.
    // Other built-in presets are generated lazily in switchWorkspace().

    // Load the current workspace (last used, or Default)
    if (!loadFromFile(currentName_)) {
        if (currentName_ != "Default") {
            currentName_ = "Default";
            loadFromFile("Default");
        }
    }
}

// ---------------------------------------------------------------------------
// Workspace operations
// ---------------------------------------------------------------------------

void WorkspaceManager::switchWorkspace(const std::string &name)
{
    if (name == currentName_) return;

    // Save current if dirty
    if (dirty_) {
        saveCurrentWorkspace();
    }

    // Generate built-in preset on demand (only when the user actually
    // switches to it, not on every startup)
    if (isBuiltIn(name)) {
        writeBuiltinIfMissing(name);
    }

    // Load target
    if (loadFromFile(name)) {
        currentName_ = name;
        dirty_ = false;
    } else {
        // Fallback to Default
        if (name != "Default") {
            switchWorkspace("Default");
        }
    }
}

void WorkspaceManager::saveCurrentWorkspace()
{
    saveToFile(currentName_);
    dirty_ = false;
}

void WorkspaceManager::saveWorkspaceAs(const std::string &name)
{
    std::string safe = sanitizeFilename(name);
    if (safe.empty()) return;

    saveToFile(safe);
    currentName_ = safe;
    dirty_ = false;
}

void WorkspaceManager::deleteWorkspace(const std::string &name)
{
    if (isBuiltIn(name)) return;

    std::string path = workspaceFilePath(name);
    unlink(path.c_str());

    // If deleted workspace was active, switch to Default
    if (currentName_ == name) {
        currentName_ = "Default";
        loadFromFile("Default");
        dirty_ = false;
    }
}

void WorkspaceManager::resetWorkspace()
{
    // Delete the current workspace file so writeBuiltinIfMissing regenerates it.
    // The actual regeneration happens in processDeferredOps() between frames,
    // because it requires a temporary ImGui context.
    if (isBuiltIn(currentName_)) {
        std::string path = workspaceFilePath(currentName_);
        unlink(path.c_str());
        deferredResetNeeded_ = true;
    } else {
        // User workspace — reset to Default layout
        std::string path = workspaceFilePath("Default");
        unlink(path.c_str());
        deferredResetNeeded_ = true;
    }
    dirty_ = false;
}

void WorkspaceManager::processDeferredOps()
{
    if (deferredResetNeeded_) {
        deferredResetNeeded_ = false;
        std::string target = currentName_;
        if (!isBuiltIn(target)) target = "Default";
        writeBuiltinIfMissing(target);
        loadFromFile(target);
    }
}

// ---------------------------------------------------------------------------
// Autosave (debounced)
// ---------------------------------------------------------------------------

void WorkspaceManager::autosave()
{
    if (!dirty_) return;

    double now = ImGui::GetTime();
    if (lastDirtyTime_ == 0.0) {
        lastDirtyTime_ = now;
        return;
    }

    if (now - lastDirtyTime_ >= kAutosaveDelay) {
        saveCurrentWorkspace();
        lastDirtyTime_ = 0.0;
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

const std::string &WorkspaceManager::currentWorkspaceName() const
{
    return currentName_;
}

std::vector<std::string> WorkspaceManager::listWorkspaces() const
{
    std::vector<std::string> result;
    DIR *dir = opendir(workspacesDir_.c_str());
    if (!dir) return result;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".ini") {
            result.push_back(name.substr(0, name.size() - 4));
        }
    }
    closedir(dir);

    std::sort(result.begin(), result.end());
    return result;
}

bool WorkspaceManager::isBuiltIn(const std::string &name) const
{
    for (const auto &bn : builtInNames_) {
        if (bn == name) return true;
    }
    return false;
}

bool WorkspaceManager::isDirty() const
{
    return dirty_;
}

void WorkspaceManager::markDirty()
{
    if (!dirty_) {
        dirty_ = true;
        lastDirtyTime_ = 0.0;  // reset debounce timer
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

std::string WorkspaceManager::getSetting(const std::string &key,
                                          const std::string &defaultVal) const
{
    auto it = settings_.find(key);
    return (it != settings_.end()) ? it->second : defaultVal;
}

void WorkspaceManager::setSetting(const std::string &key, const std::string &value)
{
    auto it = settings_.find(key);
    if (it != settings_.end() && it->second == value) return;
    settings_[key] = value;
    markDirty();
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

std::string WorkspaceManager::workspaceFilePath(const std::string &name) const
{
    return workspacesDir_ + "/" + sanitizeFilename(name) + ".ini";
}

bool WorkspaceManager::saveToFile(const std::string &name)
{
    ensureDirectory();

    std::string path = workspaceFilePath(name);

    // Save ImGui layout
    const char *ini = ImGui::SaveIniSettingsToMemory();
    std::string iniStr = ini ? ini : "";

    // Strip LastUsed lines — they change every frame and cause unnecessary
    // rewrites (and can corrupt the file with constant disk writes).
    {
        std::string cleaned;
        std::istringstream ss(iniStr);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("LastUsed=") != std::string::npos) continue;
            cleaned += line;
            cleaned += '\n';
        }
        iniStr = cleaned;
    }

    // Build full file content
    std::string content;
    content += "[ImGui]\n";
    content += iniStr;
    if (!iniStr.empty() && iniStr.back() != '\n') {
        content += "\n";
    }
    content += "\n[Visibility]\n";
    content += serializeVisibility();
    content += "\n[Settings]\n";
    for (const auto &pair : settings_) {
        content += pair.first + "=" + pair.second + "\n";
    }

    // Skip write if file content hasn't changed
    {
        std::ifstream existing(path);
        if (existing.good()) {
            std::string oldContent((std::istreambuf_iterator<char>(existing)),
                                    std::istreambuf_iterator<char>());
            if (oldContent == content) {
                return true;  // nothing to write
            }
        }
    }

    std::ofstream file(path);
    if (!file.good()) return false;
    file << content;
    return true;
}

bool WorkspaceManager::loadFromFile(const std::string &name)
{
    std::string path = workspaceFilePath(name);
    std::ifstream file(path);
    if (!file.good()) return false;

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Parse sections
    std::string iniContent;
    std::string visContent;
    std::string settingsContent;

    size_t imguiStart = content.find("[ImGui]");
    size_t visStart = content.find("[Visibility]");
    size_t settingsStart = content.find("[Settings]");

    if (imguiStart != std::string::npos) {
        size_t dataStart = content.find('\n', imguiStart);
        if (dataStart == std::string::npos) dataStart = content.size();
        else dataStart++;

        size_t dataEnd = (visStart != std::string::npos) ? visStart : content.size();
        iniContent = content.substr(dataStart, dataEnd - dataStart);

        // Trim trailing whitespace
        while (!iniContent.empty() &&
               (iniContent.back() == '\n' || iniContent.back() == '\r' ||
                iniContent.back() == ' ')) {
            iniContent.pop_back();
        }
    }

    if (visStart != std::string::npos) {
        size_t dataStart = content.find('\n', visStart);
        if (dataStart == std::string::npos) dataStart = content.size();
        else dataStart++;
        size_t dataEnd = (settingsStart != std::string::npos) ? settingsStart : content.size();
        visContent = content.substr(dataStart, dataEnd - dataStart);
    }

    if (settingsStart != std::string::npos) {
        size_t dataStart = content.find('\n', settingsStart);
        if (dataStart == std::string::npos) dataStart = content.size();
        else dataStart++;
        settingsContent = content.substr(dataStart);
    }

    // Apply ImGui layout
    if (!iniContent.empty()) {
        ImGui::LoadIniSettingsFromMemory(iniContent.c_str(), iniContent.size());
        lastLoadedIni_ = iniContent;
    }

    // Apply visibility
    if (!visContent.empty()) {
        deserializeVisibility(visContent);
    }

    // Parse settings
    if (!settingsContent.empty()) {
        settings_.clear();
        std::istringstream ss(settingsContent);
        std::string line;
        while (std::getline(ss, line)) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                                      line.back() == ' ')) {
                line.pop_back();
            }
            if (line.empty()) continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            settings_[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }

    return true;
}

void WorkspaceManager::writeBuiltinIfMissing(const std::string &name)
{
    std::string path = workspaceFilePath(name);
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return;  // already exists

    auto it = presets_.find(name);
    if (it == presets_.end()) return;

    // Generate preset layout using a temporary ImGui context.
    // This MUST be called between frames (after Render, before NewFrame)
    // to avoid corrupting the main context's frame state.
    ImGuiContext *tempCtx = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime = 1.0f / 60.0f;

    // Build font atlas (required before NewFrame in v1.93+)
    io.Fonts->GetTexDataAsRGBA32(nullptr, nullptr, nullptr, nullptr);

    // Use explicit IDs (auto-ID requires a window context)
    ImGuiID nodeId = 0x10000001;

    // DockBuilderAddNode without DockSpace flag — the flag is set internally
    // when needed. Using DockSpace flag here would call DockSpace() which
    // requires a current window context.
    ImGuiID dockspaceId = ImGui::DockBuilderAddNode(nodeId,
        ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodePos(dockspaceId, ImVec2(0, 0));
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImVec2(1920, 1080));

    it->second(dockspaceId);
    ImGui::DockBuilderFinish(dockspaceId);

    // Complete a full frame cycle in the temp context so that
    // FrameCountEnded == FrameCount (required by NewFrame sanity checks).
    // LoadIniSettingsFromMemory("") marks settings as loaded, preventing
    // the assertion in UpdateSettings() that expects SettingsWindows empty.
    ImGui::LoadIniSettingsFromMemory("");
    ImGui::NewFrame();
    ImGui::EndFrame();
    ImGui::Render();

    const char *ini = ImGui::SaveIniSettingsToMemory();

    ensureDirectory();
    std::ofstream file(path);
    if (file.good()) {
        std::string iniStr = ini ? ini : "";
        file << "[ImGui]\n";
        file << iniStr;
        if (!iniStr.empty() && iniStr.back() != '\n') file << "\n";
        file << "\n[Visibility]\n";
        // Don't write visibility for auto-generated presets — the caller's
        // visibility refs should take precedence over default values.
    }

    ImGui::DestroyContext(tempCtx);
    // CreateContext/DestroyContext automatically save/restore GImGui,
    // so the caller's context is restored with its frame state intact.
}

// ---------------------------------------------------------------------------
// Visibility serialization
// ---------------------------------------------------------------------------

std::string WorkspaceManager::serializeVisibility() const
{
    std::string result;
    for (const auto &pair : visibilityRefs_) {
        result += pair.first + "=" + (pair.second ? std::to_string(*pair.second) : "0") + "\n";
    }
    return result;
}

void WorkspaceManager::deserializeVisibility(const std::string &section)
{
    std::istringstream ss(section);
    std::string line;
    while (std::getline(ss, line)) {
        // Trim
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                                  line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string name = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        auto it = visibilityRefs_.find(name);
        if (it != visibilityRefs_.end() && it->second) {
            *it->second = (val == "1");
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string WorkspaceManager::sanitizeFilename(const std::string &name)
{
    std::string result = name;

    // Strip Windows-style path components (backslash is never valid in names)
    size_t lastSlash = result.find_last_of("\\");
    if (lastSlash != std::string::npos) {
        result = result.substr(lastSlash + 1);
    }

    // Replace all other invalid characters with underscore
    // (including forward slash — "I/O Analysis" → "I_O Analysis")
    for (char &c : result) {
        if (c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }

    // Trim whitespace
    while (!result.empty() && result.front() == ' ') result.erase(result.begin());
    while (!result.empty() && result.back() == ' ') result.pop_back();

    return result;
}

void WorkspaceManager::ensureDirectory() const
{
    mkdir(workspacesDir_.c_str(), 0755);
}

void WorkspaceManager::saveCurrentVisibility()
{
    savedVisibility_.clear();
    for (const auto &pair : visibilityRefs_) {
        savedVisibility_[pair.first] = pair.second ? *pair.second : false;
    }
}

void WorkspaceManager::restoreVisibility()
{
    for (const auto &pair : savedVisibility_) {
        auto it = visibilityRefs_.find(pair.first);
        if (it != visibilityRefs_.end() && it->second) {
            *it->second = pair.second;
        }
    }
}

// ---------------------------------------------------------------------------
// Preset layout builders
// ---------------------------------------------------------------------------

void WorkspaceManager::buildDefaultLayout(unsigned int dockspaceId)
{
    // 3-column: Left (CPU/Stack) | Center (Disasm/Trace) | Right (Memory/History)
    ImGuiID left, centerRight;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.18f, &left, &centerRight);

    ImGuiID center, right;
    ImGui::DockBuilderSplitNode(centerRight, ImGuiDir_Right, 0.30f, &right, &center);

    ImGuiID leftTop, leftBottom;
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.55f, &leftTop, &leftBottom);

    ImGuiID centerTop, centerBottom;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.65f, &centerTop, &centerBottom);

    ImGuiID rightTop, rightBottom;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.55f, &rightTop, &rightBottom);

    ImGui::DockBuilderDockWindow("CPU Registers", leftTop);
    ImGui::DockBuilderDockWindow("Stack View", leftBottom);
    ImGui::DockBuilderDockWindow("Disassembly", centerTop);
    ImGui::DockBuilderDockWindow("Execution Trace", centerBottom);
    ImGui::DockBuilderDockWindow("Memory Inspector", rightTop);
    ImGui::DockBuilderDockWindow("Instruction History", rightBottom);
    ImGui::DockBuilderDockWindow("Vector Screen", rightBottom);
    ImGui::DockBuilderDockWindow("Breakpoints", centerBottom);
    ImGui::DockBuilderDockWindow("Memory Map", rightTop);
    ImGui::DockBuilderDockWindow("I/O & Hardware Inspector", leftBottom);
    ImGui::DockBuilderDockWindow("Functions", centerTop);
    ImGui::DockBuilderDockWindow("Cross References", centerTop);
    ImGui::DockBuilderDockWindow("Call Graph", centerTop);
    ImGui::DockBuilderDockWindow("Search", rightTop);
    ImGui::DockBuilderDockWindow("Keyboard", leftBottom);
}

void WorkspaceManager::buildScreenAnalysisLayout(unsigned int dockspaceId)
{
    // Large Vector Screen center, Memory right, Disasm+Trace left
    ImGuiID left, centerRight;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.25f, &left, &centerRight);

    ImGuiID center, right;
    ImGui::DockBuilderSplitNode(centerRight, ImGuiDir_Right, 0.30f, &right, &center);

    ImGuiID leftTop, leftBottom;
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.50f, &leftTop, &leftBottom);

    ImGuiID rightTop, rightBottom;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.50f, &rightTop, &rightBottom);

    // Left: Disassembly + Execution Trace
    ImGui::DockBuilderDockWindow("Disassembly", leftTop);
    ImGui::DockBuilderDockWindow("Execution Trace", leftBottom);
    ImGui::DockBuilderDockWindow("Functions", leftTop);

    // Center: Vector Screen (large)
    ImGui::DockBuilderDockWindow("Vector Screen", center);

    // Right: Memory Inspector + Memory Map
    ImGui::DockBuilderDockWindow("Memory Inspector", rightTop);
    ImGui::DockBuilderDockWindow("Memory Map", rightTop);
    ImGui::DockBuilderDockWindow("Search", rightTop);

    // Bottom extras
    ImGui::DockBuilderDockWindow("CPU Registers", leftTop);
    ImGui::DockBuilderDockWindow("Stack View", leftBottom);
    ImGui::DockBuilderDockWindow("Breakpoints", leftBottom);
    ImGui::DockBuilderDockWindow("I/O & Hardware Inspector", leftBottom);
    ImGui::DockBuilderDockWindow("Instruction History", leftBottom);
    ImGui::DockBuilderDockWindow("Cross References", leftTop);
    ImGui::DockBuilderDockWindow("Call Graph", leftTop);
}

void WorkspaceManager::buildCpuAnalysisLayout(unsigned int dockspaceId)
{
    // Large Disassembly center, CPU+Stack left, Trace+Breakpoints right
    ImGuiID left, centerRight;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.20f, &left, &centerRight);

    ImGuiID center, right;
    ImGui::DockBuilderSplitNode(centerRight, ImGuiDir_Right, 0.30f, &right, &center);

    ImGuiID leftTop, leftBottom;
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.45f, &leftTop, &leftBottom);

    ImGuiID rightTop, rightBottom;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.50f, &rightTop, &rightBottom);

    // Left: CPU Registers + Stack
    ImGui::DockBuilderDockWindow("CPU Registers", leftTop);
    ImGui::DockBuilderDockWindow("Stack View", leftBottom);
    ImGui::DockBuilderDockWindow("Functions", leftTop);

    // Center: Disassembly (large)
    ImGui::DockBuilderDockWindow("Disassembly", center);
    ImGui::DockBuilderDockWindow("Cross References", center);
    ImGui::DockBuilderDockWindow("Call Graph", center);

    // Right: Execution Trace + Breakpoints
    ImGui::DockBuilderDockWindow("Execution Trace", rightTop);
    ImGui::DockBuilderDockWindow("Breakpoints", rightBottom);
    ImGui::DockBuilderDockWindow("Instruction History", rightTop);

    // Remaining
    ImGui::DockBuilderDockWindow("Memory Inspector", rightBottom);
    ImGui::DockBuilderDockWindow("Memory Map", rightBottom);
    ImGui::DockBuilderDockWindow("Vector Screen", rightBottom);
    ImGui::DockBuilderDockWindow("I/O & Hardware Inspector", leftBottom);
    ImGui::DockBuilderDockWindow("Search", rightBottom);
}

void WorkspaceManager::buildIoAnalysisLayout(unsigned int dockspaceId)
{
    // Large I/O Inspector center-left, CPU+Disasm right, Trace+Screen bottom
    ImGuiID top, bottom;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Down, 0.30f, &bottom, &top);

    ImGuiID topLeft, topRight;
    ImGui::DockBuilderSplitNode(top, ImGuiDir_Right, 0.40f, &topRight, &topLeft);

    ImGuiID bottomLeft, bottomRight;
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.50f, &bottomRight, &bottomLeft);

    // Top-left: I/O & Hardware Inspector (large)
    ImGui::DockBuilderDockWindow("I/O & Hardware Inspector", topLeft);

    // Top-right: CPU Registers + Disassembly
    ImGui::DockBuilderDockWindow("CPU Registers", topRight);
    ImGui::DockBuilderDockWindow("Disassembly", topRight);
    ImGui::DockBuilderDockWindow("Functions", topRight);

    // Bottom-left: Execution Trace + Vector Screen
    ImGui::DockBuilderDockWindow("Execution Trace", bottomLeft);
    ImGui::DockBuilderDockWindow("Vector Screen", bottomLeft);

    // Bottom-right: Stack + Memory + Breakpoints
    ImGui::DockBuilderDockWindow("Stack View", bottomRight);
    ImGui::DockBuilderDockWindow("Memory Inspector", bottomRight);
    ImGui::DockBuilderDockWindow("Memory Map", bottomRight);
    ImGui::DockBuilderDockWindow("Breakpoints", bottomRight);
    ImGui::DockBuilderDockWindow("Instruction History", bottomLeft);
    ImGui::DockBuilderDockWindow("Cross References", topRight);
    ImGui::DockBuilderDockWindow("Call Graph", topRight);
    ImGui::DockBuilderDockWindow("Search", bottomRight);
}

void WorkspaceManager::buildMemoryAnalysisLayout(unsigned int dockspaceId)
{
    // Large Memory Map center, Memory+Stack left, Disasm+CPU right
    ImGuiID left, centerRight;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.25f, &left, &centerRight);

    ImGuiID center, right;
    ImGui::DockBuilderSplitNode(centerRight, ImGuiDir_Right, 0.35f, &right, &center);

    ImGuiID leftTop, leftBottom;
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.55f, &leftTop, &leftBottom);

    ImGuiID rightTop, rightBottom;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.50f, &rightTop, &rightBottom);

    // Left: Memory Inspector + Stack
    ImGui::DockBuilderDockWindow("Memory Inspector", leftTop);
    ImGui::DockBuilderDockWindow("Stack View", leftBottom);
    ImGui::DockBuilderDockWindow("Search", leftTop);

    // Center: Memory Map (large)
    ImGui::DockBuilderDockWindow("Memory Map", center);
    ImGui::DockBuilderDockWindow("Vector Screen", center);

    // Right: Disassembly + CPU Registers
    ImGui::DockBuilderDockWindow("Disassembly", rightTop);
    ImGui::DockBuilderDockWindow("CPU Registers", rightBottom);
    ImGui::DockBuilderDockWindow("Functions", rightTop);

    // Extras
    ImGui::DockBuilderDockWindow("Execution Trace", rightBottom);
    ImGui::DockBuilderDockWindow("Breakpoints", rightBottom);
    ImGui::DockBuilderDockWindow("I/O & Hardware Inspector", leftBottom);
    ImGui::DockBuilderDockWindow("Instruction History", leftBottom);
    ImGui::DockBuilderDockWindow("Cross References", rightTop);
    ImGui::DockBuilderDockWindow("Call Graph", rightTop);
}
