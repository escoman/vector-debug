# Stage 4 — ROM Reverse Engineering & Live Analysis

## Architecture

All new code in `debugger/`. No changes to `src/`.
GUI accesses emulator data only through `DebugBackend` / `BoardWrapper`.

```
debugger/src/
  symbol_database.h/cpp    — symbols, classification, xrefs, call graph
  project_file.h/cpp       — .dbg save/load (JSON)
debugger/gui/
  vector_screen_window.h/cpp   — live Vector-06C screen
  memory_map_window.h/cpp      — 64K memory visualization
  functions_window.h/cpp       — function/label management table
  xrefs_window.h/cpp           — cross-references viewer
  call_graph_window.h/cpp      — call graph tree
  search_window.h/cpp          — global search
```

Backend extended with symbol database, screen snapshot, memory classification.

---

## Sub-stage 4.1 — Symbol Database & Memory Classification

**Files:** `debugger/src/symbol_database.h`, `debugger/src/symbol_database.cpp`

Core data structures (no GUI, no board dependency):

```cpp
enum class SymbolType { Function, Label };
enum class MemoryRegionType { Unknown, Code, Data };

struct DebugSymbol {
    uint16_t address;
    std::string name;
    std::string comment;
    SymbolType type;
};

struct MemoryRegion {
    uint16_t start;
    uint16_t end;
    MemoryRegionType type;
    std::string comment;
};

struct XrefEntry {
    uint16_t from;       // source address
    uint16_t to;         // target address
    // "Code->Code", "Code->Data" etc.
};

class SymbolDatabase {
public:
    // Symbols
    bool addSymbol(uint16_t addr, const std::string &name, SymbolType type);
    bool removeSymbol(uint16_t addr);
    bool renameSymbol(uint16_t addr, const std::string &newName);
    bool setComment(uint16_t addr, const std::string &comment);
    const DebugSymbol *findSymbol(uint16_t addr) const;
    const DebugSymbol *findSymbolByName(const std::string &name) const;
    std::vector<DebugSymbol> allSymbols() const;

    // Auto-name for unknown functions: sub_XXXX
    std::string autoName(uint16_t addr) const;
    std::string displayName(uint16_t addr) const; // user name or auto or ""

    // Memory classification
    bool setRegion(uint16_t start, uint16_t end, MemoryRegionType type);
    bool removeRegion(uint16_t start);
    MemoryRegionType classify(uint16_t addr) const;
    std::vector<MemoryRegion> allRegions() const;

    // Xrefs (populated by analysis)
    void rebuildXrefs(std::function<uint8_t(uint16_t)> readByte);
    std::vector<XrefEntry> xrefsTo(uint16_t addr) const;
    std::vector<XrefEntry> xrefsFrom(uint16_t addr) const;

    // Call graph
    struct CallEdge { uint16_t from; uint16_t to; };
    std::vector<CallEdge> callGraph() const;
};
```

**Tests:** `debugger/tests/test_symbol_database.cpp`
- create/rename/delete function
- create label, comments
- lookup by address, lookup by name
- Unknown->Code, Code->Data, Data->Unknown, range handling
- auto-name sub_XXXX
- xrefs: CALL->function, JMP->label, multiple refs
- call graph: A calls B, A calls B and C, recursive

---

## Sub-stage 4.2 — Backend Integration

**Files:** `debugger/src/backend.h`, `debugger/src/backend.cpp`

Extend `DebugBackend` with:
```cpp
#include "symbol_database.h"

class DebugBackend {
public:
    // Symbol database access (thread-safe via stateMutex_)
    SymbolDatabase &symbolDatabase();
    SymbolDatabaseSnapshot symbolDatabaseSnapshot() const;

    // Screen snapshot (for Vector Screen window)
    struct ScreenSnapshot {
        std::vector<uint32_t> pixels;  // ARGB8888
        int width;
        int height;
    };
    ScreenSnapshot screenSnapshot() const;  // copies from TV::pixels() under mutex

    // Memory activity counters (extend existing MemoryStats)
    // Already have: reads, writes per address
    // Add: execute count per address (from instruction events)
    struct ActivitySnapshot {
        std::vector<uint64_t> executeCount;  // indexed by address (64K)
        std::vector<uint64_t> readCount;
        std::vector<uint64_t> writeCount;
    };
    ActivitySnapshot activitySnapshot() const;
};
```

Thread safety: `stateMutex_` protects SymbolDatabase. ScreenSnapshot taken under a dedicated `screenMutex_` (TV pixels updated by emulation thread).

Activity counters: incremented in `onInstruction` callback (execute) and existing `onMemoryRead/Write` (read/write). Stored as `std::vector<uint64_t>(65536)` — 512KB total, acceptable.

**Tests:** extend `test_backend.cpp`
- symbol database access through backend
- screen snapshot returns valid dimensions
- activity counters increment on step

---

## Sub-stage 4.3 — Vector Screen Window

**Files:** `debugger/gui/vector_screen_window.h`, `debugger/gui/vector_screen_window.cpp`

Display the live Vector-06C framebuffer in an ImGui window.

```cpp
class VectorScreenWindow {
public:
    void render(DebugBackend &backend, BoardWrapper &wrapper);
    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    void requestRefresh() { needsRefresh_ = true; }
private:
    bool visible_ = true;
    bool needsRefresh_ = true;
    bool fitToWindow_ = true;
    bool liveUpdate_ = true;
    int updateIntervalFrames_ = 1;  // update every N frames
    int frameCounter_ = 0;
    GLuint textureId_ = 0;
    std::vector<uint32_t> cachedPixels_;
    int texWidth_ = 0, texHeight_ = 0;
};
```

Implementation:
- Get pixels via `backend.screenSnapshot()` (or directly from `wrapper.tv.pixels()` since we're on GUI thread and can access BoardWrapper)
- Actually, per architecture: GUI goes through backend. But for screen, we need efficient access. The backend's `screenSnapshot()` copies pixels under mutex — acceptable.
- Create ImGui texture from pixel data using `ImGui::Image()`
- Scale modes: Fit to Window (stretch ImGui image to window size), 1:1 (native pixels)
- Live Update toggle: when off, only refresh on Pause/Step
- Save screenshot button: write pixels to PNG file

**CMakeLists.txt:** add new files to GUI_SOURCES.

---

## Sub-stage 4.4 — Memory Map Window

**Files:** `debugger/gui/memory_map_window.h`, `debugger/gui/memory_map_window.cpp`

Visualize the full 64K address space as a 2D bitmap (256x256 pixels, each pixel = 256 bytes).

```cpp
class MemoryMapWindow {
public:
    void render(DebugBackend &backend);
    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }

    std::function<void(uint16_t address)> onGoToMemoryInspector;
    std::function<void(uint16_t address)> onGoToDisassembly;
private:
    bool visible_ = true;
    GLuint mapTextureId_ = 0;
    std::vector<uint32_t> mapPixels_;  // 256x256 ARGB
    uint16_t hoverAddress_ = 0;
    // Context menu state
    uint16_t contextAddress_ = 0;
};
```

Implementation:
- Each pixel represents 256 bytes of address space (65536 / 256 = 256 pixels in 256 rows)
- Color by classification: Unknown=gray, Code=blue, Data=green
- Overlay runtime activity: brighter = more active (use activity snapshot)
- Hover: show address tooltip
- Left click: popup with "Go to Memory Inspector" / "Go to Disassembly"
- Right click: "Mark as Code" / "Mark as Data" / "Mark as Unknown"
- If address belongs to a function: show function name in tooltip
- Backend provides `activitySnapshot()` and `symbolDatabaseSnapshot()`

---

## Sub-stage 4.5 — Functions Window

**Files:** `debugger/gui/functions_window.h`, `debugger/gui/functions_window.cpp`

Table of all user-defined functions and labels.

```cpp
class FunctionsWindow {
public:
    void render(DebugBackend &backend);
    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    void requestRefresh() { needsRefresh_ = true; }

    std::function<void(uint16_t address)> onGoToDisassembly;
    std::function<void(uint16_t address)> onGoToMemoryInspector;
private:
    bool visible_ = true;
    bool needsRefresh_ = true;
    char searchBuffer_[64] = "";
    int sortColumn_ = 0;  // 0=address, 1=name
    bool sortReverse_ = false;
};
```

Columns: Address | Name | Size | Calls | Comment
- Size: for functions, distance to next function/RET or "N/A" for labels
- Calls: count of xrefs to this address
- Sort by address or name
- Search by name (filter)
- Context menu: Rename, Delete, Edit Comment, Go to Disassembly, Go to Memory
- "Define Function" button: dialog with Address + Name + Comment inputs

---

## Sub-stage 4.6 — Disassembly Integration

**Files:** `debugger/gui/disassembly_window.h`, `debugger/gui/disassembly_window.cpp`

Modify existing disassembly window to use symbol database:

1. **Function/label names in operands:**
   - `CALL 7A20` → `CALL DRAW_SPRITE` (if 7A20 has symbol)
   - `JMP 4000` → `JMP PLAYER_X`
   - Use `backend.symbolDatabase().displayName(addr)` for target addresses

2. **Labels before addresses:**
   - Before a line with a symbol, show: `0345 <DRAW_SPRITE>:`
   - For labels: `4020 <PLAYER_X>:`

3. **Comments:**
   - After instruction: `; comment text` (from symbol database)

4. **Context menu additions:**
   - "Define Function" — opens inline input for name
   - "Define Label" — opens inline input for name
   - "Add Comment" — opens inline input
   - "Mark as Code" / "Mark as Data" / "Mark as Unknown"
   - "Find Xrefs to this address"

5. **Breakpoint display with symbol:**
   - Breakpoint marker shows function name if available

---

## Sub-stage 4.7 — Xrefs Window & Call Graph

**Files:** `debugger/gui/xrefs_window.h`, `debugger/gui/xrefs_window.cpp`,
        `debugger/gui/call_graph_window.h`, `debugger/gui/call_graph_window.cpp`

**Xrefs Window:**
- Shows all references to a selected address
- Populated when user selects "Find Xrefs" from disassembly context menu
- Columns: From Address | Instruction | From Function
- Click → Go to Disassembly

**Call Graph Window:**
- Tree view of function call hierarchy
- Root: entry point (address 0000 or first function)
- Expandable nodes: each function shows called functions
- Unknown targets shown as `sub_XXXX`
- Built from `SymbolDatabase::callGraph()`
- Simple ImGui tree (TreeNode/TreePop)

**Automatic Code Discovery (integrated here):**
- Scan all executed addresses (from activity snapshot or instruction history)
- For each CALL/JMP/RST target: if not already a function, suggest "Possible function at XXXX"
- User confirms → creates function
- Button in Functions window: "Auto-discover functions"

---

## Sub-stage 4.8 — Search Window

**Files:** `debugger/gui/search_window.h`, `debugger/gui/search_window.cpp`

Global search across:
- Address (hex)
- Function name
- Label name
- Comment text

```cpp
class SearchWindow {
public:
    void render(DebugBackend &backend);
    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }

    std::function<void(uint16_t address)> onGoToDisassembly;
    std::function<void(uint16_t address)> onGoToMemoryInspector;
private:
    bool visible_ = true;
    char searchBuffer_[128] = "";
    struct SearchResult {
        uint16_t address;
        std::string name;
        std::string detail;  // "Function", "Label", "Comment: ..."
    };
    std::vector<SearchResult> results_;
    bool needsSearch_ = false;
};
```

- Search on Enter or button press
- Results as selectable list
- Click → Go to Disassembly / Memory Inspector

---

## Sub-stage 4.9 — Project File (.dbg)

**Files:** `debugger/src/project_file.h`, `debugger/src/project_file.cpp`

Save/load analysis results alongside ROM:
- `game.rom` → `game.dbg`

Format: simple JSON (hand-written, no external dependency).

```json
{
    "version": 1,
    "rom_path": "/path/to/game.rom",
    "symbols": [
        {"address": "0345", "name": "DRAW_SPRITE", "type": "function", "comment": "Draws sprite"},
        {"address": "4000", "name": "PLAYER_X", "type": "label", "comment": ""}
    ],
    "regions": [
        {"start": "0000", "end": "03FF", "type": "code", "comment": "ROM code"},
        {"start": "4000", "end": "40FF", "type": "data", "comment": "Player state"}
    ],
    "breakpoints": ["0345", "7A20"]
}
```

API:
```cpp
class ProjectFile {
public:
    static bool save(const std::string &path, const SymbolDatabase &db,
                     const std::vector<DebuggerBreakpoint> &breakpoints);
    static bool load(const std::string &path, SymbolDatabase &db,
                     std::vector<DebuggerBreakpoint> &breakpoints);
};
```

GUI integration:
- Menu bar or buttons: "Save Project", "Load Project"
- Auto-save on quit, auto-load on start (if .dbg exists next to ROM)

---

## Sub-stage 4.10 — Execution Trace Integration & Final Cleanup

**Files:** `debugger/gui/execution_trace_window.h/cpp`, `debugger/gui/breakpoints_window.h/cpp`,
        `debugger/gui/gui.h/cpp`, `debugger/gui/main.cpp`

1. **Execution Trace integration (Section 23):**
   - Add "Function" column to trace entries
   - Resolve address → symbol name via `symbolDatabaseSnapshot()`
   - Unknown functions shown as `sub_XXXX`
   - Historical entries use current symbol names (UI resolution, not stored)

2. **Breakpoints integration (Section 22):**
   - Breakpoint list shows function name instead of raw address
   - "Breakpoint at DRAW_SPRITE" instead of "Breakpoint at 7A20"

3. **GUI integration:**
   - Add all new window toggle buttons to toolbar
   - Wire up cross-window navigation callbacks
   - Menu bar with: File (Save/Load Project), View (all windows), Analysis (Auto-discover)

4. **Status bar:** show current function name (if PC is inside a function)

5. **Final cleanup:** remove debug output, verify `git diff -- src/` empty

---

## Test Plan

### Per sub-stage tests:

**4.1:** `test_symbol_database.cpp` — ~20 tests (symbols, classification, xrefs, call graph)
**4.2:** extend `test_backend.cpp` — ~5 tests (backend symbol API, screen snapshot, activity)
**4.3–4.10:** GUI windows tested manually (smoke test) + extend `test_board_smoke.cpp` for screen/activity

### Integration tests (4.10):
- Symbol displayed in disassembly
- Breakpoint displays function name
- Trace resolves current symbol
- Memory Map navigation
- Screen window update
- Save/load project preserves all data

---

## Execution Order

```
4.1 Symbol Database → 4.2 Backend → 4.3 Vector Screen
                                  → 4.4 Memory Map
                                  → 4.5 Functions Window
                                  → 4.6 Disassembly Integration
                                  → 4.7 Xrefs & Call Graph
                                  → 4.8 Search
                                  → 4.9 Project File
                                  → 4.10 Integration & Cleanup
```

4.3–4.10 all depend on 4.1+4.2 and can be developed somewhat independently,
but should be done sequentially for manageable commits.

---

## Key Constraints

1. **No `src/` changes** — all new code in `debugger/`
2. **GUI through DebugBackend only** — except Vector Screen which needs pixel data (via backend snapshot)
3. **Thread safety** — all snapshots via mutex, GUI never holds mutex during render
4. **Performance** — activity counters are simple uint64_t arrays, snapshots are cheap copies
5. **Tests for all new backend logic** — symbol database is fully testable without GUI
