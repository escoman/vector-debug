# Stage 5.1 — Workspace Manager

## Phase 1 — Create WorkspaceManager class

New files: `debugger/gui/workspace_manager.h`, `debugger/gui/workspace_manager.cpp`

**workspace_manager.h** — pure GUI component, no emulator dependencies:

```cpp
#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>

class WorkspaceManager {
public:
    struct WindowVisibility {
        std::string name;
        bool *visiblePtr = nullptr;
    };

    void initialize(const std::string &workspacesDir);
    void shutdown();  // save on exit

    void setWindowVisibilityRefs(std::vector<WindowVisibility> refs);

    // Workspace operations
    void switchWorkspace(const std::string &name);
    void saveCurrentWorkspace();
    void saveWorkspaceAs(const std::string &name);
    void deleteWorkspace(const std::string &name);
    void resetWorkspace();

    // Queries
    const std::string &currentWorkspaceName() const;
    std::vector<std::string> listWorkspaces() const;
    bool isBuiltIn(const std::string &name) const;
    bool isDirty() const;
    void markDirty();

    // Preset layout builders (public for testability)
    using LayoutBuilder = std::function<void(unsigned int dockspaceId)>;
    static void buildDefaultLayout(unsigned int dockspaceId);
    static void buildScreenAnalysisLayout(unsigned int dockspaceId);
    static void buildCpuAnalysisLayout(unsigned int dockspaceId);
    static void buildIoAnalysisLayout(unsigned int dockspaceId);
    static void buildMemoryAnalysisLayout(unsigned int dockspaceId);

private:
    std::string workspacesDir_;
    std::string currentName_ = "Default";
    std::string lastLoadedIni_;   // raw ini from last load (for Reset)
    std::map<std::string, bool*> visibilityRefs_;
    bool dirty_ = false;

    // Autosave debounce
    double lastDirtyTime_ = 0.0;
    static constexpr double kAutosaveDelay = 1.0; // seconds

    static const std::vector<std::string> builtInNames_;
    static const std::map<std::string, LayoutBuilder> presets_;

    // File I/O
    std::string workspaceFilePath(const std::string &name) const;
    bool saveToFile(const std::string &name);
    bool loadFromFile(const std::string &name);
    void writeBuiltinIfMissing(const std::string &name);

    // Visibility serialization
    std::string serializeVisibility() const;
    void deserializeVisibility(const std::string &section);

    // Helpers
    static std::string sanitizeFilename(const std::string &name);
    void ensureDirectory() const;
    void saveCurrentVisibility();  // snapshot before switch
    void restoreVisibility();     // apply after load
    std::map<std::string, bool> savedVisibility_;  // name -> visible
};
```

**Key design decisions:**
- Uses `ImGui::SaveIniSettingsToMemory()` / `ImGui::LoadIniSettingsFromMemory()` for layout
- Workspace file format: ImGui ini + `[Visibility]` section with `WindowName=0/1` per line
- Preset layouts are static builder functions using `DockBuilder` API
- On first run, preset .ini files are generated and written to `workspaces/`
- Debounced autosave: 1 second after last layout change
- `current.txt` stores last used workspace name

**Workspace file format** (`workspaces/Default.ini`):
```
[ImGui]
<ImGui ini settings string>

[Visibility]
CPU Registers=1
Vector Screen=1
...
```

## Phase 2 — Integrate into DebuggerGui

**gui.h changes:**
- Add `#include "workspace_manager.h"`
- Add `WorkspaceManager workspaceManager_;` member
- Add `bool showSaveAsDialog_ = false;`
- Add `char saveAsNameBuffer_[256] = "";`
- Add `bool showDeleteConfirm_ = false;`
- Remove `defaultLayoutBuilt_` and `buildDefaultLayout()` (moved to WorkspaceManager)

**gui.cpp changes:**

1. In `initialize()`: call `workspaceManager_.initialize(workspacesDir)` and register visibility refs
2. In `shutdown()`: call `workspaceManager_.shutdown()` (saves current workspace + last used)
3. In `render()`:
   - Replace `buildDefaultLayout()` call with `workspaceManager_` startup logic
   - After all windows rendered, call `workspaceManager_.markDirty()` if layout changed
   - Autosave check each frame
4. In `renderToolbar()`: add **Workspace** menu between View and controls:
   ```
   Workspace
    [*] Default
        Screen Analysis
        CPU Analysis
        I/O Analysis
        Memory Analysis
    ─────────
    Save
    Save As...
    Delete
    Reset
   ```
5. Save As dialog: simple ImGui popup with name input
6. Delete confirmation: ImGui modal popup

**Visibility refs registration** (in initialize, after creating windows):
```cpp
workspaceManager_.setWindowVisibilityRefs({
    {"CPU Registers", &showCpuRegisters_},
    {"Instruction History", &showInstructionHistory_},
    {"Vector Screen", &vectorScreen_.getVisibleRef()},
    {"Memory Inspector", &memoryInspector_.getVisibleRef()},
    // ... all 14 windows
});
```

## Phase 3 — Preset layouts

Each preset is a static function that uses DockBuilder to create a specific layout. All presets use the same window names as Stage 5.0.

**Default**: same as current `buildDefaultLayout()` — 3-column layout
**Screen Analysis**: large Vector Screen center, Memory Map + Memory Inspector right, Disassembly + Trace left
**CPU Analysis**: large Disassembly center, CPU Registers + Stack left, Trace + Breakpoints right
**I/O Analysis**: large I/O Inspector center-left, CPU Registers + Disassembly right, Trace + Vector Screen bottom
**Memory Analysis**: large Memory Map center, Memory Inspector + Stack left, Disassembly + CPU right

Presets are generated on first startup: WorkspaceManager calls the builder function, then `SaveIniSettingsToMemory()`, and writes to file.

## Phase 4 — Tests

New file: `debugger/tests/test_workspace.cpp`

Tests (no real emulator needed — pure file I/O + ImGui context):
1. Create workspace directory
2. Save workspace to file
3. Load workspace from file
4. Visibility save/restore roundtrip
5. Layout save/restore roundtrip
6. Switch between workspaces
7. Save As creates new workspace
8. Delete user workspace
9. Cannot delete built-in workspace
10. Reset restores preset layout
11. Missing file falls back to Default
12. Corrupted file falls back to Default
13. Persistence across WorkspaceManager restart
14. Sanitized filenames (no `../` escape)
15. Built-in workspace list is correct

CMakeLists.txt: add `test_workspace` target linking against `debugger_core` (for ImGui sources only).

## Phase 5 — Build, test, verify

1. Build `v06c-debugger`, `debugger_core`, `debugger_adapter`
2. Run all existing tests: `test_backend`, `test_board_smoke`, `test_symbol_database`, `test_vram_mapping`
3. Run new `test_workspace`
4. `git diff -- src/` must be empty
5. `git diff --check` must be clean

## Files to create/modify

| File | Action |
|------|--------|
| `debugger/gui/workspace_manager.h` | Create |
| `debugger/gui/workspace_manager.cpp` | Create |
| `debugger/gui/gui.h` | Modify — add WorkspaceManager member, remove old layout code |
| `debugger/gui/gui.cpp` | Modify — integrate WorkspaceManager, add Workspace menu, remove `buildDefaultLayout()` |
| `debugger/tests/test_workspace.cpp` | Create |
| `debugger/CMakeLists.txt` | Modify — add `test_workspace` target |

## Files NOT modified

- `src/*` (emulator sources)
- `debugger/src/*` (debugger core — backend, adapter, target)
