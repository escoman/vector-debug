# Stage 5.0 — Dear ImGui Docking Integration

## Phase 1 — Switch ImGui to Docking Branch

Checkout the `docking` branch of `thirdparty/imgui`. The docking branch contains the same `v1.91.8` base commit (`dbb5eeaad`) plus docking features. The branch HEAD is at `v1.93.0 WIP` which includes incremental changes (ImTextureRef, SDL2 DPI helpers, etc.) but all existing APIs remain backward-compatible.

Steps:
```bash
cd debugger/thirdparty/imgui
git checkout FETCH_HEAD   # docking branch HEAD
```

No CMake changes needed — file structure is identical.

**Risk**: `ImGui::Image()` now takes `ImTextureRef` instead of `ImTextureID`, but `ImTextureRef` has an implicit constructor from `ImTextureID`, so existing code compiles without changes.

## Phase 2 — Enable Docking in gui.cpp

In `debugger/gui/gui.cpp`, method `DebuggerGui::initialize()`, add after `io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard`:
```cpp
io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
```

## Phase 3 — Add Central DockSpace

Replace the current monolithic "Debugger" window approach in `DebuggerGui::render()` with a DockSpace.

**Current approach** (lines 196-252 of `gui.cpp`): A single full-window "Debugger" with hardcoded left/right child panels, toolbar, and status bar.

**New approach**:
1. Create a DockSpace that covers the main viewport using `ImGui::DockSpaceOverViewport()`.
2. Keep the existing toolbar (File menu, Run/Pause/Step/Reset buttons, window toggle buttons) in a separate small window docked at the top, OR as a menu bar within the dockspace host window.
3. Keep the status bar at the bottom.
4. The CPU panel and Instruction History (currently hardcoded as left/right children) become separate dockable windows.
5. All existing debugger windows (Memory Inspector, Stack, Breakpoints, etc.) are already separate `ImGui::Begin/End` windows — they automatically participate in docking.

**Key change**: Remove the monolithic "Debugger" window with its hardcoded `BeginChild("LeftPanel")` / `BeginChild("RightPanel")` layout. Instead:
- Create a "CPU Registers" window (from `renderCpuPanel` + `renderCurrentInstruction`)
- Create an "Instruction History" window (from `renderInstructionHistory`)
- These become dockable like all other windows

**DockSpace host window**: Use `ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode)` to create a full-viewport dockspace.

**Menu bar**: Add a menu bar to the dockspace host window (or use the main viewport's menu). The existing File button + toolbar buttons move into a "Main Toolbar" window docked at the top.

## Phase 4 — Refactor render() Method

Split the monolithic `render()` into window submissions:

1. **Submit DockSpace** (first, before all windows)
2. **Submit toolbar/status** as dockable or fixed overlays
3. **Submit CPU Registers** as a dockable window
4. **Submit Instruction History** as a dockable window
5. **Submit all existing windows** (Memory Inspector, Stack, etc.) — unchanged

The existing `renderCpuPanel()`, `renderCurrentInstruction()`, `renderInstructionHistory()`, `renderControls()`, `renderStatusBar()` methods remain but are called from within appropriate `ImGui::Begin/End` blocks.

## Phase 5 — Initial Window Layout

On first launch (no saved layout), programmatically set up a sensible default layout using `ImGui::DockBuilder*` API or simply rely on ImGui's default docking behavior. Since the spec says "не реализовывать Workspace Manager", we just need windows to be dockable — the user can arrange them.

Optionally, use `ImGui::SetNextWindowDockID()` to give windows an initial dock position on first run.

## Phase 6 — Build and Test

1. **Build debugger_core**: `cmake --build build --target debugger_core`
2. **Build debugger_adapter**: `cmake --build build --target debugger_adapter`
3. **Build v06c-debugger**: `cmake --build build --target v06c-debugger`
4. **Run tests**:
   - `test_backend` (115 tests)
   - `test_board_smoke` (1 test)
   - `test_symbol_database` (22 tests)
   - `test_vram_mapping` (17 tests)
5. **Source isolation**: `git diff -- src/` must be empty
6. **Whitespace check**: `git diff --check` must be clean

## Phase 7 — Manual GUI Verification

Verify:
- GUI launches without crash
- DockSpace is visible and functional
- All 13 windows can be docked, undocked, tabbed, moved, resized
- SDL2 input works (keyboard, mouse)
- OpenGL rendering works (Vector Screen, Memory Map)
- Window resize works
- Existing debugger functions work (Run, Pause, Step, Reset, Open ROM)
- Clean shutdown

## Files Modified

| File | Change |
|------|--------|
| `thirdparty/imgui/*` | Switch to docking branch |
| `debugger/gui/gui.cpp` | Enable docking, add DockSpace, refactor render() |
| `debugger/gui/gui.h` | Add new private methods if needed |

## Files NOT Modified

- `src/*` (emulator sources) — verified by `git diff -- src/`
- `debugger/src/*` (debugger core) — no changes needed
- CMakeLists.txt — no changes needed (ImGui file structure unchanged)
